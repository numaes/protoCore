# Architecture Overview: The Object Model and ProtoContext

This overview summarizes how protoCore represents values, how objects inherit from each other, and how the per-thread attribute cache speeds up lookups. [DESIGN.md](../../../DESIGN.md) is the detailed reference.

## ProtoContext

Almost every protoCore API call takes a `ProtoContext*` as its first argument. A context represents one method call on one thread:

- **Chaining.** The constructor `ProtoContext(ProtoSpace* space, ProtoContext* previous = nullptr, ...)` creates the context for a call, and `previous` links it to the caller's context, so each thread has a chain of contexts. `ProtoContext` has no default constructor. On the main thread, `ProtoSpace::rootContext` is the root of the chain; threads managed by protoCore have their own context chains.
- **Locals.** A context holds the call's local variables (`automaticLocals`), the variables captured by closures (`closureLocals`) and the return value (`returnValue`). The garbage collector scans them as roots.
- **Allocation.** Cells allocated through a context come from the thread's pool of free cells and are recorded in the context's young-generation chain. When the context is destroyed, or at the next safepoint after its allocation count crosses `ProtoSpace::maxAllocatedCellsPerContext`, the chain is handed to the collector as a `DirtySegment`. Until then, the context protects those cells.
- **Null context.** Allocating with a null `ProtoContext*` creates perpetual cells that the collector never reclaims; see DESIGN.md, "Keeping ProtoObjects alive across allocation boundaries the GC cannot see".

## Tagged Pointers

A `ProtoObject*` is a 64-bit word. Heap cells are 64-byte aligned, so the low 6 bits of a cell address are always zero; protoCore stores a 6-bit `pointer_tag` in them (`ProtoObjectPointer` in `headers/proto_internal.h`).

| `pointer_tag` | Meaning |
|---|---|
| 0 (`POINTER_TAG_OBJECT`) | An object cell (`ProtoObjectCell`) on the heap |
| 1 (`POINTER_TAG_EMBEDDED_VALUE`) | An immediate value; a 4-bit `embedded_type` follows the tag |
| 22 (`POINTER_TAG_SYMBOL`) | An interned string (symbol), compared by pointer identity |
| 2–21, 23–26 | Other heap cell types: lists, tuples, strings, sparse lists, sets, multisets, buffers, methods, threads, `LargeInteger`, `Double`, iterators, and internal string and small-collection forms |

Embedded types for tag 1:

| `embedded_type` | Value |
|---|---|
| 0 | SmallInt: a signed 54-bit integer in bits 10–63 |
| 2 | Unicode character |
| 3 | Boolean |
| 4 | Inline string: up to 6 UTF-8 bytes (`INLINE_STRING_MAX_BYTES`) |
| 5 | None (`PROTO_NONE`) |

Immediate values need no heap allocation and are never seen by the collector. Values outside these ranges are promoted to heap objects (`LargeInteger`, `Double`, heap strings). Embedders can use the inline helpers `proto::isSmallInt`, `proto::asSmallInt`, `proto::smallIntInRange` and `proto::makeSmallInt` from `protoCore.h` to work with SmallInts without calling into the library.

## Prototype-Based Inheritance

protoCore uses prototype delegation instead of classes. An object's parents are held in a chain of `ParentLink` cells, and an object can have several parents. `newChild` creates an object whose parent is the receiver, `addParent` adds a parent, and `setParents` replaces the parent list.

`getAttribute` looks in the object's own attributes first and then walks the parent chain until it finds the attribute or the chain ends. It returns `PROTO_NONE` when the attribute is missing; because an attribute can also hold `PROTO_NONE`, use `hasAttribute` or `hasOwnAttribute` to test for presence. `getOwnAttributeDirect` reads only the object's own attributes.

`ProtoObject::call` looks up a method by name with `getAttribute` and, if the value is a method, calls its native function with the receiver and the arguments. Native methods are C++ functions of type `ProtoMethod`; `ProtoContext::fromMethod` wraps one, together with its receiver, in a method object.

## The Per-Thread Attribute Cache

Each thread keeps a 1024-entry attribute cache (`THREAD_CACHE_DEPTH`) in its `ProtoThreadExtension`. Each 32-byte entry records an object state, an attribute name and a result. The slot index is computed from the two pointers:

```
((object >> 6) ^ (name >> 4)) % THREAD_CACHE_DEPTH
```

In `getAttribute`, an entry records an own-attribute fact: the value an object state owns for a name, or that it does not own that name. A hit avoids searching that object's attribute tree during the lookup. For a mutable object the key is its current state, so an update by any thread produces a different key; `setAttribute` also clears the matching entry on the writing thread. The cache belongs to a single thread, so reading it needs no locks.

A second per-thread table, the mutable value cache, maps a mutable object's `mutable_ref` to its current state; see [the mutability model](02_mutability_model.md).
