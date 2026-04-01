# ProtoCore String Refactoring — Design Specification
**Date:** 2026-03-31  
**Status:** Approved for implementation  
**Scope:** Complete redesign of the string subsystem in protoCore

---

## 1. Motivation

The current string implementation has three critical problems:

1. **Low storage density** — each Unicode character occupies a full 64-byte Cell (one tuple slot per codepoint), yielding an effective density of ~2-4% for ASCII content.
2. **No UTF-8 support in embedded strings** — inline pointer strings use 7-bit ASCII encoding, leaving multi-byte characters as tree nodes unconditionally.
3. **No non-interned strings** — all strings are symbols (globally interned). This prevents efficient processing of large text content that should be released by the GC after use.

---

## 2. Architecture: Three-Tier Representation

All string values in protoCore are represented by one of three forms. The form is encoded in the tagged pointer and is transparent to API consumers.

```
┌─────────────────────────────────────────────────────────┐
│                    ProtoString API                       │
│  charAt · slice · append · prepend · insert · remove    │
│  iterate · compare · hash · fromUTF8 · toStdString      │
└──────────────┬─────────────────┬───────────────────────┘
               │                 │                │
   ┌───────────▼───┐   ┌─────────▼──┐   ┌────────▼────────┐
   │  Embedded     │   │   Symbol   │   │     String      │
   │  UTF-8        │   │ (interned) │   │  (non-interned) │
   │  in pointer   │   │ AVL + table│   │  AVL, GC only   │
   └───────────────┘   └────────────┘   └─────────────────┘
   up to 6 bytes        identifiers      text processing
   always unique        always unique    identity = object
   no Cell alloc        pointer hash     content compare
```

### 2.1 Pointer Tag Assignment

| Tag | Representation | Equality | Hash for SparseList |
|-----|---------------|----------|---------------------|
| `POINTER_TAG_EMBEDDED_VALUE` + `EMBEDDED_TYPE_INLINE_STRING` | Embedded UTF-8 | Pointer identity O(1) | Pointer value |
| `POINTER_TAG_SYMBOL` *(new)* | Symbol (interned AVL) | Pointer identity O(1) | Pointer value |
| `POINTER_TAG_STRING` *(new)* | String (non-interned AVL) | Content comparison O(K) | N/A — not valid as attribute key without interning |

### 2.2 Result Type Selection

Operations always return the most efficient representation:

```
result fits in ≤ 6 UTF-8 bytes  →  Embedded
otherwise                        →  String (non-interned)

Auto-interning: when used as object attribute key →  String.intern() → Symbol
Operations never produce Symbols directly.
Symbols are only created through the interning path.
```

---

## 3. New Cell Types

### 3.1 StringLeafNode

Stores a contiguous chunk of UTF-8-encoded content.

```
StringLeafNode : Cell (64 bytes total)
┌──────────────────────────────────────┐
│ byte_count   : uint8_t               │  bytes used in payload (0–48)
│ char_count   : uint16_t              │  Unicode codepoints in this leaf
│ flags        : uint8_t               │  bit 0: is_partial (leaf not full)
│ [padding]    : uint8_t[4]            │
│ content_hash : uint64_t              │  FNV-1a/xxHash of utf8_payload
│ utf8_payload : uint8_t[48]           │  UTF-8 encoded content
└──────────────────────────────────────┘
```

The `is_partial` flag marks leaves with available space. Partial leaves are produced by
split operations and allow subsequent insertions without an additional split. Leaves are
merged lazily when two siblings are both below 25% fill.

### 3.2 StringInternalNode

AVL internal node spanning a range of the logical string.

```
StringInternalNode : Cell (64 bytes total)
┌──────────────────────────────────────┐
│ left         : StringNode*           │  left subtree (leaf or internal)
│ right        : StringNode*           │  right subtree
│ total_chars  : uint32_t              │  total codepoints in this subtree
│ left_chars   : uint32_t              │  codepoints in left subtree
│ total_bytes  : uint32_t              │  total UTF-8 bytes in subtree
│ subtree_hash : uint64_t              │  hash_combine(left.hash, right.hash)
│ height       : uint8_t               │  AVL height (1 + max child height)
│ [padding]    : uint8_t[27]           │
└──────────────────────────────────────┘
```

`subtree_hash` is computed in O(1) at construction time from children's hashes,
enabling O(1) inequality fast-path for content comparison at any tree level.

### 3.3 StringNode Union

`StringNode` is the discriminated union of the two cell types, distinguished by a type
flag in the Cell header (consistent with the existing Cell tagging convention).

---

## 4. AVL Operations

### 4.1 Node Construction Invariants

Every `newInternal(ctx, left, right)` call recomputes all derived fields:

```cpp
StringInternalNode* newInternal(ProtoContext* ctx,
                                StringNode* left, StringNode* right) {
    return new(ctx) StringInternalNode {
        .left        = left,
        .right       = right,
        .total_chars = charCount(left) + charCount(right),
        .left_chars  = charCount(left),
        .total_bytes = byteCount(left) + byteCount(right),
        .subtree_hash = hashCombine(subtreeHash(left), subtreeHash(right)),
        .height      = 1 + max(height(left), height(right))
    };
}
```

### 4.2 Rebalancing

Four standard AVL cases, identical in structure to `ProtoListImplementation::rebalance`:

| Balance | Case | Action |
|---------|------|--------|
| > +1, left child balanced/left-heavy | LL | `rotateRight(node)` |
| > +1, left child right-heavy | LR | `rotateLeft(node->left)` then `rotateRight(node)` |
| < −1, right child balanced/right-heavy | RR | `rotateLeft(node)` |
| < −1, right child left-heavy | RL | `rotateRight(node->right)` then `rotateLeft(node)` |

All rotations create new nodes via path copying — no in-place mutation.

### 4.3 Fundamental Primitives

All string operations compose from two primitives:

**`split(node, char_index)`** → `(StringNode*, StringNode*)` — O(log N)

Splits the string at the given character index. Leaf splits find the UTF-8 byte boundary
for `char_index` and produce two partial leaf nodes.

```
split(null, _)               → (null, null)
split(leaf, 0)               → (null, leaf)
split(leaf, leaf.char_count) → (leaf, null)
split(leaf, i)               → (newLeaf(chars[0..i)), newLeaf(chars[i..end)))

split(internal, i):
  if i <= left_chars: (ll, lr) = split(left, i)
                      → (ll, concat(lr, right))
  else:               (rl, rr) = split(right, i - left_chars)
                      → (concat(left, rl), rr)
```

**`concat(a, b)`** → `StringNode*` — O(log|height(a) − height(b)| + 1)

```
concat(null, b) → b
concat(a, null) → a
|height(a) − height(b)| ≤ 1  → newInternal(a, b)
height(a) > height(b) + 1    → rebalance(newInternal(a->left, concat(a->right, b)))
height(b) > height(a) + 1    → rebalance(newInternal(concat(a, b->left), b->right))
```

### 4.4 All Operations as Compositions

| Operation | Implementation | Complexity |
|-----------|---------------|------------|
| `charAt(i)` | Navigate by `left_chars`, no split | O(log N) |
| `append(a, b)` | `concat(a, b)` | O(log N) |
| `prepend(prefix, s)` | `concat(prefix, s)` | O(log N) |
| `slice(s, from, to)` | `split(s, from)` → `split(right, to−from)` | O(log N) |
| `insert(s, i, chars)` | `split(s, i)` → `concat(concat(left, chars), right)` | O(log N) |
| `delete(s, from, to)` | `split(s, from)` → `split(rest, to−from)` → `concat(left, tail)` | O(log N) |

### 4.5 Leaf Merge Policy

Leaf merging is lazy. During `rebalance`, if the current node has both children as
leaf nodes and both are below 25% fill (≤ 12 of 48 bytes used), they are merged into
a single leaf node and the internal node is replaced by that leaf.

Upon interning a `String` → `Symbol`, the tree is normalized: all leaves brought to
≥ 50% fill. This canonical form ensures that two Symbols with the same content
produce identical `subtree_hash` values at the root.

---

## 5. Embedded UTF-8 in Pointer

The existing `EMBEDDED_TYPE_INLINE_STRING` tag is extended from 7-bit ASCII to real UTF-8:

```
bits 63..58  POINTER_TAG_EMBEDDED_VALUE  (6 bits, existing)
bits 57..54  EMBEDDED_TYPE_INLINE_STRING (4 bits, existing)
bits 53..51  byte_count (3 bits, values 0–6)
bits 50..3   utf8_bytes  (48 bits = 6 bytes of UTF-8)
bits 2..0    reserved
```

Maximum embedded capacity: **6 bytes of UTF-8**, covering:
- Up to 6 ASCII characters
- Up to 3 two-byte characters (U+0080–U+07FF: Latin Extended, Arabic, Hebrew, etc.)
- Up to 2 three-byte characters (U+0800–U+FFFF: CJK and most Unicode BMP)
- 1 four-byte supplementary character + 1 ASCII

Migration note: the previous limit was 7 ASCII characters (7 × 7-bit). Strings of
exactly 7 ASCII characters will be stored as single-leaf Symbols after migration.
Behavioral impact is zero — the API is identical.

---

## 6. Symbol Table

### 6.1 Sharded Hash Table

The global `SymbolTable` uses 64 independent shards to replace the single `globalMutex`
used by the current `TupleDictionary`:

```cpp
class SymbolTable {
    static constexpr int SHARD_COUNT = 64;  // power of 2
    struct Shard {
        std::mutex    mutex;
        SymbolBucket* buckets;
        uint32_t      count;
        uint32_t      capacity;
    };
    Shard shards[SHARD_COUNT];

    int shardIndex(uint64_t content_hash) const {
        return (content_hash >> 58) & (SHARD_COUNT - 1);
    }
};
```

Lock contention is reduced by ~64× compared to the current single-mutex design.
Operations on distinct symbols with different shard assignments never contend.

### 6.2 Auto-Interning

Triggered only by `ProtoObject::setAttribute` when the key is a non-interned String:

```
intern(ctx, str):
  if Embedded or Symbol → return str unchanged
  hash  = str->root->subtree_hash      // O(1)
  shard = shardIndex(hash)
  lock shard.mutex
    if found in shard by hash → return existing Symbol
    if hash collision → compareByContent → return if equal
    normalize tree (leaves ≥ 50% fill)
    tag root node as Symbol
    insert into shard
    return new Symbol
  unlock
```

### 6.3 Symbol Lifetime: Mixed Weak/Strong Policy

| Symbol origin | Reference strength | Rationale |
|---|---|---|
| C++ static literals, `fromLiteral()` | Strong | Never collected; live for program duration |
| Auto-interning at runtime | Weak | Collected when no live references remain |

Weak symbols are removed from their shard by the GC sweep phase before the cell is freed,
using the same finalizer notification mechanism as `ProtoExternalPointer`.

---

## 7. Comparison Semantics

### 7.1 For Attribute Lookup (SparseList internal)

The hash key for attribute lookup is always the **pointer value** — for Embedded and
Symbol, the pointer is unique by construction and serves directly as the hash. This is
an internal optimization never exposed to API consumers.

A non-interned String is not valid as a raw attribute key. The `setAttribute` path
interns it first, producing a Symbol, whose pointer is then used.

### 7.2 For User-Visible Comparison

```
compareStrings(a, b):
  a == b (same pointer)                     → equal, O(1)
  both Embedded, different pointer          → different content (unique by def); compare bytes in pointer
  both Symbol, different pointer            → different content (unique by def); compare content for order
  Symbol(x) vs String with same content    → content equal after content comparison
  any case involving non-interned String:
    rootHash(a) != rootHash(b)             → not equal, O(1)
    rootHash(a) == rootHash(b), same size  → compare byte by byte via iterators, O(K)
```

A non-interned String and a Symbol **may have identical content**. Pointer comparison
is never used as a proxy for content equality when either operand is a non-interned String.

---

## 8. Iterators

### 8.1 Codepoint Iterator (Forward and Reverse)

Stack-based traversal, O(1) amortized per codepoint, O(log N) stack space:

- **Forward**: descend left spine, yield leaf bytes left-to-right, backtrack to right children.
- **Reverse**: descend right spine, yield leaf bytes right-to-left (scan backwards for
  UTF-8 lead bytes within each leaf), backtrack to left children.

### 8.2 Indexed Access

`charAt(i)` navigates internal nodes using `left_chars` without constructing an iterator.
O(log N), no heap allocation.

### 8.3 Grapheme Cluster Boundary

protoCore provides codepoint-level iteration as its base primitive. Grapheme cluster
segmentation (Unicode TR#29) requires ~1.7 MB of Unicode tables and is out of scope for
the core library. A `GraphemeClusterIterator` can be layered on top of
`StringCodepointIterator` by consumers (protoJS, protoPython) that require it.

---

## 9. Creation API

```cpp
// From null-terminated UTF-8 (C interop)
static const ProtoString* fromUTF8(ProtoContext*, const char* utf8);

// From sized C++ string literal (static strings, zero-copy candidate)
static const ProtoString* fromLiteral(ProtoContext*, const char* utf8, size_t byte_len);

// From raw buffer — handles UTF-8 boundary between consecutive chunks
struct BufferResult {
    const ProtoString* string;
    uint8_t  remainder[3];       // incomplete UTF-8 bytes at buffer end (0–3)
    uint8_t  remainder_count;
};
static BufferResult fromUTF8Buffer(ProtoContext*,
                                   const uint8_t* buf, size_t len,
                                   const uint8_t* pending, uint8_t pending_count);

// From collections of Unicode codepoints
static const ProtoString* fromCodepointList(ProtoContext*, const ProtoList*);
static const ProtoString* fromCodepointTuple(ProtoContext*, const ProtoTuple*);

// STL interop (copy semantics — protoCore owns its cells)
static const ProtoString* fromStdString(ProtoContext*, const std::string&);
std::string                toStdString  (ProtoContext*) const;
```

`fromUTF8Buffer` detects incomplete UTF-8 sequences at the buffer boundary by scanning
backwards from the end (at most 3 bytes). The remainder is returned to the caller and
prepended to the next chunk. No mutable state is required between calls.

---

## 10. GC Integration

### 10.1 Per-Type GC Interaction

| Type | GC participation |
|------|-----------------|
| Embedded | None — no Cell allocated |
| Symbol (strong) | Reachable via SymbolTable root; never collected |
| Symbol (weak) | Reachable via SymbolTable; GC sweep notifies table before freeing |
| String | Normal tracing via arena; collected when unreachable |

### 10.2 Path Copying and GC Pressure

Each string operation produces O(log N) new nodes along the modified path. The previous
nodes on that path become unreachable immediately and are collected in the next young-gen
sweep of the owning thread's arena.

Immutability provides a structural guarantee: string nodes never hold references to
objects younger than themselves (no forward pointers in generational terms). The write
barrier does not apply to string node fields. This reduces GC overhead compared to
mutable string implementations.

### 10.3 Structural Sharing and Liveness

Subtrees shared between two String objects remain live as long as any String referencing
them is reachable. The GC resolves this correctly via standard reachability tracing.
String AVL trees are DAGs (directed, acyclic) — no cycles are possible, which simplifies
the GC's marking phase.

### 10.4 SymbolTable as GC Root

The SymbolTable is registered as a GC root. During the marking phase its 64 shards are
scanned in parallel (each shard is an independent root). Weak-reference entries are
processed in the sweep phase before cell reclamation.

---

## 11. Files Affected

| File | Change |
|------|--------|
| `headers/proto_internal.h` | Add `StringLeafNode`, `StringInternalNode`, `StringNode`; add `POINTER_TAG_SYMBOL`, `POINTER_TAG_STRING`; extend `EMBEDDED_TYPE_INLINE_STRING` encoding |
| `headers/protoCore.h` | No changes to public API signatures; `ProtoString` opaque handle unchanged |
| `core/ProtoString.cpp` | Full rewrite — replace rope/tuple structure with AVL implementation |
| `core/ProtoSpace.cpp` | Add `SymbolTable` member; register as GC root; replace `TupleDictionary` interning for strings |
| `core/ProtoObject.cpp` | Update `setAttribute` to invoke `intern()` on non-interned String keys |
| `test/test_string.cpp` | Extend with: UTF-8 multi-byte, partial leaves, Symbol vs String equality, buffer boundary, iterator forward/reverse |

ProtoTuple is **not modified** — it remains a separate high-density list structure.
The new string cells (`StringLeafNode`, `StringInternalNode`) are independent types.

---

## 12. Non-Goals

- Mutable strings or a StringBuilder type — incompatible with the parallel GC model.
- Grapheme cluster segmentation in protoCore core — delegated to consumers.
- Zero-copy wrapping of external `std::string` buffers — may be added later via `ProtoExternalBuffer`; copy semantics are used for now.
- Unicode normalization (NFC/NFD) — out of scope for this refactoring.
