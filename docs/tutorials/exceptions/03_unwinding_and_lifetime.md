# 03 — Unwinding, cleanup and lifetime

**Example:** [`03_unwinding_and_lifetime.cpp`](../../../examples/exceptions/03_unwinding_and_lifetime.cpp).

Between the raise and the handler, C++ unwinds the stack: destructors run,
`catch (...)` blocks for cleanup run, and contexts end. This tutorial shows
what that means for protoCore values.

## Step 1. `finally` is a catch-all that rethrows

A language's `finally` (or Smalltalk's `ensure:`, or Clojure's `finally`) is,
in C++:

```cpp
template <typename Body, typename Cleanup>
void tryFinally(Body body, Cleanup cleanup) {
    try {
        body();
    } catch (...) {
        cleanup();
        throw;   // the same exception object, not a copy
    }
    cleanup();
}
```

`catch (...)` is right here, and only here. The cleanup runs for every
exception, whether or not it is a language error, and `throw;` resumes the
same unwinding.

A C++ destructor (RAII) is the other way to run cleanup. Runtimes use both.

## Step 2. The value survives the collector during unwinding

Cleanup code can allocate, and an allocation can start a collection. The
example's cleanup allocates 5,000 objects and waits for two complete
collection cycles before the handler runs:

```cpp
tryFinally(
    [&] { (void) callModule(ctx, geometry, "circleArea", {ctx->fromInteger(-3)}); },
    [&] {
        for (int i = 0; i < 5000; ++i) {
            ProtoContext scratch(&space, ctx);
            (void) scratch.newObject(false)->setAttribute(&scratch, sym(&scratch, "i"),
                                                         scratch.fromInteger(i));
        }
        cleaned = collect(space, ctx, 2);
    });
```

The handler then reads the error's message intact. The error was built in
the module method's context, and that context had already ended when the
cleanup ran. What kept the value alive is the exception itself: a
`ProtoException` pins its value in the space's root set
`protocore-exceptions-in-flight` until the last copy is destroyed.

**A runtime does not pin thrown values itself.** Before protoCore 3.0.0,
protoJS, protoPython and protoClojure each kept a root set for errors in
flight, and protoScala kept none, relying on nothing allocating between
frames.

## Step 3. Keep an exception, rethrow it later

A future, a promise or an actor's reply carries an error from where it
happened to where it is awaited. In C++ that is `std::exception_ptr`:

```cpp
std::exception_ptr kept;
{
    ProtoContext call(&space, ctx);
    try {
        (void) callModule(&call, geometry, "circleArea", {call.fromInteger(-4)});
    } catch (const ProtoException&) {
        kept = std::current_exception();
    }
}   // the context that built the value is gone

collect(space, ctx, 2);

try {
    std::rethrow_exception(kept);
} catch (const ProtoException& e) {
    // the NegativeRadius, message intact
}
```

The kept exception is a copy that shares the pin. The example checks that
exactly one value is pinned while `kept` holds the exception, and that none
is pinned after `kept = nullptr`:

```cpp
check(space.exceptionRoots_->size() == 1, "one value is pinned while the exception is kept");
// ...
kept = nullptr;
check(space.exceptionRoots_->size() == 0, "no value is pinned once the exception is gone");
```

## Step 4. The one lifetime rule

**An exception must not outlive its space.** Destroying a `ProtoSpace`
releases its root sets, so a `ProtoException` destroyed after its space
would release a pin in freed memory. A runtime that stores errors (futures,
promises, mailboxes) destroys them before the space, which it already does
for every `ProtoObject*` it holds.

## Step 5. Threads

A `ProtoException` that escapes a `ProtoThread`'s method is stopped at
protoCore's thread entry, which prints one line under `PROTO_THREAD_DIAG`.
The thread still leaves the space in order. That is a last line of defence:
a runtime catches its errors in the thread's method and reports them, or
hands them to whoever joins (as `std::exception_ptr`, step 3).

Next: [04 — Foreign C++ exceptions](04_foreign_cpp_exceptions.md).
