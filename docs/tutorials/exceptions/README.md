# Tutorials: exceptions on protoCore

These tutorials teach [protoCore's exception model](../../EXCEPTIONS.md) by
example. Every C++ fragment comes from a program in
[examples/exceptions/](../../../examples/exceptions/). Those programs compile
with protoCore's test suite and run as the tests `example.exceptions.*`, so the
tutorials cannot drift from the code without a test failing.

The model in one sentence: **every language throws `proto::ProtoException`
carrying a value, and a handler selects the error by the value's prototype
chain.**

| # | Tutorial | You will learn | Example |
|---|---|---|---|
| 01 | [Raising from a module, catching by prototype](01_raising_from_a_module.md) | Define an error hierarchy in a UMD module, raise it, catch it from C++ by base type, rethrow what is not yours | `01_catch_by_prototype.cpp`, `geometry_module/` |
| 02 | [protoCore's errors and the error factory](02_error_factory.md) | What protoCore raises, and how a runtime makes it arrive as the language's own exception classes | `02_error_factory.cpp` |
| 03 | [Unwinding, cleanup and lifetime](03_unwinding_and_lifetime.md) | `finally` in C++, keeping an exception for later, and why the value survives the collector | `03_unwinding_and_lifetime.cpp` |
| 04 | [Foreign C++ exceptions](04_foreign_cpp_exceptions.md) | Translate `std::exception` at the native boundary, let `ProtoException` through, never `std::terminate` | `04_foreign_cpp_exceptions.cpp` |
| 05 | [Writing a runtime's handlers](05_writing_a_runtime_handler.md) | For runtime authors: `try`/`catch`/`finally` in an interpreter, the catch-all form, crossing out, conformance rule 14 | `conformance/SelfHost.h` |
| 06 | [The same module from every language](06_languages.md) | JavaScript, Python, Smalltalk, Scala and Clojure catching the geometry module's errors; each runtime's factory table | the runtimes' own tests |

## Building and running the examples

```bash
cmake -S . -B build_release -DCMAKE_BUILD_TYPE=Release
cmake --build build_release -j4
ctest --test-dir build_release -R example.exceptions --output-on-failure
# or one by one, to read what each checks:
./build_release/examples/exceptions/protocore_example_01_catch_by_prototype
```

Each example prints one line per check (`ok` or `FAIL`) and a final
`<name>: PASS`.

## The example module

All tutorials use one module,
[`geometry_module/GeometryModule.cpp`](../../../examples/exceptions/geometry_module/GeometryModule.cpp).
It is a UMD provider with alias `example_geometry` and logical path
`example/geometry`, built as its own shared library
(`libprotocore_example_geometry`):

```
GeometryError                 the module's base error type
 +-- NegativeRadius           circleArea(r) with r < 0
 +-- DegenerateTriangle       triangleArea(a, b, c) when the sides make no triangle

circleArea(r), triangleArea(a, b, c)    raise the module's errors
parseRadius(text)                       lets protoCore's InvalidArgument through
legacyArea(r)                           throws std::runtime_error (the anti-pattern)
```
