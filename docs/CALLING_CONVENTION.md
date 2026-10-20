# Calling convention (protoCore 3.0.0)

protoCore has **one method type and two ways to call it**. A method does not
know, and does not need to know, how it was called: it reads its arguments
through descriptors, and the caller decides whether those descriptors read a
`ProtoList` and a `ProtoSparseList` it already holds, or arrays it owns.

This document is the reference for writing methods, calling them, and
migrating code written for protoCore 2.x.

## 1. The method type

```cpp
typedef const ProtoObject* (*ProtoMethod)(
    ProtoContext* context,
    const ProtoObject* self,
    const ParentLink* parentLink,
    const ProtoArguments* positionalParameters,
    const ProtoKeywordArguments* keywordParameters);
```

A method binds its parameters by reading the descriptors:

```cpp
const ProtoObject* add(ProtoContext* c, const ProtoObject*, const ParentLink*,
                       const ProtoArguments* args, const ProtoKeywordArguments* kw) {
    const ProtoObject* a = args->getAt(c, 0);
    const ProtoObject* b = args->getAt(c, 1);
    const ProtoObject* scale = kw ? kw->getAt(c, reinterpret_cast<proto_ulong>(scaleName))
                                  : PROTO_NONE;
    ...
}
```

It is registered exactly as before: `context->fromMethod(self, &add)`.

`positionalParameters` is never null when protoCore makes the call (every form
of `ProtoObject::call`, a thread's main method); it describes zero values when
there are none. `keywordParameters` is null when the call has no keywords. A
method invoked directly by an embedder receives whatever that embedder passes,
so a defensive method still checks both.

## 2. The descriptors

### `ProtoArguments` — positional arguments

A descriptor over an array of values: `{pointer, count}`, plus the list the
arguments came from, if any. It reads like a `ProtoList`:

| Member | Meaning |
|---|---|
| `getSize(ctx)`, `count()` | the number of values |
| `getAt(ctx, i)` | value `i`; `PROTO_NONE` outside `0 .. size-1` and for a null slot, as `ProtoList::getAt` |
| `getFirst(ctx)`, `getLast(ctx)` | as in `ProtoList` |
| `has(ctx, value)` | as `ProtoList::has` (identity, integers by value) |
| `values()` | the array itself, for a binding loop: `count()` values, no bounds check |
| `asList(ctx)` | the arguments as a `ProtoList`: the call's own list if it was made with one, a list built in `ctx` otherwise |

`getAt` is inline and never branches on where the values came from: it is
`values[i]` behind a bounds check. A runtime's binding loop can read
`values()` directly.

### `ProtoKeywordArguments` — keyword arguments

A descriptor over an array of `ProtoKeywordArgument` pairs:

```cpp
struct ProtoKeywordArgument {
    const ProtoString* name;    // an interned symbol (ProtoString::createSymbol)
    const ProtoObject* value;
};
```

A key is the interned name's pointer, the same key `ProtoObject::setAttribute`
uses for attributes. It reads like a `ProtoSparseList`: `getSize`,
`has(ctx, key)`, `getAt(ctx, key)` (`PROTO_NONE` when absent),
`processElements` (in array order), `pairs()` / `count()`, and
`asSparseList(ctx)` (the call's own sparse list, or one built in `ctx`).

## 3. The call forms

The same lookup (`getAttribute` on the receiver's chain), the same method, and
the space's `nonMethodCallback` when the name resolves to something that is not
a method.

### Array form — no allocation

```cpp
const ProtoObject* call(ProtoContext* context, const ParentLink* nextParent,
                        const ProtoString* method, const ProtoObject* self,
                        const ProtoObject* const* positional, unsigned positionalCount,
                        const ProtoKeywordArgument* keywords = nullptr,
                        unsigned keywordCount = 0) const;
```

The method receives descriptors over the caller's arrays; nothing is copied
and nothing is allocated. **The caller decides where the arrays live and how
large they are.** The usual choices:

- a runtime's operand stack or frame slots, which already hold the arguments
  and which the collector already traces: pass a pointer into them;
- a C++ local array (`const ProtoObject* args[N]`) for values that are
  reachable from somewhere else for the duration of the call.

### List form — the 2.x call

```cpp
const ProtoObject* call(ProtoContext* context, const ParentLink* nextParent,
                        const ProtoString* method, const ProtoObject* self,
                        const ProtoList* positionalParameters,
                        const ProtoSparseList* keywordParametersDict = nullptr) const;
```

The method receives descriptors over the lists. A small list (up to 5 values,
`ProtoListSmallImplementation`) is read in place; a larger one, and the
keywords, are copied into local arrays of the call's own frame (16 values and
8 pairs). Only a call with more arguments than that goes through the
heap-backed bridges below. Keyword keys must be interned names' pointers, the
convention every runtime already uses.

### Descriptor form — passing arguments on

```cpp
const ProtoObject* call(ProtoContext* context, const ParentLink* nextParent,
                        const ProtoString* method, const ProtoObject* self,
                        const ProtoArguments& positional,
                        const ProtoKeywordArguments* keywords = nullptr) const;
```

For a method that forwards the descriptors it received (a dispatcher, a
`super` send): nothing is rebuilt.

### Invoking a method pointer directly

Code that holds a method value and calls its function pointer, as runtimes do
for their own natives, builds the descriptors itself:

```cpp
const ProtoObject* argv[2] = {a, b};               // or a pointer into the operand stack
const ProtoArguments args(argv, 2);
const ProtoObject* r = callee->asMethod(ctx)(ctx, self, nullptr, &args, nullptr);
```

When the arguments are in a list, `ProtoListArguments` and
`ProtoSparseListKeywordArguments` are the bridges: they read a small list in
place, copy a larger one into an inline buffer (8 values, 4 pairs) and use the
heap only beyond it.

```cpp
const ProtoListArguments args(ctx, list);
const ProtoObject* r = callee->asMethod(ctx)(ctx, self, nullptr, &args, nullptr);
```

## 4. The rules

1. **A descriptor lives only for the call.** A method that keeps its arguments
   keeps `asList(ctx)` / `asSparseList(ctx)`, never the descriptor and never
   the pointer from `values()`.
2. **The caller keeps the values reachable** for the duration of the call.
   protoCore does not trace the arrays. Values in the caller context's
   automatic locals, or in a runtime's traced operand stack, are reachable; so
   are values held by a list the caller holds (the list form). A value that
   exists only in a C++ local array is not, unless it is embedded (a
   SmallInteger, a boolean, `PROTO_NONE`) or held elsewhere.
3. **Keyword names are interned symbols.** Callees compare them by pointer.
4. **The method definition does not depend on the call form.** Do not test
   whether the call came with lists; read the descriptors.

## 5. Parameter binding in `ProtoContext`

The `ProtoContext` constructor takes the descriptors:

```cpp
ProtoContext(ProtoSpace* space, ProtoContext* previous,
             const ProtoList* parameterNames, const ProtoList* localNames,
             const ProtoArguments* args, const ProtoKeywordArguments* kwargs,
             size_t totalSlots, const ProtoObject** externalSlots);
```

A method that wants protoCore to bind its parameters passes the descriptors it
received. The binding reads them: positional values by index, keywords matched
to a parameter by the name's pointer or by its content hash (the key the
binding used before 3.0.0).

## 6. Migrating from 2.x

1. **Signatures.** In every method, replace `const ProtoList*` by
   `const ProtoArguments*` and `const ProtoSparseList*` by
   `const ProtoKeywordArguments*`. Code that only reads (`getAt`, `getSize`,
   `has`, `getAt(key)`, `processElements`) compiles unchanged: measured across
   the five runtimes before the change, these are about 97 % of all uses.
2. **Retaining or deriving.** Code that stores the argument list, iterates it
   with an iterator, slices it, appends to it or passes it to a function that
   takes a `ProtoList*` calls `asList(ctx)` first (`asSparseList(ctx)` for
   keywords).
3. **Direct invocations** (`asMethod(ctx)(...)`) build descriptors, from
   arrays or with the bridges (section 3).
4. **`nonMethodCallback`** receives descriptors.
5. **`ProtoContext` construction** with arguments passes descriptors.
6. **The hot calls** — a native calling back into user code (`map`, `reduce`,
   `sort` comparators), a call with keywords — move to the array form,
   pointing at the operand stack. That is where the gain is.

## 7. Cost

The C++ floor (`identity` method, one argument, 3.54 GHz, best of 5 runs of
4,000,000 calls, default heap), 2026-10-20:

| Call | 2.14.8 | 3.0.0 list form | 3.0.0 array form |
|---|---:|---:|---:|
| by name, arguments reused | 22.8 ns | 22–25 ns | 18.5–19.6 ns |
| + the callee's context | 53.0 ns | 53–57 ns | 47–51 ns |
| fresh arguments every call, with caller and callee contexts | 209 ns (a new 1-element list) | 217–221 ns | 63–66 ns (C++ local array or caller slots) |

The array form removes the list a caller builds for every call: about 150 ns of
a 210 ns call. What remains is the two contexts.
