# 01 — Sequences built at the front

Example: [`examples/linked-list/01_front_built_sequences.cpp`](../../../examples/linked-list/01_front_built_sequences.cpp),
test `example.linked_list.01_front_built_sequences`. Requires protoCore
**3.3.0** or later.

Many sequences in a language runtime are built once at the front, read at
their head and dropped: the values a function returns, a Lisp list, the
arguments a method keeps. `ProtoLinkedList` is the type for them.

## Which sequence to use

| Type | Built | Read | Collected? | Use it for |
|---|---|---|---|---|
| `ProtoLinkedList` | at the front, 1 cell per `prepend` | head, O(1); `getAt(i)` walks i / 4 cells | yes | returned values, Lisp lists, kept arguments |
| `ProtoList` | anywhere, O(log n) | anywhere, O(log n) | yes | indexed sequences, Python tuples and lists, vectors |
| `ProtoTuple` | once | O(log n) | **no**: every tuple is interned and perennial | canonical constants and keys compared by pointer |

A `ProtoTuple` is never collected. Do not build one per operation: protoPython
did, and every call that built a tuple kept about ten cells for good.

## The representation

A list is a chain of 64-byte cells of up to four elements. Each cell also
holds the count of its whole suffix, so `getSize` is O(1) and every suffix is
a list. Every cell is full except the head:

```
(1 2 3 4 5 6)   ->   [1 2] -> [3 4 5 6]
```

`prepend` copies a partial head with the new element in front, or adds a new
one-element head when the head is full; it allocates one cell either way. `getRest`
returns the next cell when the head holds one element, and otherwise a copy
of the head without its first element (one cell). The values after the head
are never copied: versions share their tails.

## Returning several values

```cpp
const ProtoLinkedList* divmod(ProtoContext* ctx, long long a, long long b) {
    const ProtoObject* values[] = {ctx->fromInteger(a / b), ctx->fromInteger(a % b)};
    return ctx->newLinkedListFromArray(2, values);
}
```

Up to four values cost one cell, and the cell is garbage as soon as the caller
has read them.

## A Lisp list

```cpp
const ProtoLinkedList* list = ctx->newLinkedList();          // ()
for (int i = 3; i >= 1; --i) list = list->prepend(ctx, ctx->fromInteger(i));
list->getSize(ctx);                  // (count l)  -> 3
list->getFirst(ctx);                 // (first l)  -> 1
list->getRest(ctx)->getFirst(ctx);   // (second l) -> 2
```

`newLinkedList()` is the one empty list of the process; `getRest` of a
one-element list returns it.

## Walking without allocating

```cpp
ProtoLinkedListCursor cursor(list);
while (cursor.hasNext()) total += cursor.next()->asLong(ctx);
```

The cursor is a C++ value in a local, not a cell. It is valid while the list
it started from is reachable. Walking with `getRest` instead allocates one
cell in three steps out of four, so a runtime walks with the cursor wherever
the program does not ask for the rest list itself.

## When identity matters

Two lists with the same elements are distinct values; `isEqual` compares
their element words, and `getHash` is the hash of the tuple with the same
elements. When a canonical value is needed (a key compared by pointer),
`asTuple` returns the interned tuple, the same for equal lists:

```cpp
equal->asTuple(ctx) == list->asTuple(ctx)   // true
```

Interned tuples are perennial, so this is for keys and constants, not for
every list.

## Requiring the version

A runtime that uses the type requires protoCore 3.3.0:

```cmake
find_package(protoCore 3.3 REQUIRED)
```

A binary that calls these functions does not load against an older 3.x
library: the dynamic loader reports the undefined symbol at start-up. A
Debian package that uses the type declares `Depends: protocore (>= 3.3.0)`.
