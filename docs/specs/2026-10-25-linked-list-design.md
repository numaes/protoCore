# `ProtoLinkedList`: a collectable, chunked, singly linked list

Status: **approved 2026-10-25** (decisions D1-D8 as recommended, section 11);
implemented in protoCore 3.3.0 (additive; SOVERSION stays 4). Section 12
records what the implementation changed. Author: Gustavo Marino, with Claude.

## 1. Problem

protoCore has no small, immutable, collectable sequence built for the front:
a sequence that is consed onto, read at its head and dropped.

- **`ProtoTuple` is perennial.** Every tuple node is canonicalised by the
  `TupleInterner`, whose table is a GC root that never drops an entry
  (DESIGN.md, *Interning summary*). A tuple built per operation is a
  permanent allocation, and each build pays a hash, a shard mutex and a
  lookup. protoPython found this the hard way: since `29158d41` (v1.1.0) its
  run-time tuples are immutable objects over a `ProtoList` (*"a collectable
  tuple is a ProtoList"*). Before the change every call that built a tuple
  kept about 10 cells for good. `ProtoTuple` remains correct for what it is:
  a canonical constant (code-object constants, `__mro__`, keyword-name
  tuples).
- **`ProtoList` is built for random access.** It is an AVL tree with one
  node per element above five elements. `appendFirst` path-copies O(log n)
  nodes, and `removeAt(0)` does the same.
- **The runtimes build their own lists.** protoClojure's list (M44,
  `721e5c3`) is a chain of objects of four protoCore cells (256 bytes) per
  element, and its author asks protoCore for a one-cell immutable record.

The uses that need such a sequence:

- returning several values;
- Lisp lists (Clojure `PersistentList`, `keys`/`vals`, rest arguments);
- argument lists a method keeps;
- small sequences that are built once and read from the front.

## 2. Decision and scope

Add a **new type**, `ProtoLinkedList`. No existing type changes its model,
its API or its cost. This follows the platform rule *extend protoCore by a
new type, never change an existing model*.

Goals:

- The front operations are O(1): `prepend` (cons), `getFirst`, `getSize`.
  `rest` is O(1) in time.
- Density: four elements per 64-byte cell.
- Collectable: nothing is interned, and a list no longer referenced is
  garbage.
- Conversions to and from `ProtoList` and `ProtoTuple`. `asTuple` is the way
  to obtain a canonical, interned value when identity is needed.
- **No cost for a program that does not use the type** (section 9).

Non-goals:

- **Random access.** `getAt(i)` is O(i). A program that indexes uses
  `ProtoList`.
- **Operations at the end.** `appendLast` and `getLast` are not offered; a
  sequence that grows at the end is a `ProtoList`.
- **Replacing `ProtoTuple` or `ProtoList`.** Both are unchanged.
- **The general one-cell record** asked for by protoClojure M44 (pairs, map
  entries). A one-element `ProtoLinkedList` is a cons cell, but a record type
  is a separate decision.

## 3. Representation

### 3.1 The node cell

```
Cell base (16 bytes: vptr, next_and_flags)
proto_ulong  counts;        // bits 0-2: elements in this node (1..4)
                            // bits 3-63: elements in the whole list (this node and every node after it)
const ProtoLinkedListNode* next;   // nullptr in the last node
const ProtoObject* slot[4];         // slot[0 .. node count) valid
```

This fills the 64-byte cell exactly: 16 + 8 + 8 + 32 bytes. A fifth slot
does not fit unless the total count is dropped, which would make `getSize`
O(n) (decision D2).

A handle (`const ProtoLinkedList*`) is the word of the **head node**, with
the new pointer tag (D1). Because the total sits in the head node, every
suffix of a list is itself a list, and `rest` can return the next node.

### 3.2 The empty list

The empty list is **one perennial value per process**: a cell with count 0,
allocated outside the collector (a null context) the way symbols are, and
held by a function-local static. It holds no reference, so it needs no root.
`newLinkedList()` returns it, and so does `rest` of a one-element list.
`isEmpty()` is `getSize() == 0`.

### 3.3 Canonical form: aligned at the end

Every node is full (4 elements) **except the head**, which holds
`((n - 1) mod 4) + 1` elements. Every operation produces this form:

- **`prepend(v)`**: when the head has fewer than 4 elements, the head is
  copied with `v` in front (1 cell). When it is full, a new node with one
  element points at it (1 cell).
- **`rest`**:
  - head with 1 element: return `next` (0 cells);
  - head with k > 1 elements: copy it with k − 1 elements and the same
    `next` (1 cell).
- **The bulk builders** fill nodes from the end, so the partial node lands
  at the head.

Consequences:

- Two lists with the same elements have the same node boundaries. Suffixes
  are shared, and equality can compare node by node.
- `rest` allocates one cell in three steps out of four when the program
  walks a list with `first`/`rest`. This is the price of the density. The
  internal walks (cursor, `isEqual`, `getHash`, conversions) never allocate
  (section 5). See D3.

### 3.4 Elements

An element is any `ProtoObject` word, embedded values included. `PROTO_NONE`
is a valid element. `nullptr` is not: `prepend(nullptr)` returns the
receiver unchanged (D4).

## 4. Public API

```cpp
class ProtoLinkedList {
public:
    // Front access, O(1).
    const ProtoObject* getFirst(ProtoContext* context) const;        // nullptr when empty
    const ProtoLinkedList* getRest(ProtoContext* context) const;     // the empty list when size <= 1
    proto_ulong getSize(ProtoContext* context) const;
    bool isEmpty(ProtoContext* context) const;

    // Construction at the front: one cell.
    const ProtoLinkedList* prepend(ProtoContext* context, const ProtoObject* value) const;

    // O(k) walks, with no cells allocated except the result.
    const ProtoObject* getAt(ProtoContext* context, proto_ulong index) const;          // nullptr out of range
    const ProtoLinkedList* removeFirst(ProtoContext* context, proto_ulong count) const;  // drop
    const ProtoLinkedList* takeFirst(ProtoContext* context, proto_ulong count) const;    // ceil(count/4) cells

    // O(|this|) cells; `tail` is shared, not copied.
    const ProtoLinkedList* prependAll(ProtoContext* context, const ProtoLinkedList* tail) const;
    // O(n) cells.
    const ProtoLinkedList* reverse(ProtoContext* context) const;

    // Comparison: same size, same element words in order (D5).
    bool isEqual(ProtoContext* context, const ProtoLinkedList* other) const;
    proto_ulong getHash(ProtoContext* context) const;               // ProtoTuple's algorithm (D5)

    // Conversions.
    const ProtoList* asList(ProtoContext* context) const;            // a new ProtoList
    const ProtoTuple* asTuple(ProtoContext* context) const;          // the interned, perennial tuple
    const ProtoObject* asObject(ProtoContext* context) const;
};

// Iteration without allocation: a value cursor the caller keeps in a local (D6).
class ProtoLinkedListCursor {
public:
    explicit ProtoLinkedListCursor(const ProtoLinkedList* list);
    bool hasNext() const;
    const ProtoObject* next();
private:
    const void* node;
    proto_ulong index;
};
```

Additions to existing classes (non-virtual members only):

- `ProtoContext`:
  - `newLinkedList()`: the empty list;
  - `newLinkedListFromArray(count, values)`: for returning several values,
    without a `ProtoList` detour;
  - `newLinkedListFromList(const ProtoList*)` and
    `newLinkedListFromTuple(const ProtoTuple*)`.
- `ProtoObject`: `isLinkedList(context)` and `asLinkedList(context)`. They
  follow `isMap`/`asMap`, including the `__data__` delegation of a wrapper
  object.
- `ProtoSpace`: `getLinkedListPrototype()` and
  `setLinkedListPrototype(ProtoObject*)` (D7).

## 5. Cost

Cells allocated (n = elements, k = argument):

| Operation | Cells | Time |
|---|---|---|
| `prepend` | 1 | O(1) |
| `getFirst`, `getSize`, `isEmpty` | 0 | O(1) |
| `getRest` | 0 when the head has 1 element, otherwise 1 | O(1) |
| `getAt(k)`, `removeFirst(k)` | 0 or 1 (partial head) | O(k/4) |
| `takeFirst(k)` | ceil(k/4) | O(k/4) |
| `prependAll(tail)` | ceil(n/4) | O(n) |
| `reverse` | ceil(n/4) | O(n) |
| `newLinkedListFromArray` | ceil(n/4) | O(n) |
| `newLinkedListFromList/Tuple` | ceil(n/4) | O(n log n): one `getAt` per element |
| `asList`, `asTuple` | those of the target | O(n) |
| cursor walk, `isEqual`, `getHash` | 0 | O(n) |

Retained memory, compared with the current structures:

| n | `ProtoLinkedList` | `ProtoTuple` (today's builder) | `ProtoList` | protoClojure M44 list |
|---|---|---|---|---|
| 1-4 | 1 | 1 | 1 | 4-16 |
| 5 | 2 | 4 | 1 (Small) | 20 |
| 8 | 2 | 5 | ~8 | 32 |
| 17 | 5 | 14 | ~17 | 68 |
| n | ceil(n/4) | ~0.8 n | ~n | 4 n |

These counts come from reading the code; section 8 verifies them with tests.

## 6. Collector and threads

- **Immutable after construction.** No locks, no atomics. Publication follows
  the rules of every other immutable cell.
- **`processReferences`** reports `next` and the node's valid slots that
  carry a cell pointer (`ProtoObject::asCellPointer`).
- **Long chains are safe for marking.** The marker pushes each reported
  reference onto an explicit stack and handles overflow (`Marker::shade`,
  core/ProtoSpace.cpp), so a chain of a million nodes is not marked
  recursively.
- **Every operation walks the chain in a loop, never by recursion.** This
  covers `isEqual`, `getHash`, the conversions and the cursor. There is no
  finaliser.
- **The cursor holds a raw node pointer.** It stays valid while the list it
  started from is reachable, which is the same rule as for any C++ local
  holding a `ProtoObject*` (docs/EMBEDDER-CONFORMANCE.md).

## 7. Tags and cell types

- **One new pointer tag**, the next free value, 29, for the handle (D1). This
  needs the maintainer's explicit approval under the tag-budget rule in
  proto_internal.h; this spec is that request. After it, 34 tags remain
  free.
- **Nodes and the empty list** are `CellType::LinkedList`, appended at the
  end of the enum.
- **The cursor** is a C++ value. It is not a cell and takes no tag.

## 8. Tests (GoogleTest, new `test/LinkedListTests.cpp`)

1. Canonical form: after any mix of `prepend`, `getRest` and
   `removeFirst`, the head holds `((n-1) mod 4)+1` elements and every other
   node holds 4.
2. Cells per operation, measured with the context's allocation counter
   against the table in section 5.
3. Structural sharing: `getRest` of a one-element head returns `next` itself,
   and `prependAll` shares its tail.
4. The empty list: one per process, and `getRest` of a one-element list
   returns it.
5. Conversions round trip: list → `ProtoList` → list, and list → tuple →
   list. `asTuple` of two equal lists returns the same tuple.
6. `getHash` equals `ProtoTuple::getHash` for the same elements, and
   `isEqual` follows element words.
7. Collection:
   - a loop of 10^6 `prepend` calls that drops each list keeps the heap
     bounded (the run uses `PROTOCORE_HEAP_LIMIT_CELLS`);
   - a retained list survives several cycles intact.
8. A chain of 10^6 elements is marked, compared, hashed and converted
   without stack overflow (ASan build).
9. Concurrency: lists shared between threads and read concurrently, run
   under TSan.
10. `ProtoObject::isLinkedList` and `asLinkedList`, directly and through a
    `__data__` wrapper; `getPrototype` returns the configured prototype.
11. ABI guard: `static_assert` that `sizeof(ProtoSpace)` and the offsets of
    its public fields are unchanged from 3.2.0.

## 9. Compatibility and versioning

**Version 3.3.0, SOVERSION 4.** Everything is additive:

- new classes;
- new non-virtual member functions on `ProtoContext`, `ProtoObject` and
  `ProtoSpace`;
- a new tag value;
- a `CellType` value appended at the end.

**No public layout changes.** In particular no field is added to
`ProtoSpace`, whose public prototype fields are read by embedders compiled
against the old layout, and which embedders construct themselves with the
`sizeof` of their own header (D7).

**A runtime that uses the type requires the version.**

- At configure time: `find_package(protoCore 3.3 REQUIRED)`. The package
  version file uses `SameMajorVersion`, so any 3.x from 3.3 on is accepted.
- At load time: a binary that calls the new symbols does not load against
  3.2. The dynamic loader reports the undefined symbol at start-up, never in
  the middle of a run.
- A Debian package that uses the type declares
  `Depends: protocore (>= 3.3.0)`, because the DEB dependency is not an ABI
  check.

**A runtime that does not use the type is unaffected.**

- No allocation path, no collector phase, no existing type and no existing
  public layout changes.
- The only shared code that changes is a new `case` in
  `ProtoObject::getPrototype` and in the `CellType` name tables. The
  collector's root scan is untouched: the prototype is pinned in a root set
  of the space, which the collector already scans (section 12).
- A 3.2 binary keeps working against 3.3 without a rebuild.

This is verified, not assumed:

- the full `ctest` suite;
- the ASan and TSan jobs;
- the embedder conformance suite;
- the protoClojure A/B harness (`run_ab.sh`, 7 rounds) against master
  3.2.0, where the numeric workloads must show identical instruction counts.

## 10. Rollout

1. protoCore 3.3.0: implementation, the tests of section 8, CHANGELOG,
   DESIGN.md (the type, and the interning summary pointing at `asTuple`), a
   tutorial `docs/tutorials/linked-list/` and an example.
2. protoClojure first: lists over `ProtoLinkedList` in place of the M44
   chain. Its A/B against the current main measures:
   - the list walk with `rest` (the allocation of section 3.3);
   - the retained memory;
   - `(vec list)`, which a bulk `asList` makes O(n).
3. Other runtimes on demand:
   - returning several values (protoScala, protoST);
   - the protoJS `ProtoCoreModule` tuple binding, which builds a perennial
     `ProtoTuple` for every call.
4. Not protoPython: its tuples need random access and are correctly served
   by `ProtoList`.

## 11. Decisions for the maintainer

| Id | Question | Recommendation |
|---|---|---|
| D1 | Approve a new pointer tag (29) for the handle | Yes. A list is a first-class value and needs its own prototype, which the tag gives |
| D2 | Four slots plus the total count, or five slots without it | Four. `getSize` O(1) is required by Clojure `count`, and a total is needed for every conversion |
| D3 | Accept that `getRest` allocates one cell in three steps out of four | Yes, and measure it in protoClojure (rollout 2). The alternative, one element per node when consing, loses the density and the canonical form |
| D4 | `nullptr` element: ignore (`prepend` returns the receiver) or treat as a programming error | Ignore, as `ProtoMap` does with a null key (its D3) |
| D5 | `isEqual` by element words, `getHash` with `ProtoTuple`'s algorithm, so a list and its `asTuple` hash alike | Yes. Language equality (Clojure `=`, Python `==`) stays in the runtime |
| D6 | Iteration through a value cursor (no cell) or a cell iterator like `ProtoMapIterator` | The cursor. Iterating the type must not allocate, and the cursor's two-word layout becomes part of the ABI |
| D7 | The prototype through `get/setLinkedListPrototype()` stored in `ProtoSpaceImplementation`, or a new public field like `mapPrototype` | The accessors. A new field changes `sizeof(ProtoSpace)` and breaks binaries built against 3.2 without a SOVERSION bump. `ProtoMap` accepted that risk (its D5); this type promises no regression |
| D8 | Name: `ProtoLinkedList` | Keep it. It says what the type is and does not collide with `ProtoList` |

All eight recommendations were approved by the maintainer on 2026-10-25.

## 12. Implementation notes (3.3.0)

- **Nothing is added to the collector's root scan.** A first version added
  the prototype and a per-space empty list to the global roots of the
  stop-the-world scan in `gcThreadLoop`. Those five lines, executed once per
  cycle, slowed protoClojure's `reduce-list` by 11-13 % in cycles with
  identical instruction counts, reproduced across three 7-round runs and
  isolated by bisection (headers alone: byte-identical library; the
  `getPrototype` case alone: no change; the root-scan lines alone: the whole
  slowdown). Hence:
  - the empty list is one perennial cell per process (section 3.2);
  - the prototype is pinned in a `ProtoRootSet` the space creates
    (`protoCore-space-extension`); the collector scans root sets already,
    and the space destroys them with the others.
- **`ProtoSpaceImplementation`** was a declared, never-defined class behind
  the unused `ProtoSpace::impl` field. It is now defined in proto_internal.h
  and holds the state added after the 3.2.0 layout: the root set and the
  prototype. The space creates it in its constructor and deletes it in its
  destructor.
- **Builders** (`newLinkedListFromArray`, `takeFirst`, `prependAll`,
  `reverse`) build from the end and anchor the chain under construction in
  `ProtoContext::pendingRoot`, parking for stop-the-world after each cell,
  as `ProtoContext::newList(n, items)` does. No critical section spans the
  build. (Superseded after 3.3.1: `pendingRoot` was removed; the chain under
  construction is rooted by the building context's young generation, which
  the builder never submits. See CHANGELOG, Unreleased.)
- **`isEqual`** stops at the first shared cell: equal sizes give equal cell
  boundaries, so a shared suffix is detected by pointer.
- **Verified for no regression** (section 9): the layout of `ProtoSpace`
  measured identical to master `bb2d581a` (19,584 bytes, same offsets) and
  pinned by `LinkedList.ProtoSpaceLayoutIsUnchangedFrom320`.
- **Performance of a program that does not use the type** (protoClojure
  main `58caf71`, one `protoclj` built against 3.2.0 and run against the
  3.2.0 and 3.3.0 libraries, 7 rounds, fresh copies, quiet gate, perf stat):
  - instruction counts are identical on every workload (within 0.1 %);
  - cycles: `fib` +1 to +4 %, `tak` +2 to +3 %, `sum-squares` +3 to +4 %,
    the others within noise, over two runs;
  - noise floor, master against a byte-identical copy of itself: within
    1 % (2.5 % for `map-loop` and `str-map`);
  - layout control, master plus 160 bytes of dead code at the top of
    ProtoSpace.cpp: `fib` +4.9 %, `tak` +3.6 %. A change with no semantics
    moves these workloads as much as this type does, so the residual
    cycle deltas are the library's sensitivity to code placement, not work
    the type adds. `sum-squares` +3 to +4 % is not reproduced by the
    control and stays unexplained.
- **The collection test discriminates.** With every list retained
  (a negative control, not committed), `LinkedList.DroppedListsAreCollected`
  stops with *heap hard limit reached ... out of memory*.
