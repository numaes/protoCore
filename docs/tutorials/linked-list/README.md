# Tutorials: ProtoLinkedList

These tutorials teach `ProtoLinkedList`, the immutable list built at the
front, by example. Every C++ fragment comes from a program in
[examples/linked-list/](../../../examples/linked-list/). Those programs compile
with protoCore's test suite and run as the tests `example.linked_list.*`, so
the tutorials cannot drift from the code without a test failing. The design is
in [docs/specs/2026-10-25-linked-list-design.md](../../specs/2026-10-25-linked-list-design.md).

| # | Tutorial | You will learn | Example |
|---|---|---|---|
| 01 | [Sequences built at the front](01_front_built_sequences.md) | Returning several values; a Lisp list with `prepend`, `getFirst`, `getRest`; walking without allocating; `asTuple` for identity | `01_front_built_sequences.cpp` |

## Building and running the examples

```bash
cmake -S . -B build_release -DCMAKE_BUILD_TYPE=Release
cmake --build build_release -j4
ctest --test-dir build_release -R example.linked_list --output-on-failure
```

Each example prints one line per check (`ok` or `FAIL`) and a final
`<name>: PASS`.
