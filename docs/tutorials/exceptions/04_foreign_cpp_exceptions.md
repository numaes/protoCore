# 04 — Foreign C++ exceptions

**Example:** [`04_foreign_cpp_exceptions.cpp`](../../../examples/exceptions/04_foreign_cpp_exceptions.cpp).

A C++ library used by a module may throw `std::runtime_error`,
`std::bad_alloc`, or anything at all. Those are not language errors: they
carry no value and no prototype. This tutorial is the boundary rule
([EXCEPTIONS.md](../../EXCEPTIONS.md), E6).

## Step 1. Untranslated, the error is invisible to the language

The geometry module's `legacyArea` throws `std::runtime_error`, the
anti-pattern:

```cpp
if (r < 0) throw std::runtime_error("legacyArea: negative radius");
```

A language handler catches `ProtoException`, so this goes past it:

```cpp
try {
    try {
        (void) callModule(ctx, geometry, "legacyArea", {ctx->fromInteger(-1)});
    } catch (const ProtoException&) {
        sawProto = true;          // never runs
    }
} catch (const std::exception&) {
    sawStd = true;                // the error arrives here
}
```

In a runtime this means a `try`/`catch` in the language cannot handle it,
and no `finally` written as `catch (const ProtoException&)` runs. Write
cleanups as `catch (...)` (tutorial 03).

## Step 2. Translate at the native-call boundary

Every runtime has one place where its interpreter calls native code (a
primitive, a builtin, a module method). Translate there, once:

```cpp
const ProtoObject* callNative(ProtoContext* ctx, const ProtoObject* module, const char* method,
                              std::initializer_list<const ProtoObject*> args) {
    try {
        return callModule(ctx, module, method, args);
    } catch (const ProtoException&) {
        throw;   // a language value passes unchanged
    } catch (const std::exception& e) {
        const ProtoObject* error = g_RuntimeError->newChild(ctx)->setAttribute(
            ctx, sym(ctx, "message"), ctx->fromUTF8String(e.what()));
        ctx->raise(error);
    }
}
```

- **A `std::exception` becomes the language's generic runtime error:**
  JavaScript `Error`, Python `RuntimeError`, Scala `RuntimeException`,
  Clojure `RuntimeException`, Smalltalk `Error`. Its message is `what()`.
- **A `ProtoException` is caught first and rethrown untouched.** It does not
  derive from `std::exception`, so the second clause would not catch it
  anyway; the explicit clause documents the intent.
- **A runtime may map specific standard exceptions.** For example,
  `std::bad_alloc` to `OutOfMemoryError` or `std::out_of_range` to
  `IndexOutOfBoundsException`, as protoScala does. It must keep the
  `std::exception` fallback.

## Step 3. A module's own errors are not translated

```cpp
try {
    (void) callNative(ctx, geometry, "circleArea", {ctx->fromInteger(-1)});
} catch (const ProtoException& e) {
    e.isInstanceOf(ctx, GeometryError);    // true
    e.isInstanceOf(ctx, g_RuntimeError);   // false
}
```

## Step 4. The top level never terminates

What is neither a `ProtoException` nor a `std::exception` (a thrown `int`, a
foreign runtime's private type) reaches the top level. There, and at every
thread, actor and event-loop entry, a runtime reports it:

```cpp
try {
    run(program);
} catch (const ProtoException& e) {
    reportUncaught(e.value());        // the language's own "Uncaught ..." output
    return 1;
} catch (const std::exception& e) {
    std::fprintf(stderr, "internal error: %s\n", e.what());
    return 70;
} catch (...) {
    std::fprintf(stderr, "uncaught foreign exception\n");
    return 70;
}
```

Without the last clause, C++ calls `std::terminate`. That aborts the
process, often without unwinding the stack: no destructor runs and nothing
is flushed.

## Writing modules: the rule

A module raises `ProtoException` (tutorial 01), or `ctx->raiseError` for one
of protoCore's kinds (tutorial 02). When it wraps a C++ library, it catches
that library's exceptions in the module and raises its own error types.
There, unlike in a runtime's generic boundary, it knows what the failure
means.

Next: [05 — Writing a runtime's handlers](05_writing_a_runtime_handler.md).
