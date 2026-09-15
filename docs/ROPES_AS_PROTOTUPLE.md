# Strings as ProtoTuple Only (Ropes)

> **Superseded (note added 2026-09-15).** This note describes an earlier string representation and does not match the current code. `ProtoString` now uses a three-tier representation: strings of up to 6 UTF-8 bytes embedded in the pointer (`INLINE_STRING_MAX_BYTES` in `headers/proto_internal.h`), interned symbols, and heap strings backed by a persistent AVL tree of `StringLeafNode` and `StringInternalNode` cells. `ProtoString` no longer uses tuples. Current description: [DESIGN.md § 2](../DESIGN.md#2-the-data-model-immutable-and-efficient). Design specification: [2026-03-31-string-refactoring-design.md](archive/design-specs/2026-03-31-string-refactoring-design.md).

## Overview

In this design, ProtoString was implemented exclusively using **ProtoTuple** and tagged pointers, with no dedicated rope node type. A string was either inline (up to 7 ASCII characters in the pointer in this design; the current inline tier holds up to 6 UTF-8 bytes) or a tree of **ProtoTuple** cells.

## Representations

### 1. Inline string (no cell)

- **Tag**: `POINTER_TAG_EMBEDDED_VALUE` with `EMBEDDED_TYPE_INLINE_STRING`.
- **Payload** (this design): up to 7 code units in the range 0–127, packed as 7 bits each after a length field. The current inline tier stores up to 6 UTF-8 bytes instead.
- **Invariant**: Zero allocation; all data in the pointer word.

### 2. Leaf (tuple of characters)

- **ProtoStringImplementation** holds a **ProtoTupleImplementation** whose slots are character objects (embedded Unicode or similar).
- **Convention**: `actual_size` is the number of characters; `implGetAt(index)` returns `slot[index]` for leaf tuples.

### 3. Concat (tuple of two strings)

- **ProtoStringImplementation** holds a **ProtoTupleImplementation** with exactly 2 slots: `slot[0]` = left string (inline or tuple-backed), `slot[1]` = right string; `actual_size` = total logical length.
- **Concatenation**: Allocate one new ProtoTuple via `tupleConcat(context, left, right, leftSize + rightSize)`; do not intern. O(1), no character copy.
- **Convention**: When `actual_size == 2` and both slots are strings, `ProtoStringImplementation::implGetAt` descends by index (if index < leftSize then getAt(left, index), else getAt(right, index - leftSize)).

## 64-byte alignment

Every node is a Cell (ProtoTupleImplementation); protoCore enforces 64-byte cells. No new layout.

## GC

ProtoTuple already has `processReferences` that visits slot references. Concat tuples reference two string objects (inline or cells); only cell pointers are traced. Inline strings have no references.

## Creation

- **fromUTF8String** (this design): if the decoded length is at most 7 and all code points are in 0..127, build the inline representation and return (no cell, no intern). Otherwise build a leaf tuple, then intern and return.
- **appendLast**: Create concat tuple with left = this, right = other, size = sum; wrap in ProtoStringImplementation and return.

## Comparison

String comparison is lexicographical (Unicode code point by code point) and optimized for rope structures. Instead of repeatedly descending the tree for each character ($O(N \log N)$), it uses a `RopeCharacterIterator` that maintains a traversal stack, achieving $O(N)$ performance for full string comparisons. Use `compareStrings(context, s1, s2)` for efficient comparison.
