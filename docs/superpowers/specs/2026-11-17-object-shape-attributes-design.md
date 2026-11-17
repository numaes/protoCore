# Object Attributes via Shared Shape and 4×6^d Value Tree — Design Specification

**Date:** 2026-11-17  
**Project:** `protoCore`  
**Branch:** `explore/mejora`

---

## 1. Motivation and Goals

Historically, `ProtoObjectCell` stored its own attributes in a persistent AVL tree (`ProtoSparseListImplementation`), where every attribute occupied a full 64-byte `Cell` (`key`, `value`, `previous`, `next`, `hash`, `size`, `height`), and every attribute write copied the root-to-key path ($O(\log n)$ allocations) plus a new `ProtoObjectCell`. Even an empty object allocated 2 cells (`ProtoObjectCell` + empty AVL node).

Furthermore, the per-thread `AttributeCache` indexed `(object_snapshot_ptr, name_symbol_ptr)`. When traversing a collection or tree of distinct objects sharing the same structure, every object missed in `AttributeCache` and forced an AVL lookup.

This design replaces the AVL attribute storage in `ProtoObjectCell` with:
1. **A singly-linked list of 64-byte `ProtoShape` cells** holding up to 4 interned attribute names per cell, shared across object versions and clones (`newClone` / `clone`).
2. **A per-thread `(ProtoShape*, ProtoString*) -> int64_t offset` cache (`shapeCache`)** that resolves attribute offsets in $O(1)$ across all objects sharing a shape or shape prefix.
3. **A dense value representation rooted inside `ProtoObjectCell`:**
   - For $n \le 4$ attributes (depth $d = 0$), the 4 values live directly inside `ProtoObjectCell::slots[0..3]` (**0 extra cells**).
   - For $n > 4$ attributes, `ProtoObjectCell::slots[0..3]` point to a $4 \times 6^d$ tree of 64-byte indirect cells (`ProtoAttrBranchCell` with 6 child pointers, `ProtoAttrLeafCell` with 6 `ProtoObject*` values), where the indirection depth $d$ is determined strictly by the attribute count $n$.
   - Accessors for depths $d = 0, 1, 2, 3$ ($n \le 4, 24, 144, 864$) are fully unrolled with compile-time divisors (`6`, `36`, `216`), eliminating loop branches and hardware division instructions (`idiv`) on the hot path.

---

## 2. 64-Byte Cell Layouts

Every internal cell in `protoCore` is 64 bytes (8 words of 8 bytes), starting with the 16-byte `Cell` header (`vptr` + `next_and_flags`), leaving 6 words (48 bytes) of payload.

### 2.1 `ProtoShape` (`CellType::Shape`, 64 bytes, internal-only)
```cpp
class ProtoShape final : public Cell {
public:
    const proto_ulong count;          // Total attributes in chain up to this block
    const ProtoShape* const next;     // Next block towards older (lower-offset) attributes
    const ProtoString* names[4];      // Filled from names[3] down to names[0]; unused = nullptr
};
```
- **Packing invariant:** Slots are filled from `names[3]` towards `names[0]`. Unused leading slots are `nullptr` (avoiding a separate slot-count field).
- **Intrinsic offset formula:** For any non-null `b->names[i]` ($i \in \{0, 1, 2, 3\}$) in block `b`:
  $$\text{offset}(b, i) = ((b\text{->count} - 1) \ \& \ \sim 3\text{UL}) + (3 - i)$$
  Because new attributes are prepended at the front of the `ProtoShape` chain with $\text{offset} = \text{new\_count} - 1$, every previously added attribute keeps its original offset $0 \dots \text{count}-1$ permanently, regardless of how many attributes are later added in front of `b`.
- **Internal cell:** `ProtoShape` is never returned to embedders as a `ProtoObject*`; `processReferences` traces `next`.

### 2.2 `ProtoObjectCell` (`CellType::Object`, 64 bytes, immutable object cell)
```cpp
class ProtoObjectCell : public Cell {
public:
    const ParentLinkImplementation* parent; // Word 2 (8 B)
    const ProtoShape* shape;                // Word 3 (8 B): nullptr when count == 0
    union {                                 // Words 4..7 (32 B)
        const ProtoObject* values[4];       // Used when shape && shape->count <= 4
        const Cell* blocks[4];              // Used when shape && shape->count > 4
        uintptr_t rawSlots[4];
    };
};
```
- **Empty immutable object (`count == 0`):** `shape == nullptr`, `rawSlots[0..3] = 0`. Allocates **1 single cell** (down from 2 cells).

### 2.3 `ProtoMutableObjectCell` (`CellType::MutableObject`, 64 bytes)
```cpp
class ProtoMutableObjectCell final : public ProtoObjectCell {
public:
    // Inherits parent, shape (always nullptr), and 32-byte union area.
    // Stores mutable_ref in rawSlots[0].
};
```
- A mutable object handle always has `shape == nullptr` and `getType() == CellType::MutableObject`.
- Its live attributes and parent chain are stored in an immutable `ProtoObjectCell` snapshot published in `globalMutableShards[shard]` under `mutable_ref = rawSlots[0]` (or in `rawSlots[1]` as birth snapshot before any shard mutation if created via `clone(true)` / `newClone(true)`).
- Fast check on the read hot path: whenever `oc->shape != nullptr`, `oc` is guaranteed to be an immutable `ProtoObjectCell` with attributes—requiring **zero virtual calls**. Only when `oc->shape == nullptr` does the code check `oc->isMutableCell()` (`getType() == CellType::MutableObject`).

### 2.4 `ProtoLazyObjectCell` (`CellType::Object`, 64 bytes)
- Inherits `ProtoObjectCell` with `shape == nullptr` and places `producer`, `value`, and `stateEpoch` inside the 32-byte slot union so `sizeof(ProtoLazyObjectCell) == 64` continues to hold.

### 2.5 `ProtoAttrLeafCell` and `ProtoAttrBranchCell` (64 bytes each, internal-only)
```cpp
class ProtoAttrLeafCell final : public Cell {
public:
    const ProtoObject* values[6]; // 48 B: tagged ProtoObject* values (nullptr = absent/removed)
};

class ProtoAttrBranchCell final : public Cell {
public:
    const Cell* children[6];      // 48 B: pointers to child ProtoAttrBranchCell or ProtoAttrLeafCell
};
```
- Distinguished by their `vptr` (`CellType::AttrLeaf` vs `CellType::AttrBranch`) at zero payload cost so the GC marker (`processReferences`) and `MutableCycles` know whether the 6 entries are `ProtoObject*` handles or `const Cell*` pointers.

---

## 3. Indirection Depth and Unrolled Value Access ($4 \times 6^d$)

Given `count = shape->count`:
- **Depth $d = 0$ ($1 \le \text{count} \le 4$):**
  - Read: `oc->values[offset]`
  - Write (replace): 1 new `ProtoObjectCell` with `values[offset] = val`.
- **Depth $d = 1$ ($5 \le \text{count} \le 24$):**
  - Read: `static_cast<const ProtoAttrLeafCell*>(oc->blocks[offset / 6])->values[offset % 6]`
  - Write (replace): 1 new `ProtoAttrLeafCell` + 1 new `ProtoObjectCell`.
- **Depth $d = 2$ ($25 \le \text{count} \le 144$):**
  - Read:
    ```cpp
    const auto* b1 = static_cast<const ProtoAttrBranchCell*>(oc->blocks[offset / 36]);
    const unsigned r1 = offset % 36;
    const auto* leaf = static_cast<const ProtoAttrLeafCell*>(b1->children[r1 / 6]);
    return leaf->values[r1 % 6];
    ```
  - Write (replace): 1 new `ProtoAttrLeafCell` + 1 new `ProtoAttrBranchCell` + 1 new `ProtoObjectCell`.
- **Depth $d = 3$ ($145 \le \text{count} \le 864$):**
  - Unrolled with compile-time divisors `216`, `36`, `6`.
- **Depth $d \ge 4$ ($\text{count} > 864$):**
  - General loop over depth $d$ where $4 \cdot 6^{d-1} < \text{count} \le 4 \cdot 6^d$.

### Level Transitions on Append ($\text{offset} = \text{count}$, $\text{new\_count} = \text{count} + 1$)
- **$4 \to 5$ ($d: 0 \to 1$):** Allocates 1 `ProtoAttrLeafCell` initialized with `{oc->values[0], oc->values[1], oc->values[2], oc->values[3], newValue, nullptr}` and stores it at `newOc->blocks[0]`.
- **$24 \to 25$ ($d: 1 \to 2$), $144 \to 145$ ($d: 2 \to 3$), and in general $4 \cdot 6^d \to 4 \cdot 6^d + 1$ ($d \to d+1$):**
  - The 4 full subtrees in `oc->blocks[0..3]` cover offsets $0 \dots 4 \cdot 6^d - 1$, which are the exact first 4 children (`children[0..3]`) of `newOc->blocks[0]` at depth $d + 1$!
  - Therefore, a level transition **never copies existing blocks**: it allocates 1 `ProtoAttrBranchCell` at `newOc->blocks[0]` whose `children[0..3]` reuse `oc->blocks[0..3]` verbatim, and creates the new spine of $d$ nodes in `children[4]` holding `newValue` at index 0.

---

## 4. Per-Thread Shape Cache (`shapeCache`)

- Stored in `ProtoThreadExtension` (and stashed in `ProtoContext::shapeCache_` for 1-load access).
- Entry layout:
  ```cpp
  struct ShapeCacheEntry {
      const ProtoShape* shape;
      const ProtoString* name;
      proto_long offset; // >= 0: attribute index; -1: confirmed absent in this shape chain
  };
  ```
- Index computation:
  `idx = ((reinterpret_cast<uintptr_t>(shape) >> 6) ^ (reinterpret_cast<uintptr_t>(name) >> 4)) % SHAPE_CACHE_DEPTH`
- **Chain caching:** When `findAttributeOffset(context, shape, name)` misses on the head `shape`, it walks `b = shape; b != nullptr; b = b->next`. At each subsequent block `b`, it checks `shapeCache` first; if `(b, name)` hits, traversal stops immediately with that offset. Upon finding the attribute (or reaching the end with `-1`), every visited block `b` in the prefix is written to `shapeCache` with the resulting offset.
- **GC safety:** `ProtoThreadExtension::processReferences` marks `entry.shape` and `entry.name` for all non-null entries; `clearCachesAfterStopTheWorld` clears `shapeCache`.

---

## 5. Public API and Operations

- **`ProtoObject::newClone(ProtoContext* context, bool isMutable = false) const`** (and `ProtoObject::clone`):
  - Creates a sibling object sharing `parent`, `shape`, and `rawSlots[0..3]` in $O(1)$ (1 cell for immutable).
  - Subsequent `setAttribute` calls on existing attributes of any clone reuse `shape` and only path-copy the updated value slot.
- **`ProtoObject::removeAttribute(ProtoContext* context, const ProtoString* name) const`:**
  - Looks up `offset = findAttributeOffset(context, shape, name)`. If `offset < 0` or the value at `offset` is already `nullptr`, returns `this` unchanged (no allocation). Otherwise writes `nullptr` at `offset` while keeping `shape`.
- **`ProtoObject::setAttributes(ProtoContext* context, unsigned count, const ProtoString* const* names, const ProtoObject* const* values) const`:**
  - Interns names, deduplicates keeping last write per key, applies replacements and new-attribute shape extensions inside one `CriticalSection`, and publishes a single new `ProtoObjectCell`.
- **`ProtoObject::processOwnAttributes`:**
  - Walks the `ProtoShape` chain from oldest to newest (or in offset order `0 .. count-1`) and reads non-null values without heap allocation, keeping `WalkPin` for mutable snapshots.
- **`ProtoObject::getOwnAttributes` / `ProtoObject::getAttributes`:**
  - Builds the `ProtoSparseList*` representation on demand for external callers.

---

## 6. Performance Measurements (`protoCpp` `costmodel_kernel`)

Measured with `costmodel_kernel` (`PROTOCORE_ADAPTIVE_HEAP=1`, Release `-O3`, median of 5 batches) comparing `master` (3.6.0, persistent AVL attribute storage) against `explore/mejora` (`ProtoShape` + $4 \times 6^d$ value tree):

| Operation (`costmodel_kernel`) | Baseline AVL (ns) | Branch `ProtoShape` (ns) | Speedup (wall / CPU) | Baseline Cells/op | Branch Cells/op |
|---|---:|---:|---|---:|---:|
| `setattr_immutable_4_attrs` ($d = 0$) | 64.25 | **31.96** (min 30.49) | **2.01×** | 3.00 | **1.00** (−67%) |
| `setattr_immutable_16_attrs` ($d = 1$) | 97.19 | **47.96** (min 45.65) | **2.03×** | 4.36 | **2.00** (−54%) |
| `setattr_immutable_100_attrs` ($d = 2$) | 152.67 (CPU 293.55) | **71.93** (CPU 89.51) | **2.12× / 3.28× CPU** | 6.68 | **2.96** (−56%) |
| `clone_and_update_4_attrs` | 109.27 (CPU 215.41) | **55.69** (CPU 107.82) | **1.96× / 2.00× CPU** | 4.00 | **2.00** (−50%) |
| `setattr_mutable` ($n = 4$) | 141.93 (CPU 157.47) | **64.58** (CPU 89.08) | **2.20× / 1.77× CPU** | 4.00 | **2.00** (−50%) |
| `new_object_4_attrs` | 412.94 (CPU 805.85) | **311.15** (CPU 372.22) | **1.33× / 2.16× CPU** | 18.00 | **10.00** (−44%) |
| `getattr_cloned_nodes_1000` ($n = 16$) | 15.47 (CPU 30.48) | **11.97** (CPU 11.97) | **1.29× / 2.55× CPU** | 0.00 | **0.00** |
| `mutable_write_16_attrs_1_live` | 128.27 | **82.59** | **1.55×** | 5.37 | **3.00** (−44%) |
| `mutable_write_16_attrs_1000_live` | 488.33 | **288.63** | **1.69×** | 7.37 | **5.00** (−32%) |
| `mutable_write_16_attrs_100000_live` | 805.61 | **242.47** | **3.32×** | 13.37 | **11.00** (−18%) |

