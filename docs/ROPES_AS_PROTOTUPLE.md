# Strings as ProtoTuple Only (Ropes)

## Overview

ProtoString is implemented exclusively using **ProtoTuple** and tagged pointers. No dedicated rope node type exists. Long strings are either inline (up to 7 characters in the pointer) or a tree of **ProtoTuple** cells.

## Representations

### 1. Inline string (no cell)

- **Tag**: `POINTER_TAG_EMBEDDED_VALUE` with `EMBEDDED_TYPE_INLINE_STRING`.
- **Payload**: Up to 7 logical UTF-32 code units in the 54-bit value (e.g. length in low bits, then 7×7-bit code units for ASCII 0–127).
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

- **fromUTF8String**: If decoded length ≤ 7 and all code points in 0..127, build inline representation and return (no cell, no intern). Otherwise build leaf tuple as today, then intern and return.
- **appendLast**: Create concat tuple with left = this, right = other, size = sum; wrap in ProtoStringImplementation and return.
38: 
39: ## Comparison
40: 
41: String comparison is lexicographical (Unicode code point by code point) and optimized for rope structures. Instead of repeatedly descending the tree for each character ($O(N \log N)$), it uses a `RopeCharacterIterator` that maintains a traversal stack, achieving $O(N)$ performance for full string comparisons. Use `compareStrings(context, s1, s2)` for efficient comparison.
