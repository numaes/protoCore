# Tutorials: strings on protoCore

These tutorials teach protoCore's strings by example. Every C++ fragment comes
from a program in [examples/strings/](../../../examples/strings/). Those
programs compile with protoCore's test suite and run as the tests
`example.strings.*`, so the tutorials cannot drift from the code without a test
failing. The design is in [DESIGN.md](../../../DESIGN.md), § 2.

| # | Tutorial | You will learn | Example |
|---|---|---|---|
| 01 | [Attribute names: intern once, compare by pointer](01_attribute_names.md) | The canonical form of a string; interning source names at module initialization and computed names with `ProtoString::intern` | `01_attribute_names.cpp` |

## Building and running the examples

```bash
cmake -S . -B build_release -DCMAKE_BUILD_TYPE=Release
cmake --build build_release -j4
ctest --test-dir build_release -R example.strings --output-on-failure
```

Each example prints one line per check (`ok` or `FAIL`) and a final
`<name>: PASS`.
