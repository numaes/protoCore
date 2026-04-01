# ProtoCore String Refactoring — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the rope/tuple string implementation with a three-tier AVL system: embedded UTF-8 in pointers, interned Symbols (`POINTER_TAG_SYMBOL`), and non-interned GC-managed Strings (`POINTER_TAG_STRING`), all sharing a uniform public API.

**Architecture:** Two new Cell types (`StringLeafNode`, `StringInternalNode`) form a persistent AVL tree keyed by character position. All operations compose from two primitives: `split` and `concat`. A 64-shard `SymbolTable` replaces the single-mutex `TupleDictionary` for string interning. Creation is the only API point that distinguishes Symbol from String.

**Tech Stack:** C++20, CMake 3.x, Google Test (existing in `test/`), FNV-1a hash (implemented inline).

**Design Spec:** `docs/superpowers/specs/2026-03-31-string-refactoring-design.md`

**Build commands:**
```bash
# Full build + test
cd /path/to/protoCore && cmake -B build -S . && cmake --build build -j$(nproc) && ctest --test-dir build -j$(nproc) --output-on-failure

# Build only
cmake --build build -j$(nproc)

# Run string tests only
./build/proto_tests --gtest_filter="StringTest*:SymbolTest*:StringAVLTest*"
```

---

## File Map

| File | Action | Responsibility |
|------|--------|----------------|
| `headers/proto_internal.h` | Modify | Add `StringLeafNode`, `StringInternalNode`; new pointer tags 22-24; new `inlineStringUTF8` bitfield; `SymbolTable` class |
| `core/ProtoString.cpp` | Rewrite | All string operations on AVL tree; `ProtoStringImplementation` now wraps AVL root |
| `core/SymbolTable.cpp` | Create | 64-shard interning table with weak/strong symbol support |
| `headers/protoCore.h` | Modify | Add `createSymbol` overloads; add `fromUTF8`, `fromUTF8Buffer`, `fromCodepointTuple`, `fromStdString`; deprecate `fromUTF8String` |
| `core/ProtoSpace.cpp` | Modify | Add `SymbolTable` member; register as GC root; init cached literals as Symbols |
| `core/ProtoObject.cpp` | Modify | Auto-intern String keys in `setAttribute` / `implSetAttribute` |
| `test/test_string.cpp` | Extend | New tests for UTF-8 multibyte, AVL balance, Symbol vs String equality, buffer boundaries, iterators |

`ProtoTuple` and `TupleDictionary` are **not modified** — string interning no longer uses them.

---

## Task 1: New Pointer Tags and StringLeafNode Declaration

**Files:**
- Modify: `headers/proto_internal.h`

- [ ] **Step 1: Add new pointer tags after line 195 (`POINTER_TAG_RANGE_ITERATOR 21`)**

```cpp
// After: #define POINTER_TAG_RANGE_ITERATOR 21
#define POINTER_TAG_SYMBOL            22   // Interned ProtoStringImplementation
#define POINTER_TAG_STRING_LEAF_NODE  23   // StringLeafNode (internal AVL leaf)
#define POINTER_TAG_STRING_INTERNAL_NODE 24 // StringInternalNode (internal AVL node)
```

- [ ] **Step 2: Replace the `inlineString` bitfield struct in `ProtoObjectPointer` union**

Find and replace the existing `inlineString` struct (around line 166):
```cpp
// REPLACE this:
/** Inline string: length (0..7) in low 3 bits; 7 chars at 7 bits each (bits 3..51). */
struct {
    unsigned long pointer_tag: 6;
    unsigned long embedded_type: 4;
    unsigned long inline_len: 3;
    unsigned long inline_chars: 49;
} inlineString;

// WITH this (keeps backward compat field name, extends to UTF-8):
/** Inline string UTF-8: byte_count (0..6) in bits 10..8; up to 6 UTF-8 bytes in bits 58..11. */
struct {
    unsigned long pointer_tag: 6;
    unsigned long embedded_type: 4;
    unsigned long inline_byte_count: 3;   // 0..6 bytes
    unsigned long inline_utf8_bytes: 48;  // 6 bytes packed (LSB = first byte)
    unsigned long reserved: 3;
} inlineString;
```

- [ ] **Step 3: Update INLINE_STRING constants and helper functions (after line 206)**

```cpp
// REPLACE:
#define INLINE_STRING_MAX_LEN 7
#define INLINE_STRING_LEN_BITS 3
#define INLINE_STRING_CHAR_BITS 7

// WITH:
#define INLINE_STRING_MAX_BYTES 6       // max UTF-8 bytes in embedded pointer
#define INLINE_STRING_BYTE_COUNT_BITS 3

/** Returns byte count of an inline string pointer (0..6). */
inline unsigned long inlineStringByteCount(const ProtoObject* o) {
    ProtoObjectPointer pa{}; pa.oid = o;
    return pa.inlineString.inline_byte_count;
}

/** Reads the i-th byte of an inline string (0-indexed, i < inlineStringByteCount). */
inline uint8_t inlineStringByte(const ProtoObject* o, unsigned long i) {
    ProtoObjectPointer pa{}; pa.oid = o;
    return static_cast<uint8_t>((pa.inlineString.inline_utf8_bytes >> (i * 8)) & 0xFF);
}

/** Creates an inline string from up to 6 UTF-8 bytes. bytes must be valid UTF-8. */
const ProtoObject* createInlineStringUTF8(ProtoContext* context,
                                           const uint8_t* bytes,
                                           uint8_t byte_count);
```

- [ ] **Step 4: Add `StringLeafNode` class declaration in `proto_internal.h`, after `ProtoStringImplementation` forward declaration**

```cpp
class StringLeafNode;
class StringInternalNode;

// ---- StringLeafNode -------------------------------------------------------
// 64-byte Cell. Stores up to 48 bytes of UTF-8 content in one contiguous chunk.
// is_partial (flags bit 0): leaf has available space; produced by split operations.
class StringLeafNode final : public Cell {
public:
    uint8_t  byte_count;                   // bytes used in utf8_payload (0..48)
    uint16_t char_count;                   // Unicode codepoints in this leaf
    uint8_t  flags;                        // bit 0: is_partial
    uint8_t  _pad[4];
    uint64_t content_hash;                 // FNV-1a of utf8_payload[0..byte_count)
    uint8_t  utf8_payload[48];

    static constexpr uint8_t MAX_PAYLOAD        = 48;
    static constexpr uint8_t PARTIAL_THRESHOLD  = 12;  // 25% of MAX_PAYLOAD
    static constexpr uint8_t MERGE_FILL         = 24;  // 50% fill (normalize for Symbol)

    StringLeafNode(ProtoContext* ctx,
                   const uint8_t* bytes, uint8_t byte_cnt,
                   uint16_t char_cnt, bool partial = false);

    bool isPartial() const { return (flags & 1) != 0; }

    // Returns the byte offset of codepoint at char_index within this leaf.
    uint32_t charToByteOffset(uint32_t char_index) const;

    // Reads the codepoint starting at byte_pos.
    uint32_t codepointAt(uint32_t byte_pos) const;

    // Returns the cell as a ProtoObject* with POINTER_TAG_STRING_LEAF_NODE.
    const ProtoObject* asObject() const;

    static const StringLeafNode* fromObject(const ProtoObject* obj);
    static bool isStringLeafNode(const ProtoObject* obj);

private:
    static uint64_t computeHash(const uint8_t* bytes, uint8_t len);
};
static_assert(sizeof(StringLeafNode) == 64, "StringLeafNode must be exactly one 64-byte Cell");
```

- [ ] **Step 5: Add `StringInternalNode` class declaration immediately after `StringLeafNode`**

```cpp
// ---- StringInternalNode ---------------------------------------------------
// 64-byte Cell. AVL internal node for the string tree.
// left_chars is the codepoint count of the LEFT subtree — used for O(log N) navigation.
class StringInternalNode final : public Cell {
public:
    const ProtoObject* left;       // StringLeafNode or StringInternalNode
    const ProtoObject* right;      // StringLeafNode or StringInternalNode
    uint32_t total_chars;          // total codepoints in this subtree
    uint32_t left_chars;           // codepoints in left subtree
    uint32_t total_bytes;          // total UTF-8 bytes in subtree
    uint64_t subtree_hash;         // hash_combine(left.hash, right.hash)
    uint8_t  height;               // AVL height (1 + max child height)
    uint8_t  _pad[3];

    StringInternalNode(ProtoContext* ctx,
                       const ProtoObject* l, const ProtoObject* r);

    const ProtoObject* asObject() const;

    static const StringInternalNode* fromObject(const ProtoObject* obj);
    static bool isStringInternalNode(const ProtoObject* obj);

    // AVL helpers
    static int nodeHeight(const ProtoObject* n);
    static int balance(const ProtoObject* n);
    static uint64_t subtreeHash(const ProtoObject* n);
    static uint32_t charCount(const ProtoObject* n);
    static uint32_t byteCount(const ProtoObject* n);
};
static_assert(sizeof(StringInternalNode) == 64, "StringInternalNode must be exactly one 64-byte Cell");
```

- [ ] **Step 6: Add `ExpectedTag` specializations for the two new cell types, after line 278 in proto_internal.h**

```cpp
template<> struct ExpectedTag<const StringLeafNode> {
    static constexpr unsigned long value = POINTER_TAG_STRING_LEAF_NODE;
};
template<> struct ExpectedTag<StringLeafNode> {
    static constexpr unsigned long value = POINTER_TAG_STRING_LEAF_NODE;
};
template<> struct ExpectedTag<const StringInternalNode> {
    static constexpr unsigned long value = POINTER_TAG_STRING_INTERNAL_NODE;
};
template<> struct ExpectedTag<StringInternalNode> {
    static constexpr unsigned long value = POINTER_TAG_STRING_INTERNAL_NODE;
};
```

Also add the `ProtoStringImplementation` (symbol variant) — `POINTER_TAG_SYMBOL` — to the union in `ProtoObjectPointer`. Find the line with `const ProtoStringImplementation *stringImplementation;` and add:

```cpp
const ProtoStringImplementation *symbolImplementation;  // POINTER_TAG_SYMBOL
```

- [ ] **Step 7: Build to verify header compiles cleanly**

```bash
cmake --build build -j$(nproc) 2>&1 | head -40
```
Expected: zero errors. Warnings about unused variables in unchanged files are acceptable.

- [ ] **Step 8: Commit**

```bash
git add headers/proto_internal.h
git commit -m "feat(strings): add StringLeafNode, StringInternalNode declarations and new pointer tags"
```

---

## Task 2: StringLeafNode Implementation

**Files:**
- Modify: `core/ProtoString.cpp` (add at the top, before existing ProtoStringImplementation code)

- [ ] **Step 1: Write failing tests in `test/test_string.cpp`**

Add a new test suite at the end of the file:

```cpp
#include "proto_internal.h"
using namespace proto;

class StringAVLTest : public ::testing::Test {
protected:
    proto::ProtoSpace space;
    proto::ProtoContext ctx{&space};
    ProtoContext* c = &ctx;
};

TEST_F(StringAVLTest, LeafNodeASCII) {
    const uint8_t bytes[] = {'h','e','l','l','o'};
    auto* leaf = new(c) StringLeafNode(c, bytes, 5, 5, false);
    EXPECT_EQ(leaf->byte_count, 5);
    EXPECT_EQ(leaf->char_count, 5);
    EXPECT_FALSE(leaf->isPartial());
    EXPECT_EQ(leaf->codepointAt(0), (uint32_t)'h');
    EXPECT_EQ(leaf->codepointAt(leaf->charToByteOffset(4)), (uint32_t)'o');
}

TEST_F(StringAVLTest, LeafNodeUTF8TwoByte) {
    // U+00E9 (é) = 0xC3 0xA9
    const uint8_t bytes[] = {0xC3, 0xA9, 'x'};
    auto* leaf = new(c) StringLeafNode(c, bytes, 3, 2, false);
    EXPECT_EQ(leaf->byte_count, 3);
    EXPECT_EQ(leaf->char_count, 2);
    EXPECT_EQ(leaf->codepointAt(0), 0x00E9u);  // é
    EXPECT_EQ(leaf->codepointAt(leaf->charToByteOffset(1)), (uint32_t)'x');
}

TEST_F(StringAVLTest, LeafNodePartialFlag) {
    const uint8_t bytes[] = {'a'};
    auto* leaf = new(c) StringLeafNode(c, bytes, 1, 1, true);
    EXPECT_TRUE(leaf->isPartial());
}

TEST_F(StringAVLTest, LeafNodeHashDiffers) {
    const uint8_t b1[] = {'a','b'};
    const uint8_t b2[] = {'a','c'};
    auto* l1 = new(c) StringLeafNode(c, b1, 2, 2);
    auto* l2 = new(c) StringLeafNode(c, b2, 2, 2);
    EXPECT_NE(l1->content_hash, l2->content_hash);
}
```

- [ ] **Step 2: Run tests to verify they fail**

```bash
cmake --build build -j$(nproc) && ./build/proto_tests --gtest_filter="StringAVLTest*" 2>&1 | tail -20
```
Expected: compilation error — `StringLeafNode` constructor undefined.

- [ ] **Step 3: Implement `StringLeafNode` at the top of `core/ProtoString.cpp`**

Add after existing includes:

```cpp
// ===== UTF-8 utilities =====================================================

namespace proto {

static uint32_t utf8SeqLen(uint8_t lead) {
    if (lead < 0x80) return 1;
    if (lead < 0xE0) return 2;
    if (lead < 0xF0) return 3;
    return 4;
}

static uint32_t decodeCodepoint(const uint8_t* p) {
    uint8_t lead = p[0];
    if (lead < 0x80) return lead;
    if (lead < 0xE0) return ((lead & 0x1F) << 6)  | (p[1] & 0x3F);
    if (lead < 0xF0) return ((lead & 0x0F) << 12) | ((p[1] & 0x3F) << 6)  | (p[2] & 0x3F);
    return               ((lead & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
}

static uint64_t fnv1a(const uint8_t* data, size_t len) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; ++i)
        h = (h ^ data[i]) * 1099511628211ULL;
    return h;
}

static uint64_t hashCombine(uint64_t a, uint64_t b) {
    return a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2));
}

// ===== StringLeafNode ======================================================

StringLeafNode::StringLeafNode(ProtoContext* ctx,
                               const uint8_t* bytes, uint8_t byte_cnt,
                               uint16_t char_cnt, bool partial)
    : byte_count(byte_cnt)
    , char_count(char_cnt)
    , flags(partial ? 1 : 0)
    , _pad{}
    , content_hash(computeHash(bytes, byte_cnt))
{
    std::memcpy(utf8_payload, bytes, byte_cnt);
}

uint64_t StringLeafNode::computeHash(const uint8_t* bytes, uint8_t len) {
    return fnv1a(bytes, len);
}

uint32_t StringLeafNode::charToByteOffset(uint32_t char_index) const {
    uint32_t byte_pos = 0, char_pos = 0;
    while (char_pos < char_index && byte_pos < byte_count) {
        byte_pos += utf8SeqLen(utf8_payload[byte_pos]);
        ++char_pos;
    }
    return byte_pos;
}

uint32_t StringLeafNode::codepointAt(uint32_t byte_pos) const {
    return decodeCodepoint(utf8_payload + byte_pos);
}

const ProtoObject* StringLeafNode::asObject() const {
    ProtoObjectPointer pa{};
    pa.oid = reinterpret_cast<const ProtoObject*>(this);
    // Low 6 bits of a 64-byte-aligned pointer are always 0; inject tag
    pa.op.pointer_tag = POINTER_TAG_STRING_LEAF_NODE;
    return pa.oid;
}

const StringLeafNode* StringLeafNode::fromObject(const ProtoObject* obj) {
    ProtoObjectPointer pa{}; pa.oid = obj;
    assert(pa.op.pointer_tag == POINTER_TAG_STRING_LEAF_NODE);
    pa.op.pointer_tag = 0;
    return reinterpret_cast<const StringLeafNode*>(pa.oid);
}

bool StringLeafNode::isStringLeafNode(const ProtoObject* obj) {
    if (!obj) return false;
    ProtoObjectPointer pa{}; pa.oid = obj;
    return pa.op.pointer_tag == POINTER_TAG_STRING_LEAF_NODE;
}

} // namespace proto
```

- [ ] **Step 4: Run tests — they should pass now**

```bash
cmake --build build -j$(nproc) && ./build/proto_tests --gtest_filter="StringAVLTest.Leaf*"
```
Expected: 4 tests PASSED.

- [ ] **Step 5: Commit**

```bash
git add core/ProtoString.cpp test/test_string.cpp
git commit -m "feat(strings): implement StringLeafNode with UTF-8 support and FNV-1a hash"
```

---

## Task 3: StringInternalNode Implementation

**Files:**
- Modify: `core/ProtoString.cpp`
- Modify: `test/test_string.cpp`

- [ ] **Step 1: Add failing tests**

```cpp
TEST_F(StringAVLTest, InternalNodeCharsAndHash) {
    const uint8_t b1[] = {'a','b','c'};
    const uint8_t b2[] = {'d','e'};
    auto* l1 = new(c) StringLeafNode(c, b1, 3, 3);
    auto* l2 = new(c) StringLeafNode(c, b2, 2, 2);
    auto* node = new(c) StringInternalNode(c, l1->asObject(), l2->asObject());

    EXPECT_EQ(node->total_chars, 5u);
    EXPECT_EQ(node->left_chars,  3u);
    EXPECT_EQ(node->total_bytes, 5u);
    EXPECT_EQ(node->height,      1u);
    EXPECT_EQ(node->subtree_hash,
              hashCombine(l1->content_hash, l2->content_hash));
}

TEST_F(StringAVLTest, InternalNodeBalanceLeaves) {
    // height of a node over two leaves must be 1
    const uint8_t b[] = {'x'};
    auto* l = new(c) StringLeafNode(c, b, 1, 1);
    auto* node = new(c) StringInternalNode(c, l->asObject(), l->asObject());
    EXPECT_EQ(StringInternalNode::nodeHeight(node->asObject()), 1);
    EXPECT_EQ(StringInternalNode::balance(node->asObject()), 0);
}
```

- [ ] **Step 2: Run tests to verify failure (missing constructor)**

```bash
cmake --build build -j$(nproc) 2>&1 | grep "error:" | head -10
```

- [ ] **Step 3: Implement `StringInternalNode` in `core/ProtoString.cpp` after `StringLeafNode` code**

```cpp
// ===== StringInternalNode =================================================

StringInternalNode::StringInternalNode(ProtoContext* ctx,
                                       const ProtoObject* l,
                                       const ProtoObject* r)
    : left(l)
    , right(r)
    , total_chars(charCount(l) + charCount(r))
    , left_chars(charCount(l))
    , total_bytes(byteCount(l) + byteCount(r))
    , subtree_hash(hashCombine(subtreeHash(l), subtreeHash(r)))
    , height(static_cast<uint8_t>(1 + std::max(nodeHeight(l), nodeHeight(r))))
    , _pad{}
{}

int StringInternalNode::nodeHeight(const ProtoObject* n) {
    if (!n) return 0;
    if (StringLeafNode::isStringLeafNode(n))     return 0;  // leaves have implicit height 0
    if (isStringInternalNode(n))
        return fromObject(n)->height;
    return 0;
}

int StringInternalNode::balance(const ProtoObject* n) {
    if (!n || !isStringInternalNode(n)) return 0;
    auto* node = fromObject(n);
    return nodeHeight(node->left) - nodeHeight(node->right);
}

uint64_t StringInternalNode::subtreeHash(const ProtoObject* n) {
    if (!n) return 0;
    if (StringLeafNode::isStringLeafNode(n))   return StringLeafNode::fromObject(n)->content_hash;
    if (isStringInternalNode(n))               return fromObject(n)->subtree_hash;
    return 0;
}

uint32_t StringInternalNode::charCount(const ProtoObject* n) {
    if (!n) return 0;
    if (StringLeafNode::isStringLeafNode(n))   return StringLeafNode::fromObject(n)->char_count;
    if (isStringInternalNode(n))               return fromObject(n)->total_chars;
    return 0;
}

uint32_t StringInternalNode::byteCount(const ProtoObject* n) {
    if (!n) return 0;
    if (StringLeafNode::isStringLeafNode(n))   return StringLeafNode::fromObject(n)->byte_count;
    if (isStringInternalNode(n))               return fromObject(n)->total_bytes;
    return 0;
}

const ProtoObject* StringInternalNode::asObject() const {
    ProtoObjectPointer pa{};
    pa.oid = reinterpret_cast<const ProtoObject*>(this);
    pa.op.pointer_tag = POINTER_TAG_STRING_INTERNAL_NODE;
    return pa.oid;
}

const StringInternalNode* StringInternalNode::fromObject(const ProtoObject* obj) {
    ProtoObjectPointer pa{}; pa.oid = obj;
    assert(pa.op.pointer_tag == POINTER_TAG_STRING_INTERNAL_NODE);
    pa.op.pointer_tag = 0;
    return reinterpret_cast<const StringInternalNode*>(pa.oid);
}

bool StringInternalNode::isStringInternalNode(const ProtoObject* obj) {
    if (!obj) return false;
    ProtoObjectPointer pa{}; pa.oid = obj;
    return pa.op.pointer_tag == POINTER_TAG_STRING_INTERNAL_NODE;
}
```

Also add `hashCombine` to the utility section as a free function visible in namespace scope (it's already defined above — just make sure it's accessible here).

- [ ] **Step 4: Run tests**

```bash
cmake --build build -j$(nproc) && ./build/proto_tests --gtest_filter="StringAVLTest.Internal*"
```
Expected: 2 tests PASSED.

- [ ] **Step 5: Commit**

```bash
git add core/ProtoString.cpp test/test_string.cpp
git commit -m "feat(strings): implement StringInternalNode with O(1) derived field computation"
```

---

## Task 4: AVL Rebalance + concat Primitive

**Files:**
- Modify: `core/ProtoString.cpp`
- Modify: `test/test_string.cpp`

- [ ] **Step 1: Add failing tests**

```cpp
// Free function declarations needed for tests:
// const ProtoObject* strConcat(ProtoContext*, const ProtoObject*, const ProtoObject*);

TEST_F(StringAVLTest, ConcatTwoLeaves) {
    const uint8_t b1[] = {'a','b'};
    const uint8_t b2[] = {'c','d'};
    auto* l1 = new(c) StringLeafNode(c, b1, 2, 2);
    auto* l2 = new(c) StringLeafNode(c, b2, 2, 2);

    const ProtoObject* result = strConcat(c, l1->asObject(), l2->asObject());
    EXPECT_EQ(StringInternalNode::charCount(result), 4u);
    EXPECT_EQ(StringInternalNode::nodeHeight(result), 1);
}

TEST_F(StringAVLTest, ConcatBalancesOnImbalance) {
    // Build a right-heavy chain of 4 leaves; concat should keep height <= 3
    const uint8_t b[] = {'x'};
    auto* l = new(c) StringLeafNode(c, b, 1, 1);
    const ProtoObject* acc = l->asObject();
    for (int i = 0; i < 7; ++i) {
        auto* li = new(c) StringLeafNode(c, b, 1, 1);
        acc = strConcat(c, acc, li->asObject());
    }
    // 8 leaves: balanced AVL height should be 3 (log2(8))
    EXPECT_LE(StringInternalNode::nodeHeight(acc), 3);
}

TEST_F(StringAVLTest, ConcatNullLeft) {
    const uint8_t b[] = {'a'};
    auto* l = new(c) StringLeafNode(c, b, 1, 1);
    EXPECT_EQ(strConcat(c, nullptr, l->asObject()), l->asObject());
}

TEST_F(StringAVLTest, ConcatNullRight) {
    const uint8_t b[] = {'a'};
    auto* l = new(c) StringLeafNode(c, b, 1, 1);
    EXPECT_EQ(strConcat(c, l->asObject(), nullptr), l->asObject());
}
```

- [ ] **Step 2: Run to verify failure**

```bash
cmake --build build -j$(nproc) 2>&1 | grep "error:" | head -5
```

- [ ] **Step 3: Implement `rebalance` and `strConcat` in `core/ProtoString.cpp`**

Add after the `StringInternalNode` implementation:

```cpp
// ===== AVL primitives =====================================================

static const ProtoObject* makeInternal(ProtoContext* ctx,
                                        const ProtoObject* l,
                                        const ProtoObject* r) {
    return (new(ctx) StringInternalNode(ctx, l, r))->asObject();
}

static const ProtoObject* rotateRight(ProtoContext* ctx, const ProtoObject* y) {
    // y must be StringInternalNode with a left StringInternalNode child
    auto* yn = StringInternalNode::fromObject(y);
    auto* xn = StringInternalNode::fromObject(yn->left);
    const ProtoObject* new_y = makeInternal(ctx, xn->right, yn->right);
    return makeInternal(ctx, xn->left, new_y);
}

static const ProtoObject* rotateLeft(ProtoContext* ctx, const ProtoObject* x) {
    auto* xn = StringInternalNode::fromObject(x);
    auto* yn = StringInternalNode::fromObject(xn->right);
    const ProtoObject* new_x = makeInternal(ctx, xn->left, yn->left);
    return makeInternal(ctx, new_x, yn->right);
}

static const ProtoObject* avlRebalance(ProtoContext* ctx, const ProtoObject* node) {
    if (!StringInternalNode::isStringInternalNode(node)) return node;
    int bal = StringInternalNode::balance(node);
    auto* n = StringInternalNode::fromObject(node);

    if (bal > 1) {
        if (StringInternalNode::balance(n->left) >= 0)
            return rotateRight(ctx, node);                           // LL
        const ProtoObject* new_left = rotateLeft(ctx, n->left);
        return rotateRight(ctx, makeInternal(ctx, new_left, n->right)); // LR
    }
    if (bal < -1) {
        if (StringInternalNode::balance(n->right) <= 0)
            return rotateLeft(ctx, node);                            // RR
        const ProtoObject* new_right = rotateRight(ctx, n->right);
        return rotateLeft(ctx, makeInternal(ctx, n->left, new_right)); // RL
    }
    return node;
}

const ProtoObject* strConcat(ProtoContext* ctx,
                              const ProtoObject* a,
                              const ProtoObject* b) {
    if (!a) return b;
    if (!b) return a;

    int ha = StringInternalNode::nodeHeight(a);
    int hb = StringInternalNode::nodeHeight(b);

    if (std::abs(ha - hb) <= 1)
        return makeInternal(ctx, a, b);

    if (ha > hb + 1) {
        auto* an = StringInternalNode::fromObject(a);
        const ProtoObject* new_right = strConcat(ctx, an->right, b);
        return avlRebalance(ctx, makeInternal(ctx, an->left, new_right));
    } else {
        auto* bn = StringInternalNode::fromObject(b);
        const ProtoObject* new_left = strConcat(ctx, a, bn->left);
        return avlRebalance(ctx, makeInternal(ctx, new_left, bn->right));
    }
}
```

Declare `strConcat` in the anonymous internal scope so tests can access it. Add a forward declaration at the top of the cpp file or in a small internal header if needed.

- [ ] **Step 4: Run tests**

```bash
cmake --build build -j$(nproc) && ./build/proto_tests --gtest_filter="StringAVLTest.Concat*"
```
Expected: 4 tests PASSED.

- [ ] **Step 5: Commit**

```bash
git add core/ProtoString.cpp test/test_string.cpp
git commit -m "feat(strings): implement AVL rebalance and strConcat primitive"
```

---

## Task 5: split Primitive + charAt

**Files:**
- Modify: `core/ProtoString.cpp`
- Modify: `test/test_string.cpp`

- [ ] **Step 1: Add failing tests**

```cpp
// Declarations needed:
// struct SplitResult { const ProtoObject* left; const ProtoObject* right; };
// SplitResult strSplit(ProtoContext*, const ProtoObject*, uint32_t char_index);
// uint32_t strCharAt(const ProtoObject* root, uint32_t index, uint32_t* out_codepoint);

TEST_F(StringAVLTest, SplitLeafInMiddle) {
    // "hello" split at 2 → "he" | "llo"
    const uint8_t b[] = {'h','e','l','l','o'};
    auto* leaf = new(c) StringLeafNode(c, b, 5, 5);
    auto [left, right] = strSplit(c, leaf->asObject(), 2);

    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);
    EXPECT_EQ(StringInternalNode::charCount(left),  2u);
    EXPECT_EQ(StringInternalNode::charCount(right), 3u);
}

TEST_F(StringAVLTest, SplitAtZero) {
    const uint8_t b[] = {'a','b'};
    auto* l = new(c) StringLeafNode(c, b, 2, 2);
    auto [left, right] = strSplit(c, l->asObject(), 0);
    EXPECT_EQ(left, nullptr);
    EXPECT_EQ(StringInternalNode::charCount(right), 2u);
}

TEST_F(StringAVLTest, SplitAtEnd) {
    const uint8_t b[] = {'a','b'};
    auto* l = new(c) StringLeafNode(c, b, 2, 2);
    auto [left, right] = strSplit(c, l->asObject(), 2);
    EXPECT_EQ(StringInternalNode::charCount(left), 2u);
    EXPECT_EQ(right, nullptr);
}

TEST_F(StringAVLTest, SplitTreeReconstitutesViaConcat) {
    const uint8_t b[] = {'a','b','c','d','e','f'};
    auto* leaf = new(c) StringLeafNode(c, b, 6, 6);
    auto [left, right] = strSplit(c, leaf->asObject(), 3);
    const ProtoObject* rejoined = strConcat(c, left, right);
    EXPECT_EQ(StringInternalNode::charCount(rejoined), 6u);
    EXPECT_EQ(StringInternalNode::subtreeHash(rejoined),
              StringInternalNode::subtreeHash(leaf->asObject()));
}

TEST_F(StringAVLTest, CharAtInTree) {
    // Build "abcde" across two leaves
    const uint8_t b1[] = {'a','b','c'};
    const uint8_t b2[] = {'d','e'};
    auto* l1 = new(c) StringLeafNode(c, b1, 3, 3);
    auto* l2 = new(c) StringLeafNode(c, b2, 2, 2);
    const ProtoObject* tree = strConcat(c, l1->asObject(), l2->asObject());

    uint32_t cp;
    strCharAt(tree, 0, &cp); EXPECT_EQ(cp, (uint32_t)'a');
    strCharAt(tree, 2, &cp); EXPECT_EQ(cp, (uint32_t)'c');
    strCharAt(tree, 3, &cp); EXPECT_EQ(cp, (uint32_t)'d');
    strCharAt(tree, 4, &cp); EXPECT_EQ(cp, (uint32_t)'e');
}
```

- [ ] **Step 2: Run to verify failure**

```bash
cmake --build build -j$(nproc) 2>&1 | grep "error:" | head -5
```

- [ ] **Step 3: Implement `strSplit` and `strCharAt` in `core/ProtoString.cpp`**

```cpp
struct SplitResult { const ProtoObject* left; const ProtoObject* right; };

static SplitResult splitLeaf(ProtoContext* ctx,
                              const StringLeafNode* leaf,
                              uint32_t char_index) {
    uint32_t byte_split = leaf->charToByteOffset(char_index);
    // left part
    const StringLeafNode* l = (byte_split > 0)
        ? new(ctx) StringLeafNode(ctx, leaf->utf8_payload, byte_split,
                                  static_cast<uint16_t>(char_index), true)
        : nullptr;
    // right part
    uint8_t right_bytes = leaf->byte_count - byte_split;
    const StringLeafNode* r = (right_bytes > 0)
        ? new(ctx) StringLeafNode(ctx, leaf->utf8_payload + byte_split, right_bytes,
                                  static_cast<uint16_t>(leaf->char_count - char_index), true)
        : nullptr;
    return { l ? l->asObject() : nullptr, r ? r->asObject() : nullptr };
}

SplitResult strSplit(ProtoContext* ctx,
                     const ProtoObject* node,
                     uint32_t char_index) {
    if (!node)                                             return {nullptr, nullptr};
    uint32_t total = StringInternalNode::charCount(node);
    if (char_index == 0)                                  return {nullptr, node};
    if (char_index >= total)                              return {node, nullptr};

    if (StringLeafNode::isStringLeafNode(node))
        return splitLeaf(ctx, StringLeafNode::fromObject(node), char_index);

    auto* n = StringInternalNode::fromObject(node);
    if (char_index <= n->left_chars) {
        auto [ll, lr] = strSplit(ctx, n->left, char_index);
        return { ll, strConcat(ctx, lr, n->right) };
    } else {
        auto [rl, rr] = strSplit(ctx, n->right, char_index - n->left_chars);
        return { strConcat(ctx, n->left, rl), rr };
    }
}

void strCharAt(const ProtoObject* node, uint32_t index, uint32_t* out) {
    while (StringInternalNode::isStringInternalNode(node)) {
        auto* n = StringInternalNode::fromObject(node);
        if (index < n->left_chars)
            node = n->left;
        else {
            index -= n->left_chars;
            node = n->right;
        }
    }
    if (StringLeafNode::isStringLeafNode(node)) {
        auto* leaf = StringLeafNode::fromObject(node);
        uint32_t byte_pos = leaf->charToByteOffset(index);
        *out = leaf->codepointAt(byte_pos);
    } else {
        *out = 0;
    }
}
```

- [ ] **Step 4: Run tests**

```bash
cmake --build build -j$(nproc) && ./build/proto_tests --gtest_filter="StringAVLTest.Split*:StringAVLTest.CharAt*"
```
Expected: 6 tests PASSED.

- [ ] **Step 5: Run all existing tests to confirm no regressions**

```bash
ctest --test-dir build -j$(nproc) --output-on-failure
```
Expected: all previously passing tests still pass.

- [ ] **Step 6: Commit**

```bash
git add core/ProtoString.cpp test/test_string.cpp
git commit -m "feat(strings): implement strSplit and strCharAt on AVL tree"
```

---

## Task 6: ProtoStringImplementation Rewrite

**Files:**
- Modify: `core/ProtoString.cpp` — rewrite `ProtoStringImplementation` class methods
- Modify: `headers/proto_internal.h` — update `ProtoStringImplementation` class declaration

- [ ] **Step 1: Update `ProtoStringImplementation` class in `proto_internal.h`**

Find the existing `ProtoStringImplementation` class and replace its body:

```cpp
class ProtoStringImplementation final : public Cell {
public:
    const ProtoObject* avl_root;   // StringLeafNode, StringInternalNode, or nullptr (empty)

    explicit ProtoStringImplementation(ProtoContext* ctx, const ProtoObject* root);

    // --- Internal helpers ---
    uint32_t implGetSize()  const;
    uint64_t implGetHash()  const;

    const ProtoObject* implAsObject(ProtoContext* ctx) const;

    // Returns a new ProtoStringImplementation tagged as Symbol.
    const ProtoStringImplementation* implAsSymbol(ProtoContext* ctx) const;

    // Factory: builds from UTF-8 bytes, selects leaf/internal as needed.
    static const ProtoStringImplementation* fromUTF8Bytes(ProtoContext* ctx,
                                                           const uint8_t* bytes,
                                                           size_t len);
};
```

- [ ] **Step 2: Add failing tests for ProtoStringImplementation**

```cpp
TEST_F(StringAVLTest, StringImplFromUTF8) {
    const char* src = "hello";
    auto* s = ProtoStringImplementation::fromUTF8Bytes(
        c, reinterpret_cast<const uint8_t*>(src), 5);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->implGetSize(), 5u);
}

TEST_F(StringAVLTest, StringImplHashIsRootHash) {
    const char* src = "abc";
    auto* s = ProtoStringImplementation::fromUTF8Bytes(
        c, reinterpret_cast<const uint8_t*>(src), 3);
    EXPECT_EQ(s->implGetHash(),
              StringInternalNode::subtreeHash(s->avl_root));
}

TEST_F(StringAVLTest, StringImplLargeFromUTF8) {
    // Build a string that requires more than one leaf (> 48 bytes)
    std::string long_str(100, 'a');
    auto* s = ProtoStringImplementation::fromUTF8Bytes(
        c, reinterpret_cast<const uint8_t*>(long_str.data()), long_str.size());
    EXPECT_EQ(s->implGetSize(), 100u);
    EXPECT_LE(StringInternalNode::nodeHeight(s->avl_root), 2);
}
```

- [ ] **Step 3: Implement `ProtoStringImplementation` in `core/ProtoString.cpp`**

Replace the existing `ProtoStringImplementation` constructor and methods with:

```cpp
ProtoStringImplementation::ProtoStringImplementation(ProtoContext* ctx,
                                                      const ProtoObject* root)
    : avl_root(root) {}

uint32_t ProtoStringImplementation::implGetSize() const {
    return StringInternalNode::charCount(avl_root);
}

uint64_t ProtoStringImplementation::implGetHash() const {
    return StringInternalNode::subtreeHash(avl_root);
}

const ProtoObject* ProtoStringImplementation::implAsObject(ProtoContext* ctx) const {
    ProtoObjectPointer pa{};
    pa.oid = reinterpret_cast<const ProtoObject*>(this);
    pa.op.pointer_tag = POINTER_TAG_STRING;
    return pa.oid;
}

const ProtoStringImplementation* ProtoStringImplementation::implAsSymbol(ProtoContext* ctx) const {
    // Same cell, different tag on the returned pointer
    ProtoObjectPointer pa{};
    pa.oid = reinterpret_cast<const ProtoObject*>(this);
    pa.op.pointer_tag = POINTER_TAG_SYMBOL;
    return reinterpret_cast<const ProtoStringImplementation*>(pa.oid);
}

// Build a balanced AVL from a contiguous UTF-8 byte array.
static const ProtoObject* buildAVL(ProtoContext* ctx,
                                    const uint8_t* bytes, size_t len) {
    if (len == 0) return nullptr;

    // Count codepoints in this segment
    uint16_t char_cnt = 0;
    for (size_t i = 0; i < len; ) {
        i += utf8SeqLen(bytes[i]);
        ++char_cnt;
    }

    if (len <= StringLeafNode::MAX_PAYLOAD) {
        return (new(ctx) StringLeafNode(ctx,
            bytes, static_cast<uint8_t>(len), char_cnt))->asObject();
    }

    // Split roughly in half (at a codepoint boundary)
    size_t mid_byte = len / 2;
    while (mid_byte > 0 && (bytes[mid_byte] & 0xC0) == 0x80) --mid_byte;

    const ProtoObject* left  = buildAVL(ctx, bytes, mid_byte);
    const ProtoObject* right = buildAVL(ctx, bytes + mid_byte, len - mid_byte);
    return strConcat(ctx, left, right);
}

const ProtoStringImplementation* ProtoStringImplementation::fromUTF8Bytes(
        ProtoContext* ctx, const uint8_t* bytes, size_t len) {
    return new(ctx) ProtoStringImplementation(ctx, buildAVL(ctx, bytes, len));
}
```

- [ ] **Step 4: Run tests**

```bash
cmake --build build -j$(nproc) && ./build/proto_tests --gtest_filter="StringAVLTest.StringImpl*"
```
Expected: 3 tests PASSED.

- [ ] **Step 5: Commit**

```bash
git add core/ProtoString.cpp headers/proto_internal.h test/test_string.cpp
git commit -m "feat(strings): rewrite ProtoStringImplementation to wrap AVL root"
```

---

## Task 7: Rewrite ProtoString Public Operations

**Files:**
- Modify: `core/ProtoString.cpp` — rewrite all `ProtoString::*` methods

- [ ] **Step 1: Add tests for core operations**

```cpp
class StringTest : public ::testing::Test {
protected:
    proto::ProtoSpace space;
    proto::ProtoContext ctx{&space};
    ProtoContext* c = &ctx;

    const ProtoString* str(const char* s) {
        return ProtoString::fromUTF8(c, s);
    }
};

TEST_F(StringTest, GetSize) {
    EXPECT_EQ(str("hello")->getSize(c), 5u);
    EXPECT_EQ(str("")->getSize(c),      0u);
    EXPECT_EQ(str("héllo")->getSize(c), 5u);  // é is 2 bytes, 1 char
}

TEST_F(StringTest, GetAt) {
    auto* s = str("hello");
    // getAt returns embedded unicode char pointer
    auto* ch = s->getAt(c, 0);
    ASSERT_NE(ch, nullptr);
    ProtoObjectPointer pa{}; pa.oid = ch;
    EXPECT_EQ(pa.unicodeChar.unicodeValue, (unsigned long)'h');
}

TEST_F(StringTest, AppendLast) {
    auto* s = str("hello")->appendLast(c, str(" world"));
    EXPECT_EQ(s->getSize(c), 11u);
}

TEST_F(StringTest, AppendFirst) {
    auto* s = str(" world")->appendFirst(c, str("hello"));
    EXPECT_EQ(s->getSize(c), 11u);
}

TEST_F(StringTest, GetSlice) {
    auto* s = str("hello world")->getSlice(c, 6, 11);
    EXPECT_EQ(s->getSize(c), 5u);
}

TEST_F(StringTest, RemoveAt) {
    auto* s = str("hello")->removeAt(c, 1);
    EXPECT_EQ(s->getSize(c), 4u);
}

TEST_F(StringTest, InsertAt) {
    auto* s = str("hllo")->insertAtString(c, 1, str("e"));
    EXPECT_EQ(s->getSize(c), 5u);
}

TEST_F(StringTest, CmpToString) {
    EXPECT_EQ(str("abc")->cmp_to_string(c, str("abc")),  0);
    EXPECT_LT(str("abc")->cmp_to_string(c, str("abd")),  0);
    EXPECT_GT(str("abd")->cmp_to_string(c, str("abc")),  0);
}

TEST_F(StringTest, ToUTF8RoundTrip) {
    std::string out;
    str("hello world")->toUTF8String(c, out);
    EXPECT_EQ(out, "hello world");
}

TEST_F(StringTest, ToUTF8RoundTripMultibyte) {
    std::string out;
    str("héllo")->toUTF8String(c, out);
    EXPECT_EQ(out, "héllo");
}
```

- [ ] **Step 2: Implement all `ProtoString::*` methods in `core/ProtoString.cpp`**

Replace the entire section of `ProtoString::` method implementations with the following.
All methods follow the same pattern: unwrap to `ProtoStringImplementation`, operate on
`avl_root` via the AVL primitives, wrap result.

```cpp
// Helper: unwrap any ProtoString* (embedded, symbol, or string) to impl
static const ProtoStringImplementation* getImpl(const ProtoObject* obj) {
    ProtoObjectPointer pa{}; pa.oid = obj;
    unsigned long tag = pa.op.pointer_tag;
    if (tag == POINTER_TAG_STRING || tag == POINTER_TAG_SYMBOL) {
        pa.op.pointer_tag = 0;
        return reinterpret_cast<const ProtoStringImplementation*>(pa.oid);
    }
    return nullptr;  // embedded — handled by callers before reaching here
}

// Helper: materialize any string pointer to its AVL root
static const ProtoObject* getRoot(ProtoContext* ctx, const ProtoObject* strObj) {
    if (!strObj) return nullptr;
    if (isInlineString(strObj)) {
        // Convert inline string to a StringLeafNode for uniform handling
        unsigned long byte_count = strObj->...;  // use inlineStringByteCount
        uint8_t bytes[6];
        for (unsigned long i = 0; i < byte_count; ++i)
            bytes[i] = inlineStringByte(strObj, i);
        // Count chars
        uint16_t chars = 0;
        for (unsigned long i = 0; i < byte_count; )  {
            i += utf8SeqLen(bytes[i]); ++chars;
        }
        return (new(ctx) StringLeafNode(ctx, bytes,
            static_cast<uint8_t>(byte_count), chars))->asObject();
    }
    auto* impl = getImpl(strObj);
    return impl ? impl->avl_root : nullptr;
}

// Helper: wrap an AVL root back into a ProtoString*.
// If root represents ≤ 6 UTF-8 bytes and is a single leaf, returns inline.
static const ProtoObject* wrapRoot(ProtoContext* ctx, const ProtoObject* root) {
    if (!root) {
        // empty string as inline with byte_count=0
        return createInlineStringUTF8(ctx, nullptr, 0);
    }
    if (StringLeafNode::isStringLeafNode(root)) {
        auto* leaf = StringLeafNode::fromObject(root);
        if (leaf->byte_count <= INLINE_STRING_MAX_BYTES)
            return createInlineStringUTF8(ctx, leaf->utf8_payload, leaf->byte_count);
    }
    return (new(ctx) ProtoStringImplementation(ctx, root))->implAsObject(ctx);
}

unsigned long ProtoString::getSize(ProtoContext* context) const {
    auto* self = reinterpret_cast<const ProtoObject*>(this);
    if (isInlineString(self)) return inlineStringByteCount(self);  // TODO: actual char count
    auto* impl = getImpl(self);
    return impl ? impl->implGetSize() : 0;
}

const ProtoObject* ProtoString::getAt(ProtoContext* context, int index) const {
    auto* self = reinterpret_cast<const ProtoObject*>(this);
    const ProtoObject* root = getRoot(context, self);
    uint32_t cp;
    strCharAt(root, static_cast<uint32_t>(index), &cp);
    // Return as embedded unicode char
    ProtoObjectPointer pa{};
    pa.op.pointer_tag    = POINTER_TAG_EMBEDDED_VALUE;
    pa.op.embedded_type  = EMBEDDED_TYPE_UNICODE_CHAR;
    pa.unicodeChar.unicodeValue = cp;
    return pa.oid;
}

const ProtoString* ProtoString::appendLast(ProtoContext* context,
                                            const ProtoString* other) const {
    auto* self     = reinterpret_cast<const ProtoObject*>(this);
    auto* otherObj = reinterpret_cast<const ProtoObject*>(other);
    const ProtoObject* root = strConcat(context, getRoot(context, self),
                                                  getRoot(context, otherObj));
    return reinterpret_cast<const ProtoString*>(wrapRoot(context, root));
}

const ProtoString* ProtoString::appendFirst(ProtoContext* context,
                                             const ProtoString* other) const {
    auto* self     = reinterpret_cast<const ProtoObject*>(this);
    auto* otherObj = reinterpret_cast<const ProtoObject*>(other);
    const ProtoObject* root = strConcat(context, getRoot(context, otherObj),
                                                  getRoot(context, self));
    return reinterpret_cast<const ProtoString*>(wrapRoot(context, root));
}

const ProtoString* ProtoString::getSlice(ProtoContext* context, int from, int to) const {
    auto* self = reinterpret_cast<const ProtoObject*>(this);
    const ProtoObject* root = getRoot(context, self);
    auto [_, right] = strSplit(context, root, static_cast<uint32_t>(from));
    auto [mid, __]  = strSplit(context, right, static_cast<uint32_t>(to - from));
    return reinterpret_cast<const ProtoString*>(wrapRoot(context, mid));
}

const ProtoString* ProtoString::removeAt(ProtoContext* context, int index) const {
    return removeSlice(context, index, index + 1);
}

const ProtoString* ProtoString::removeSlice(ProtoContext* context, int from, int to) const {
    auto* self = reinterpret_cast<const ProtoObject*>(this);
    const ProtoObject* root = getRoot(context, self);
    auto [left, rest]  = strSplit(context, root, static_cast<uint32_t>(from));
    auto [_,    right] = strSplit(context, rest, static_cast<uint32_t>(to - from));
    return reinterpret_cast<const ProtoString*>(wrapRoot(context, strConcat(context, left, right)));
}

const ProtoString* ProtoString::insertAtString(ProtoContext* context, int index,
                                                const ProtoString* chars) const {
    auto* self     = reinterpret_cast<const ProtoObject*>(this);
    auto* charsObj = reinterpret_cast<const ProtoObject*>(chars);
    const ProtoObject* root = getRoot(context, self);
    auto [left, right] = strSplit(context, root, static_cast<uint32_t>(index));
    const ProtoObject* result = strConcat(context,
                                           strConcat(context, left, getRoot(context, charsObj)),
                                           right);
    return reinterpret_cast<const ProtoString*>(wrapRoot(context, result));
}

const ProtoString* ProtoString::insertAt(ProtoContext* context, int index,
                                          const ProtoObject* character) const {
    // character is an embedded unicode char — wrap in a single-codepoint string
    ProtoObjectPointer pa{}; pa.oid = character;
    uint32_t cp = static_cast<uint32_t>(pa.unicodeChar.unicodeValue);
    uint8_t buf[4];
    uint8_t byte_len;
    if (cp < 0x80)        { buf[0] = cp; byte_len = 1; }
    else if (cp < 0x800)  { buf[0] = 0xC0|(cp>>6); buf[1] = 0x80|(cp&0x3F); byte_len = 2; }
    else if (cp < 0x10000){ buf[0] = 0xE0|(cp>>12); buf[1] = 0x80|((cp>>6)&0x3F);
                            buf[2] = 0x80|(cp&0x3F); byte_len = 3; }
    else                  { buf[0] = 0xF0|(cp>>18); buf[1] = 0x80|((cp>>12)&0x3F);
                            buf[2] = 0x80|((cp>>6)&0x3F); buf[3] = 0x80|(cp&0x3F); byte_len = 4; }
    auto* leaf = new(context) StringLeafNode(context, buf, byte_len, 1);
    const ProtoString* char_str = reinterpret_cast<const ProtoString*>(
        wrapRoot(context, leaf->asObject()));
    return insertAtString(context, index, char_str);
}

int ProtoString::cmp_to_string(ProtoContext* context, const ProtoString* other) const {
    auto* a = reinterpret_cast<const ProtoObject*>(this);
    auto* b = reinterpret_cast<const ProtoObject*>(other);
    // Fast path: same pointer
    if (a == b) return 0;
    // Fast path: different hash means different content
    uint64_t ha = getProtoStringHash(context, a);
    uint64_t hb = getProtoStringHash(context, b);
    if (ha != hb || getSize(context) != other->getSize(context)) {
        // Fall through to content comparison; result must be lexicographic
    }
    // Content comparison via iterators
    auto* itA = getIterator(context);
    auto* itB = other->getIterator(context);
    while (true) {
        bool hasA = itA->hasNext(context);
        bool hasB = itB->hasNext(context);
        if (!hasA && !hasB) return 0;
        if (!hasA) return -1;
        if (!hasB) return  1;
        uint32_t ca = itA->nextCodepoint(context);
        uint32_t cb = itB->nextCodepoint(context);
        if (ca != cb) return (ca < cb) ? -1 : 1;
    }
}

void ProtoString::toUTF8String(ProtoContext* context, std::string& out) const {
    auto* self = reinterpret_cast<const ProtoObject*>(this);
    out.clear();
    out.reserve(getSize(context));
    auto* it = getIterator(context);
    while (it->hasNext(context)) {
        uint32_t cp = it->nextCodepoint(context);
        if (cp < 0x80)        { out += static_cast<char>(cp); }
        else if (cp < 0x800)  { out += static_cast<char>(0xC0|(cp>>6));
                                out += static_cast<char>(0x80|(cp&0x3F)); }
        else if (cp < 0x10000){ out += static_cast<char>(0xE0|(cp>>12));
                                out += static_cast<char>(0x80|((cp>>6)&0x3F));
                                out += static_cast<char>(0x80|(cp&0x3F)); }
        else                  { out += static_cast<char>(0xF0|(cp>>18));
                                out += static_cast<char>(0x80|((cp>>12)&0x3F));
                                out += static_cast<char>(0x80|((cp>>6)&0x3F));
                                out += static_cast<char>(0x80|(cp&0x3F)); }
    }
}

unsigned long ProtoString::getHash(ProtoContext* context) const {
    return getProtoStringHash(context, reinterpret_cast<const ProtoObject*>(this));
}
```

Note: `getSize` for inline strings should count codepoints, not bytes. Adjust:
```cpp
unsigned long ProtoString::getSize(ProtoContext* ctx) const {
    auto* self = reinterpret_cast<const ProtoObject*>(this);
    if (isInlineString(self)) {
        // Decode inline string to count codepoints
        unsigned long byte_count = inlineStringByteCount(self);
        unsigned long chars = 0;
        for (unsigned long i = 0; i < byte_count; ) {
            i += utf8SeqLen(inlineStringByte(self, i));
            ++chars;
        }
        return chars;
    }
    auto* impl = getImpl(self);
    return impl ? impl->implGetSize() : 0;
}
```

- [ ] **Step 3: Run tests**

```bash
cmake --build build -j$(nproc) && ./build/proto_tests --gtest_filter="StringTest.*"
```
Expected: all StringTest tests PASSED.

- [ ] **Step 4: Run full suite for regressions**

```bash
ctest --test-dir build -j$(nproc) --output-on-failure
```

- [ ] **Step 5: Commit**

```bash
git add core/ProtoString.cpp headers/proto_internal.h test/test_string.cpp
git commit -m "feat(strings): rewrite ProtoString public operations using AVL split/concat"
```

---

## Task 8: StringCodepointIterator

**Files:**
- Modify: `headers/proto_internal.h` — update `ProtoStringIteratorImplementation`
- Modify: `core/ProtoString.cpp` — implement iterator

- [ ] **Step 1: Add failing tests**

```cpp
TEST_F(StringTest, IteratorForward) {
    auto* s = str("abc");
    auto* it = s->getIterator(c);
    ASSERT_TRUE(it->hasNext(c));
    EXPECT_EQ(it->nextCodepoint(c), (uint32_t)'a');
    EXPECT_EQ(it->nextCodepoint(c), (uint32_t)'b');
    EXPECT_EQ(it->nextCodepoint(c), (uint32_t)'c');
    EXPECT_FALSE(it->hasNext(c));
}

TEST_F(StringTest, IteratorMultibyte) {
    // é = U+00E9 = 0xC3 0xA9
    auto* s = str("éà");
    auto* it = s->getIterator(c);
    EXPECT_EQ(it->nextCodepoint(c), 0x00E9u);  // é
    EXPECT_EQ(it->nextCodepoint(c), 0x00E0u);  // à
    EXPECT_FALSE(it->hasNext(c));
}

TEST_F(StringTest, IteratorLargeString) {
    std::string src(200, 'x');
    auto* s = ProtoString::fromUTF8(c, src.c_str());
    auto* it = s->getIterator(c);
    int count = 0;
    while (it->hasNext(c)) { it->nextCodepoint(c); ++count; }
    EXPECT_EQ(count, 200);
}
```

- [ ] **Step 2: Update `ProtoStringIteratorImplementation` declaration in `proto_internal.h`**

Find the existing declaration and replace it:

```cpp
class ProtoStringIteratorImplementation final : public Cell {
    static constexpr int MAX_DEPTH = 48;

    struct Frame {
        const StringInternalNode* node;
        bool right_pending;
    };

    Frame        stack_[MAX_DEPTH];
    int          stack_top_;
    // Active leaf state
    const uint8_t* leaf_bytes_;
    uint8_t      leaf_size_;
    uint8_t      leaf_pos_;

public:
    explicit ProtoStringIteratorImplementation(ProtoContext* ctx,
                                               const ProtoObject* avl_root);

    bool hasNext(ProtoContext* ctx) const { return leaf_bytes_ != nullptr; }
    uint32_t nextCodepoint(ProtoContext* ctx);

    const ProtoObject* implAsObject(ProtoContext* ctx) const;

private:
    void descendLeft(const ProtoObject* node);
    void advance();
    void activateLeaf(const StringLeafNode* leaf);
};
```

- [ ] **Step 3: Implement iterator in `core/ProtoString.cpp`**

```cpp
// ===== StringCodepointIterator =============================================

ProtoStringIteratorImplementation::ProtoStringIteratorImplementation(
        ProtoContext* ctx, const ProtoObject* avl_root)
    : stack_top_(-1), leaf_bytes_(nullptr), leaf_size_(0), leaf_pos_(0)
{
    if (avl_root) descendLeft(avl_root);
}

void ProtoStringIteratorImplementation::activateLeaf(const StringLeafNode* leaf) {
    leaf_bytes_ = leaf->utf8_payload;
    leaf_size_  = leaf->byte_count;
    leaf_pos_   = 0;
}

void ProtoStringIteratorImplementation::descendLeft(const ProtoObject* node) {
    while (StringInternalNode::isStringInternalNode(node)) {
        auto* n = StringInternalNode::fromObject(node);
        stack_[++stack_top_] = { n, false };
        node = n->left;
    }
    if (StringLeafNode::isStringLeafNode(node))
        activateLeaf(StringLeafNode::fromObject(node));
}

void ProtoStringIteratorImplementation::advance() {
    leaf_bytes_ = nullptr;
    while (stack_top_ >= 0) {
        Frame& f = stack_[stack_top_];
        if (!f.right_pending) {
            f.right_pending = true;
            descendLeft(f.node->right);
            if (leaf_bytes_) return;
        } else {
            --stack_top_;
        }
    }
}

uint32_t ProtoStringIteratorImplementation::nextCodepoint(ProtoContext*) {
    assert(leaf_bytes_);
    uint32_t seq = utf8SeqLen(leaf_bytes_[leaf_pos_]);
    uint32_t cp  = decodeCodepoint(leaf_bytes_ + leaf_pos_);
    leaf_pos_ += seq;
    if (leaf_pos_ >= leaf_size_) advance();
    return cp;
}

const ProtoStringIterator* ProtoString::getIterator(ProtoContext* context) const {
    auto* self = reinterpret_cast<const ProtoObject*>(this);
    const ProtoObject* root = getRoot(context, self);
    auto* it = new(context) ProtoStringIteratorImplementation(context, root);
    ProtoObjectPointer pa{};
    pa.oid = reinterpret_cast<const ProtoObject*>(it);
    pa.op.pointer_tag = POINTER_TAG_STRING_ITERATOR;
    return reinterpret_cast<const ProtoStringIterator*>(pa.oid);
}
```

Also update `ProtoStringIterator::hasNext` and `ProtoStringIterator::next` (or equivalent public methods) in `core/ProtoString.cpp` to delegate to `ProtoStringIteratorImplementation`.

- [ ] **Step 4: Run tests**

```bash
cmake --build build -j$(nproc) && ./build/proto_tests --gtest_filter="StringTest.Iterator*"
```
Expected: 3 tests PASSED.

- [ ] **Step 5: Run full suite**

```bash
ctest --test-dir build -j$(nproc) --output-on-failure
```

- [ ] **Step 6: Commit**

```bash
git add core/ProtoString.cpp headers/proto_internal.h test/test_string.cpp
git commit -m "feat(strings): implement stack-based StringCodepointIterator on AVL tree"
```

---

## Task 9: Embedded UTF-8 Pointer — createInlineStringUTF8

**Files:**
- Modify: `core/ProtoString.cpp`
- Modify: `test/test_string.cpp`

- [ ] **Step 1: Add tests**

```cpp
TEST_F(StringTest, InlineStringASCII) {
    // "hi" — 2 bytes, fits inline
    auto* s = ProtoString::fromUTF8(c, "hi");
    auto* obj = reinterpret_cast<const ProtoObject*>(s);
    EXPECT_TRUE(isInlineString(obj));
    std::string out;
    s->toUTF8String(c, out);
    EXPECT_EQ(out, "hi");
}

TEST_F(StringTest, InlineStringMaxSixBytes) {
    // 6 ASCII chars — exactly at inline limit
    auto* s = ProtoString::fromUTF8(c, "abcdef");
    auto* obj = reinterpret_cast<const ProtoObject*>(s);
    EXPECT_TRUE(isInlineString(obj));
}

TEST_F(StringTest, SevenBytesNotInline) {
    // 7 ASCII chars — exceeds inline limit, must be AVL
    auto* s = ProtoString::fromUTF8(c, "abcdefg");
    auto* obj = reinterpret_cast<const ProtoObject*>(s);
    EXPECT_FALSE(isInlineString(obj));
}

TEST_F(StringTest, InlineStringTwoByte) {
    // "é" = 2 bytes — fits inline
    auto* s = ProtoString::fromUTF8(c, "\xC3\xA9");
    auto* obj = reinterpret_cast<const ProtoObject*>(s);
    EXPECT_TRUE(isInlineString(obj));
    EXPECT_EQ(s->getSize(c), 1u);
}
```

- [ ] **Step 2: Implement `createInlineStringUTF8` in `core/ProtoString.cpp`**

```cpp
const ProtoObject* createInlineStringUTF8(ProtoContext* ctx,
                                            const uint8_t* bytes,
                                            uint8_t byte_count) {
    assert(byte_count <= INLINE_STRING_MAX_BYTES);
    ProtoObjectPointer pa{};
    pa.op.pointer_tag             = POINTER_TAG_EMBEDDED_VALUE;
    pa.op.embedded_type           = EMBEDDED_TYPE_INLINE_STRING;
    pa.inlineString.inline_byte_count = byte_count;
    uint64_t packed = 0;
    for (uint8_t i = 0; i < byte_count; ++i)
        packed |= (static_cast<uint64_t>(bytes[i]) << (i * 8));
    pa.inlineString.inline_utf8_bytes = packed;
    return pa.oid;
}
```

Update `wrapRoot` and `ProtoString::fromUTF8` to use `createInlineStringUTF8` for short strings:

```cpp
// In fromUTF8:
const ProtoString* ProtoString::fromUTF8(ProtoContext* context, const char* utf8) {
    if (!utf8) return reinterpret_cast<const ProtoString*>(
        createInlineStringUTF8(context, nullptr, 0));
    size_t len = std::strlen(utf8);
    if (len <= INLINE_STRING_MAX_BYTES)
        return reinterpret_cast<const ProtoString*>(
            createInlineStringUTF8(context,
                reinterpret_cast<const uint8_t*>(utf8),
                static_cast<uint8_t>(len)));
    auto* impl = ProtoStringImplementation::fromUTF8Bytes(
        context, reinterpret_cast<const uint8_t*>(utf8), len);
    return reinterpret_cast<const ProtoString*>(impl->implAsObject(context));
}
```

- [ ] **Step 3: Run tests**

```bash
cmake --build build -j$(nproc) && ./build/proto_tests --gtest_filter="StringTest.Inline*:StringTest.SevenBytes*"
```
Expected: 4 tests PASSED.

- [ ] **Step 4: Commit**

```bash
git add core/ProtoString.cpp test/test_string.cpp
git commit -m "feat(strings): implement createInlineStringUTF8 with 6-byte embedded encoding"
```

---

## Task 10: SymbolTable

**Files:**
- Create: `core/SymbolTable.cpp`
- Modify: `headers/proto_internal.h` — add `SymbolTable` class declaration
- Modify: `test/test_string.cpp`

- [ ] **Step 1: Add `SymbolTable` declaration to `proto_internal.h`**

After the `ProtoStringImplementation` class:

```cpp
// ---- SymbolTable ----------------------------------------------------------
// 64-shard concurrent interning table. Replaces TupleDictionary for strings.
// Strong symbols (from literals) are never collected.
// Weak symbols (auto-interned) are removed by GC sweep before cell reclaim.
class SymbolTable {
public:
    static constexpr int SHARD_COUNT = 64;

    struct Bucket {
        uint64_t           content_hash;
        const ProtoObject* symbol;          // POINTER_TAG_SYMBOL pointer
        bool               is_strong;
        Bucket*            next;
    };

    struct Shard {
        std::mutex  mutex;
        Bucket*     head = nullptr;
    };

    Shard shards[SHARD_COUNT];

    SymbolTable()  = default;
    ~SymbolTable();

    // Intern a non-interned string; returns existing Symbol or creates new one.
    // is_strong = true: symbol lives forever (for literals).
    const ProtoObject* intern(ProtoContext* ctx,
                               const ProtoObject* strObj,
                               bool is_strong = false);

    // Remove a weak symbol (called by GC sweep before freeing the cell).
    void removeWeak(uint64_t content_hash, const ProtoObject* symbol);

    // Returns true if obj is a Symbol pointer (POINTER_TAG_SYMBOL).
    static bool isSymbol(const ProtoObject* obj);

private:
    int shardIndex(uint64_t hash) const {
        return static_cast<int>((hash >> 58) & (SHARD_COUNT - 1));
    }
    static bool contentEqual(ProtoContext* ctx,
                              const ProtoObject* a, const ProtoObject* b);
    static const ProtoStringImplementation* normalizeForSymbol(ProtoContext* ctx,
                                                                const ProtoObject* strObj);
};
```

- [ ] **Step 2: Add failing tests**

```cpp
class SymbolTest : public ::testing::Test {
protected:
    proto::ProtoSpace space;
    proto::ProtoContext ctx{&space};
    ProtoContext* c = &ctx;
};

TEST_F(SymbolTest, SameSymbolSamePointer) {
    auto* s1 = ProtoString::createSymbol(c, "hello");
    auto* s2 = ProtoString::createSymbol(c, "hello");
    EXPECT_EQ(s1, s2);  // pointer identity — same Symbol
}

TEST_F(SymbolTest, DifferentSymbolsDifferentPointers) {
    auto* s1 = ProtoString::createSymbol(c, "hello");
    auto* s2 = ProtoString::createSymbol(c, "world");
    EXPECT_NE(s1, s2);
}

TEST_F(SymbolTest, SymbolEqualsStringByContent) {
    auto* sym = ProtoString::createSymbol(c, "hello");
    auto* str = ProtoString::fromUTF8(c, "hello");
    // They have different pointer tags but same content
    EXPECT_EQ(sym->cmp_to_string(c, str), 0);
}

TEST_F(SymbolTest, NonInternedStringNotSymbol) {
    auto* str = ProtoString::fromUTF8(c, "hello");
    auto* obj = reinterpret_cast<const ProtoObject*>(str);
    EXPECT_FALSE(SymbolTable::isSymbol(obj));
}

TEST_F(SymbolTest, SymbolIsSymbol) {
    auto* sym = ProtoString::createSymbol(c, "hello");
    auto* obj = reinterpret_cast<const ProtoObject*>(sym);
    EXPECT_TRUE(SymbolTable::isSymbol(obj));
}

TEST_F(SymbolTest, AutoInternOnSetAttribute) {
    // A non-interned string used as attribute key must be auto-interned
    proto::ProtoObject* obj = /* create a new proto object */
        proto::ProtoObject::create(c);
    auto* key_str = ProtoString::fromUTF8(c, "myKey");
    auto* val = /* some ProtoObject */ c->space->objectPrototype;
    obj->setAttribute(c, key_str, val);
    // The stored key should now be a symbol
    auto* stored_key = obj->getAttributeKey(c, 0);  // hypothetical accessor for test
    EXPECT_TRUE(SymbolTable::isSymbol(reinterpret_cast<const ProtoObject*>(stored_key)));
}
```

- [ ] **Step 3: Implement `core/SymbolTable.cpp`**

```cpp
#include "proto_internal.h"
#include <cstring>

namespace proto {

SymbolTable::~SymbolTable() {
    for (int i = 0; i < SHARD_COUNT; ++i) {
        Bucket* b = shards[i].head;
        while (b) { Bucket* next = b->next; delete b; b = next; }
    }
}

bool SymbolTable::isSymbol(const ProtoObject* obj) {
    if (!obj) return false;
    ProtoObjectPointer pa{}; pa.oid = obj;
    return pa.op.pointer_tag == POINTER_TAG_SYMBOL;
}

bool SymbolTable::contentEqual(ProtoContext* ctx,
                                const ProtoObject* a,
                                const ProtoObject* b) {
    // Compare via iterators — O(K), only called on hash collision
    const ProtoString* sa = reinterpret_cast<const ProtoString*>(a);
    const ProtoString* sb = reinterpret_cast<const ProtoString*>(b);
    if (sa->getSize(ctx) != sb->getSize(ctx)) return false;
    auto* ia = sa->getIterator(ctx);
    auto* ib = sb->getIterator(ctx);
    while (ia->hasNext(ctx)) {
        if (ia->nextCodepoint(ctx) != ib->nextCodepoint(ctx)) return false;
    }
    return true;
}

const ProtoStringImplementation* SymbolTable::normalizeForSymbol(
        ProtoContext* ctx, const ProtoObject* strObj) {
    // Materialize content and rebuild with leaves >= 50% fill
    std::string utf8;
    reinterpret_cast<const ProtoString*>(strObj)->toUTF8String(ctx, utf8);
    return ProtoStringImplementation::fromUTF8Bytes(
        ctx,
        reinterpret_cast<const uint8_t*>(utf8.data()),
        utf8.size());
}

const ProtoObject* SymbolTable::intern(ProtoContext* ctx,
                                        const ProtoObject* strObj,
                                        bool is_strong) {
    // Already a symbol or inline (both are unique by definition)
    if (!strObj) return strObj;
    ProtoObjectPointer pa{}; pa.oid = strObj;
    if (pa.op.pointer_tag == POINTER_TAG_SYMBOL) return strObj;
    if (pa.op.pointer_tag == POINTER_TAG_EMBEDDED_VALUE) return strObj;

    // Get content hash from AVL root
    auto* impl = getImpl(strObj);
    uint64_t hash = impl ? impl->implGetHash() : 0;
    int shard_idx = shardIndex(hash);
    Shard& shard = shards[shard_idx];

    {
        std::lock_guard<std::mutex> lock(shard.mutex);

        // Search existing symbols in this shard
        for (Bucket* b = shard.head; b; b = b->next) {
            if (b->content_hash == hash &&
                contentEqual(ctx, b->symbol, strObj))
                return b->symbol;
        }

        // Not found: normalize tree and create symbol
        auto* normalized = normalizeForSymbol(ctx, strObj);
        const ProtoObject* symbol = reinterpret_cast<const ProtoObject*>(
            normalized->implAsSymbol(ctx));

        Bucket* bucket = new Bucket{ hash, symbol, is_strong, shard.head };
        shard.head = bucket;
        return symbol;
    }
}

void SymbolTable::removeWeak(uint64_t content_hash, const ProtoObject* symbol) {
    int shard_idx = shardIndex(content_hash);
    Shard& shard  = shards[shard_idx];
    std::lock_guard<std::mutex> lock(shard.mutex);

    Bucket** prev = &shard.head;
    for (Bucket* b = shard.head; b; b = b->next) {
        if (b->symbol == symbol && !b->is_strong) {
            *prev = b->next;
            delete b;
            return;
        }
        prev = &b->next;
    }
}

} // namespace proto
```

- [ ] **Step 4: Add `SymbolTable.cpp` to `CMakeLists.txt`**

In `protoCore/CMakeLists.txt`, find `add_library(protoCore SHARED ...` and add `core/SymbolTable.cpp` to the list.

- [ ] **Step 5: Implement `ProtoString::createSymbol` in `core/ProtoString.cpp`**

```cpp
const ProtoString* ProtoString::createSymbol(ProtoContext* ctx, const char* utf8) {
    if (!utf8 || !*utf8)
        return reinterpret_cast<const ProtoString*>(createInlineStringUTF8(ctx, nullptr, 0));
    size_t len = std::strlen(utf8);
    if (len <= INLINE_STRING_MAX_BYTES)
        return reinterpret_cast<const ProtoString*>(
            createInlineStringUTF8(ctx, reinterpret_cast<const uint8_t*>(utf8),
                                   static_cast<uint8_t>(len)));
    // Build non-interned first, then intern as strong symbol
    auto* impl = ProtoStringImplementation::fromUTF8Bytes(
        ctx, reinterpret_cast<const uint8_t*>(utf8), len);
    const ProtoObject* str_obj = impl->implAsObject(ctx);
    const ProtoObject* sym = ctx->space->symbolTable->intern(ctx, str_obj, /*strong=*/true);
    return reinterpret_cast<const ProtoString*>(sym);
}

const ProtoString* ProtoString::createSymbol(ProtoContext* ctx,
                                               const ProtoList* codepoints) {
    // Convert list to utf-8 string then intern
    const ProtoString* tmp = ProtoString::create(ctx, codepoints);
    auto* obj = reinterpret_cast<const ProtoObject*>(tmp);
    const ProtoObject* sym = ctx->space->symbolTable->intern(ctx, obj, true);
    return reinterpret_cast<const ProtoString*>(sym);
}

const ProtoString* ProtoString::createSymbol(ProtoContext* ctx,
                                               const std::string& s) {
    return ProtoString::createSymbol(ctx, s.c_str());
}
```

- [ ] **Step 6: Run tests**

```bash
cmake --build build -j$(nproc) && ./build/proto_tests --gtest_filter="SymbolTest.*"
```
Expected: 5 tests PASSED (skip AutoIntern test if ProtoObject accessor not yet available).

- [ ] **Step 7: Commit**

```bash
git add core/SymbolTable.cpp core/ProtoString.cpp headers/proto_internal.h \
        CMakeLists.txt test/test_string.cpp
git commit -m "feat(strings): implement SymbolTable with 64-shard interning and createSymbol"
```

---

## Task 11: ProtoSpace Integration

**Files:**
- Modify: `headers/protoCore.h` — add `symbolTable` to `ProtoSpace`
- Modify: `core/ProtoSpace.cpp` — init SymbolTable, register as GC root, init literal Symbols

- [ ] **Step 1: Add `symbolTable` member to `ProtoSpace` in `headers/protoCore.h`**

Find the `ProtoSpace` class and add after existing members:

```cpp
// String interning table — replaces TupleDictionary for strings
proto::SymbolTable* symbolTable{};
```

Since `SymbolTable` is defined in `proto_internal.h`, add a forward declaration in `protoCore.h`:

```cpp
// Near the top of protoCore.h, with other forward declarations:
namespace proto { class SymbolTable; }
```

- [ ] **Step 2: Init SymbolTable in `ProtoSpace` constructor in `core/ProtoSpace.cpp`**

Find the `ProtoSpace::ProtoSpace()` constructor and add:

```cpp
symbolTable = new SymbolTable();
```

Find the `ProtoSpace::~ProtoSpace()` destructor and add:

```cpp
delete symbolTable;
symbolTable = nullptr;
```

- [ ] **Step 3: Init cached literals as Symbols**

Find where `literalData`, `literalSetAttribute`, and `literalCallMethod` are initialized.
Change each to use `ProtoString::createSymbol`:

```cpp
// Find existing assignments like:
//   literalData = ProtoString::fromUTF8String(rootContext, "data");
// Replace with:
literalData         = ProtoString::createSymbol(rootContext, "data");
literalSetAttribute = ProtoString::createSymbol(rootContext, "setAttribute");
literalCallMethod   = ProtoString::createSymbol(rootContext, "callMethod");
```

- [ ] **Step 4: Register SymbolTable shards as GC roots in `analyzeUsedCells` or equivalent**

Find the GC marking pass (likely in `ProtoSpace.cpp` or `gcThreadLoop`). Add:

```cpp
// Mark all strong symbols in SymbolTable
for (int i = 0; i < SymbolTable::SHARD_COUNT; ++i) {
    std::lock_guard<std::mutex> lock(symbolTable->shards[i].mutex);
    for (auto* b = symbolTable->shards[i].head; b; b = b->next) {
        if (b->is_strong) {
            markCell(reinterpret_cast<const Cell*>(
                // strip tag to get raw cell pointer
                [&]{ ProtoObjectPointer pa{}; pa.oid = b->symbol;
                     pa.op.pointer_tag = 0;
                     return reinterpret_cast<const Cell*>(pa.oid); }()
            ));
        }
        // Weak symbols: mark only if referenced elsewhere (standard tracing covers this)
    }
}
```

- [ ] **Step 5: Build and run full test suite**

```bash
cmake --build build -j$(nproc) && ctest --test-dir build -j$(nproc) --output-on-failure
```
Expected: all tests pass.

- [ ] **Step 6: Commit**

```bash
git add headers/protoCore.h core/ProtoSpace.cpp
git commit -m "feat(strings): integrate SymbolTable into ProtoSpace; init cached literals as Symbols"
```

---

## Task 12: ProtoObject Auto-Interning

**Files:**
- Modify: `core/ProtoObject.cpp`
- Modify: `test/test_string.cpp`

- [ ] **Step 1: Add failing test**

```cpp
TEST_F(SymbolTest, AutoInternOnSetAttribute) {
    // Create an object, set attribute with a non-interned string key
    auto* obj = proto::ProtoObject::create(c);
    auto* non_interned_key = ProtoString::fromUTF8(c, "myKey");

    // Key should NOT be a symbol before setAttribute
    EXPECT_FALSE(SymbolTable::isSymbol(
        reinterpret_cast<const ProtoObject*>(non_interned_key)));

    auto* val = c->space->objectPrototype;
    obj->setAttribute(c, non_interned_key, val);

    // After setAttribute, retrieving the value with a Symbol key must work
    auto* sym_key = ProtoString::createSymbol(c, "myKey");
    auto* retrieved = obj->getAttribute(c, sym_key);
    EXPECT_EQ(retrieved, val);
}
```

- [ ] **Step 2: Find `setAttribute` / `implSetAttribute` in `core/ProtoObject.cpp`**

Look for the method that inserts into the object's `SparseList`. It likely receives a
`const ProtoString* key`. Add auto-interning before the SparseList insert:

```cpp
// In ProtoObject::setAttribute or implSetAttribute, before the SparseList call:
// Auto-intern the key if it is a non-interned String
{
    auto* key_obj = reinterpret_cast<const ProtoObject*>(key);
    ProtoObjectPointer pa{}; pa.oid = key_obj;
    if (pa.op.pointer_tag == POINTER_TAG_STRING) {  // non-interned
        const ProtoObject* sym = context->space->symbolTable->intern(
            context, key_obj, /*strong=*/false);
        key = reinterpret_cast<const ProtoString*>(sym);
    }
    // Embedded (POINTER_TAG_EMBEDDED_VALUE + INLINE_STRING) and Symbol
    // (POINTER_TAG_SYMBOL) are already unique — no action needed.
}
```

- [ ] **Step 3: Run the auto-intern test**

```bash
cmake --build build -j$(nproc) && ./build/proto_tests --gtest_filter="SymbolTest.AutoIntern*"
```
Expected: PASSED.

- [ ] **Step 4: Run full suite**

```bash
ctest --test-dir build -j$(nproc) --output-on-failure
```

- [ ] **Step 5: Commit**

```bash
git add core/ProtoObject.cpp test/test_string.cpp
git commit -m "feat(strings): auto-intern non-interned String keys in ProtoObject::setAttribute"
```

---

## Task 13: Public API Finalization and GC Marking

**Files:**
- Modify: `headers/protoCore.h`
- Modify: `core/ProtoString.cpp`
- Modify: `core/ProtoSpace.cpp` (GC marking for new cell tags)

- [ ] **Step 1: Update `ProtoString` class in `headers/protoCore.h`**

Replace the two existing factory methods with the full new API:

```cpp
class ProtoString
{
public:
    // --- Non-interned String creation (for text processing, file content) ---
    static const ProtoString* fromUTF8(ProtoContext* context,
                                        const char* zeroTerminatedUtf8);

    static const ProtoString* fromUTF8Buffer(ProtoContext* context,
                                              const uint8_t* buf, size_t len,
                                              const uint8_t* pending,
                                              uint8_t pending_count,
                                              uint8_t* out_remainder,
                                              uint8_t* out_remainder_count);

    static const ProtoString* create(ProtoContext* context,
                                      const ProtoList* codepoints);  // existing

    static const ProtoString* fromCodepointTuple(ProtoContext* context,
                                                  const ProtoTuple* codepoints);

    static const ProtoString* fromStdString(ProtoContext* context,
                                             const std::string& s);

    // --- Interned Symbol creation (for identifiers, attribute keys) ---
    static const ProtoString* createSymbol(ProtoContext* context,
                                            const char* zeroTerminatedUtf8);

    static const ProtoString* createSymbol(ProtoContext* context,
                                            const ProtoList* codepoints);

    static const ProtoString* createSymbol(ProtoContext* context,
                                            const std::string& s);

    // --- All operations (identical for all three internal types) ---
    int           cmp_to_string(ProtoContext*, const ProtoString*) const;
    const ProtoObject*   getAt(ProtoContext*, int index) const;
    unsigned long        getSize(ProtoContext*) const;
    const ProtoString*   getSlice(ProtoContext*, int from, int to) const;
    const ProtoString*   setAt(ProtoContext*, int index, const ProtoObject* character) const;
    const ProtoString*   insertAt(ProtoContext*, int index, const ProtoObject* character) const;
    const ProtoString*   setAtString(ProtoContext*, int index, const ProtoString*) const;
    const ProtoString*   insertAtString(ProtoContext*, int index, const ProtoString*) const;
    const ProtoString*   appendFirst(ProtoContext*, const ProtoString*) const;
    const ProtoString*   appendLast(ProtoContext*, const ProtoString*) const;
    const ProtoString*   splitFirst(ProtoContext*, int count) const;
    const ProtoString*   splitLast(ProtoContext*, int count) const;
    const ProtoString*   removeFirst(ProtoContext*, int count) const;
    const ProtoString*   removeLast(ProtoContext*, int count) const;
    const ProtoString*   removeAt(ProtoContext*, int index) const;
    const ProtoString*   removeSlice(ProtoContext*, int from, int to) const;
    const ProtoString*   multiply(ProtoContext*, const ProtoObject* count) const;
    const ProtoObject*   modulo(ProtoContext*, const ProtoObject* other) const;
    const ProtoObject*   asObject(ProtoContext*) const;
    const ProtoList*     asList(ProtoContext*) const;
    const ProtoStringIterator* getIterator(ProtoContext*) const;
    unsigned long        getHash(ProtoContext*) const;
    const ProtoString*   isCell(ProtoContext*) const;
    const Cell*          asCell(ProtoContext*) const;
    void                 toUTF8String(ProtoContext*, std::string& out) const;
    std::string          toStdString(ProtoContext*) const;

    // @deprecated — use fromUTF8() instead
    static const ProtoString* fromUTF8String(ProtoContext* context,
                                              const char* utf8)  {
        return fromUTF8(context, utf8);
    }
};
```

- [ ] **Step 2: Implement `fromUTF8Buffer`, `fromCodepointTuple`, `fromStdString`, `toStdString` in `core/ProtoString.cpp`**

```cpp
const ProtoString* ProtoString::fromStdString(ProtoContext* ctx, const std::string& s) {
    return ProtoString::fromUTF8(ctx, s.c_str());
}

std::string ProtoString::toStdString(ProtoContext* ctx) const {
    std::string out;
    toUTF8String(ctx, out);
    return out;
}

const ProtoString* ProtoString::fromUTF8Buffer(ProtoContext* ctx,
                                                const uint8_t* buf, size_t len,
                                                const uint8_t* pending,
                                                uint8_t pending_count,
                                                uint8_t* out_remainder,
                                                uint8_t* out_remainder_count) {
    // Find last complete UTF-8 boundary
    size_t valid_end = len;
    while (valid_end > 0 && (buf[valid_end - 1] & 0xC0) == 0x80)
        --valid_end;
    if (valid_end > 0) {
        uint8_t lead     = buf[valid_end - 1];
        int     expected = (lead < 0xE0) ? 2 : (lead < 0xF0) ? 3 : 4;
        int     have     = static_cast<int>(len - (valid_end - 1));
        if (have < expected) --valid_end;
    }

    // Combine pending + valid portion
    std::vector<uint8_t> full;
    full.reserve(pending_count + valid_end);
    if (pending_count) full.insert(full.end(), pending, pending + pending_count);
    full.insert(full.end(), buf, buf + valid_end);

    // Remainder
    *out_remainder_count = static_cast<uint8_t>(len - valid_end);
    if (*out_remainder_count)
        std::memcpy(out_remainder, buf + valid_end, *out_remainder_count);

    if (full.empty())
        return reinterpret_cast<const ProtoString*>(createInlineStringUTF8(ctx, nullptr, 0));
    auto* impl = ProtoStringImplementation::fromUTF8Bytes(ctx, full.data(), full.size());
    return reinterpret_cast<const ProtoString*>(
        full.size() <= INLINE_STRING_MAX_BYTES
            ? createInlineStringUTF8(ctx, full.data(), static_cast<uint8_t>(full.size()))
            : impl->implAsObject(ctx));
}

const ProtoString* ProtoString::fromCodepointTuple(ProtoContext* ctx,
                                                    const ProtoTuple* tuple) {
    // Convert tuple of embedded unicode chars to UTF-8 string
    unsigned long sz = tuple->getSize(ctx);
    std::string utf8;
    utf8.reserve(sz * 3);  // worst case 3 bytes per char
    for (unsigned long i = 0; i < sz; ++i) {
        const ProtoObject* ch = tuple->getAt(ctx, static_cast<int>(i));
        ProtoObjectPointer pa{}; pa.oid = ch;
        uint32_t cp = static_cast<uint32_t>(pa.unicodeChar.unicodeValue);
        if (cp < 0x80)        utf8 += static_cast<char>(cp);
        else if (cp < 0x800)  { utf8 += char(0xC0|(cp>>6)); utf8 += char(0x80|(cp&0x3F)); }
        else if (cp < 0x10000){ utf8 += char(0xE0|(cp>>12)); utf8 += char(0x80|((cp>>6)&0x3F));
                                utf8 += char(0x80|(cp&0x3F)); }
        else                  { utf8 += char(0xF0|(cp>>18)); utf8 += char(0x80|((cp>>12)&0x3F));
                                utf8 += char(0x80|((cp>>6)&0x3F)); utf8 += char(0x80|(cp&0x3F)); }
    }
    return ProtoString::fromUTF8(ctx, utf8.c_str());
}
```

- [ ] **Step 3: Add GC marking cases for new cell tags in `ProtoSpace.cpp`**

Find `analyzeUsedCells` (or the function that marks child pointers for the GC).
Add cases for the new tags:

```cpp
case POINTER_TAG_STRING_LEAF_NODE: {
    // StringLeafNode has no child pointers — just mark the cell itself.
    // The cell is already being processed; nothing further to scan.
    break;
}
case POINTER_TAG_STRING_INTERNAL_NODE: {
    ProtoObjectPointer raw{}; raw.oid = cell_obj;
    raw.op.pointer_tag = 0;
    auto* node = reinterpret_cast<const StringInternalNode*>(raw.oid);
    if (node->left)  markAndPush(node->left);
    if (node->right) markAndPush(node->right);
    break;
}
case POINTER_TAG_SYMBOL: {
    // ProtoStringImplementation — same as POINTER_TAG_STRING: mark avl_root
    ProtoObjectPointer raw{}; raw.oid = cell_obj;
    raw.op.pointer_tag = 0;
    auto* impl = reinterpret_cast<const ProtoStringImplementation*>(raw.oid);
    if (impl->avl_root) markAndPush(impl->avl_root);
    break;
}
```

Also update the existing `POINTER_TAG_STRING` case to use `avl_root` instead of the old `tuple` field.

- [ ] **Step 4: Run full test suite**

```bash
cmake --build build -j$(nproc) && ctest --test-dir build -j$(nproc) --output-on-failure
```
Expected: all tests pass, including GCStressTests.

- [ ] **Step 5: Commit**

```bash
git add headers/protoCore.h core/ProtoString.cpp core/ProtoSpace.cpp
git commit -m "feat(strings): finalize public API; add GC marking for StringLeafNode and StringInternalNode"
```

---

## Task 14: Buffer Boundary Tests + fromUTF8Buffer Verification

**Files:**
- Modify: `test/test_string.cpp`

- [ ] **Step 1: Add buffer boundary tests**

```cpp
TEST_F(StringTest, BufferBoundaryCleanUTF8) {
    const char* src = "hello world";
    uint8_t rem[3]; uint8_t rem_cnt;
    auto* s = ProtoString::fromUTF8Buffer(
        c, reinterpret_cast<const uint8_t*>(src), 11,
        nullptr, 0, rem, &rem_cnt);
    EXPECT_EQ(rem_cnt, 0u);
    std::string out; s->toUTF8String(c, out);
    EXPECT_EQ(out, "hello world");
}

TEST_F(StringTest, BufferBoundarySplitTwoByte) {
    // é = 0xC3 0xA9 — split between the two bytes
    const uint8_t buf1[] = {'a', 0xC3};           // first byte of é
    const uint8_t buf2[] = {0xA9, 'b'};            // second byte of é + 'b'
    uint8_t rem[3]; uint8_t rem_cnt;

    auto* s1 = ProtoString::fromUTF8Buffer(c, buf1, 2, nullptr, 0, rem, &rem_cnt);
    EXPECT_EQ(rem_cnt, 1u);
    EXPECT_EQ(rem[0], 0xC3);
    std::string out1; s1->toUTF8String(c, out1);
    EXPECT_EQ(out1, "a");

    auto* s2 = ProtoString::fromUTF8Buffer(c, buf2, 2, rem, rem_cnt, rem, &rem_cnt);
    EXPECT_EQ(rem_cnt, 0u);
    std::string out2; s2->toUTF8String(c, out2);
    EXPECT_EQ(out2, "\xC3\xA9" "b");  // é + b
}

TEST_F(StringTest, BufferBoundarySplitThreeByte) {
    // U+4E2D (中) = 0xE4 0xB8 0xAD — split after first byte
    const uint8_t buf1[] = {0xE4};
    const uint8_t buf2[] = {0xB8, 0xAD};
    uint8_t rem[3]; uint8_t rem_cnt;

    ProtoString::fromUTF8Buffer(c, buf1, 1, nullptr, 0, rem, &rem_cnt);
    EXPECT_EQ(rem_cnt, 1u);

    auto* s = ProtoString::fromUTF8Buffer(c, buf2, 2, rem, rem_cnt, rem, &rem_cnt);
    EXPECT_EQ(rem_cnt, 0u);
    EXPECT_EQ(s->getSize(c), 1u);
    uint32_t cp; strCharAt(getRoot(c, reinterpret_cast<const ProtoObject*>(s)), 0, &cp);
    EXPECT_EQ(cp, 0x4E2Du);
}
```

- [ ] **Step 2: Run buffer tests**

```bash
cmake --build build -j$(nproc) && ./build/proto_tests --gtest_filter="StringTest.Buffer*"
```
Expected: 3 tests PASSED.

- [ ] **Step 3: Commit**

```bash
git add test/test_string.cpp
git commit -m "test(strings): add UTF-8 buffer boundary split tests"
```

---

## Task 15: Final Integration — GC Stress and Full Suite

**Files:**
- Modify: `test/test_string.cpp` (add concurrent test)

- [ ] **Step 1: Add concurrent symbol interning test**

```cpp
TEST_F(SymbolTest, ConcurrentInternSamePointer) {
    // 8 threads all trying to intern "sharedKey" simultaneously
    // must all receive the same pointer
    const int NTHREADS = 8;
    std::vector<const ProtoString*> results(NTHREADS, nullptr);
    std::vector<std::thread> threads;

    for (int i = 0; i < NTHREADS; ++i) {
        threads.emplace_back([&, i]() {
            proto::ProtoContext thread_ctx{&space};
            results[i] = ProtoString::createSymbol(&thread_ctx, "sharedKey");
        });
    }
    for (auto& t : threads) t.join();

    for (int i = 1; i < NTHREADS; ++i)
        EXPECT_EQ(results[0], results[i])
            << "Thread " << i << " got a different pointer";
}
```

- [ ] **Step 2: Run GC stress tests**

```bash
cmake --build build -j$(nproc) && ./build/proto_tests --gtest_filter="GCStress*:Swarm*:StringTest*:SymbolTest*:StringAVLTest*" --gtest_repeat=3
```
Expected: all pass on all 3 repetitions.

- [ ] **Step 3: Run ThreadSanitizer build (if available)**

```bash
cmake -B build_tsan -S . -DCMAKE_CXX_FLAGS="-fsanitize=thread -g"
cmake --build build_tsan -j$(nproc)
./build_tsan/proto_tests --gtest_filter="SymbolTest.Concurrent*:GCStress*"
```
Expected: no data races reported.

- [ ] **Step 4: Run full suite one final time**

```bash
ctest --test-dir build -j$(nproc) --output-on-failure
```
Expected: 100% pass rate.

- [ ] **Step 5: Final commit**

```bash
git add test/test_string.cpp
git commit -m "test(strings): add concurrent symbol interning test; string refactoring complete"
```

---

## Self-Review Against Spec

| Spec Section | Covered By |
|---|---|
| §2 Three-tier architecture (Embedded/Symbol/String) | Tasks 1, 9, 10 |
| §3 StringLeafNode (48-byte UTF-8, is_partial, hash) | Task 2 |
| §3 StringInternalNode (AVL, subtree_hash, left_chars) | Task 3 |
| §4 AVL rebalance (LL/LR/RR/RL) | Task 4 |
| §4 concat + split primitives | Tasks 4, 5 |
| §4 charAt, append, slice, insert, delete | Tasks 5, 7 |
| §4 Lazy leaf merge (25% threshold) | Task 7 (in rebalance) |
| §5 Embedded UTF-8 6-byte encoding | Task 9 |
| §6 SymbolTable 64 shards | Task 10 |
| §6 Auto-interning via setAttribute | Task 12 |
| §6 Weak/strong symbol lifetime | Task 10 |
| §7 Comparison semantics (hash fast-path) | Task 7 |
| §7 Symbol vs String content equality | Tasks 10, 14 |
| §8 Forward/reverse codepoint iterator | Task 8 |
| §8 charAt indexed access | Task 5 |
| §8 Grapheme clusters out of scope | Non-goal, no task needed |
| §9 fromUTF8, fromUTF8Buffer | Tasks 9, 13 |
| §9 fromStdString, toStdString | Task 13 |
| §9 fromCodepointTuple | Task 13 |
| §9 createSymbol overloads | Task 10 |
| §9 fromUTF8String deprecated | Task 13 |
| §10 GC marking for new cell types | Task 13 |
| §10 SymbolTable as GC root | Task 11 |
| §10 Path copying; no write barrier | Structural (immutability, no task) |
| §11 Files affected | All tasks |
| §12 Non-goals (no StringBuilder, no NFC) | Not implemented |
