# 05 — Writing a runtime's handlers

**For runtime authors.** The reference implementation is
[`conformance/SelfHost.h`](../../../conformance/SelfHost.h)
(`catchForeignError`, `escapeLanguageError`). The checks are conformance rule
14 in [EMBEDDER-CONFORMANCE.md](../../EMBEDDER-CONFORMANCE.md).

This tutorial turns [EXCEPTIONS.md §4](../../EXCEPTIONS.md#4-obligations-of-a-runtime)
into code for a bytecode interpreter. It covers a language's `throw`,
`try`/`catch`, `finally`, rethrow, and the errors leaving the runtime.

## Step 1. `throw` is `raise`

```cpp
case OP_THROW: {
    const ProtoObject* value = pop();
    ctx->raise(value);                  // throw proto::ProtoException(ctx, value)
}
```

The same applies to every error the VM raises itself. A type error, an
undefined name or a stack overflow is built as the language's exception
object and raised:

```cpp
[[noreturn]] void raiseTypeError(ProtoContext* ctx, const char* message) {
    ctx->raise(newInstance(ctx, lang.TypeError, message));
}
```

**A subclass for extras.** A runtime may throw its own subclass carrying a
source line or a traceback:

```cpp
struct LangException : proto::ProtoException {
    LangException(ProtoContext* c, const ProtoObject* v, int line)
        : proto::ProtoException(c, v), line(line) {}
    int line;
};
```

Handlers still catch the base. Another runtime sees only `value()`, so
anything a user can observe (the message, the traceback a language exposes)
belongs in the value's attributes too.

**A lazy representation is internal only.** An interpreter may keep a VM
error as a class name and a message, and build the object only when a
handler needs it. Before the error crosses any boundary (a call into native
code or another runtime, a return to the embedder, a thread, future or actor
entry) it must become a `ProtoException` with the object.

## Step 2. `try`/`catch`: one C++ `try` per language `try`

The structure protoJS and protoPython use: each language `try` runs its
protected range as a nested call of the dispatch loop, inside a C++ `try`.

```cpp
// The protected range [begin, end) of a try whose handler starts at handlerPc.
static NOINLINE bool runProtected(Frame& f, size_t begin, size_t end, size_t handlerPc) {
    const size_t depth = f.stackDepth();
    try {
        dispatch(f, begin, end);            // returns when the range completes
        return false;                       // no exception: continue after the range
    } catch (const proto::ProtoException& e) {
        f.truncateStack(depth);             // drop what the range pushed
        f.push(e.value());                  // the catch clause's binding
        f.pc = handlerPc;
        return true;
    }
}
```

Points that are easy to get wrong:

- **Catch `proto::ProtoException`, not only your subclass.** An error raised
  by a module or by another language is a plain `ProtoException`.
- **Restore interpreter state in the handler.** That means the operand stack
  depth, the current-frame marker and any "active call" globals. Prefer RAII
  for them, so that the unwinding restores them on its way out. That is the
  frame-scoped state; there is no global exception state to restore.
- **Keep the nested call out of line.** A `NOINLINE` function keeps the
  dispatch loop's frame small; recursion depth depends on that frame's size.

## Step 3. Selection: by prototype, rethrow the rest

The language's `catch` clauses compile to tests on the bound value:

```cpp
case OP_CATCH_MATCH: {                 // except T / case e: T / (catch T e)
    const ProtoObject* type = pop();
    const ProtoObject* value = peek();
    push(value->isInstanceOf(ctx, type) == PROTO_TRUE ? PROTO_TRUE : PROTO_FALSE);
    break;
}
case OP_RERAISE:                       // no clause matched
    throw;                             // only valid inside the C++ catch, see below
```

The test is the prototype chain. A type is any value the language can name:
its own classes, an imported module's export (`geometry.NegativeRadius`), or
another language's class. Matching by an integer class id, a C++ type, a
string prefix or a per-class marker attribute does not work for foreign
types (rule 14, `exception.foreign_type_caught`).

**The catch-all form** (`catch (e)`, `except BaseException`,
`(catch Object e)`, `on: Exception do:`, `case e: Throwable`) catches every
value, including a thrown SmallInteger or another language's object (E4).
Whatever the language's own notion of "throwable", a foreign value must be
catchable by one documented form.

**Rethrow unchanged.** When the clauses run outside the C++ `catch` (as in
step 2, where the handler runs in the enclosing dispatch), keep the original
exception to rethrow it, not only its value:

```cpp
} catch (const proto::ProtoException&) {
    f.pendingRethrow = std::current_exception();   // frame state, not global
    ...
}
// OP_RERAISE:
std::rethrow_exception(f.pendingRethrow);
```

Raising `e.value()` again would create a new exception, lose a subclass's
extras and take a second pin. It is still correct, but it is worse.

## Step 4. `finally`

```cpp
try {
    dispatch(f, bodyBegin, bodyEnd);
} catch (...) {
    runFinally(f);                     // the language's finally block
    throw;
}
runFinally(f);
```

`catch (...)` runs the cleanup for every exception, including a foreign C++
one on its way to the top level. A `finally` written as
`catch (const proto::ProtoException&)` would skip cleanups exactly when the
process is in the most trouble.

If the `finally` block itself raises, the new exception replaces the old
one, as in every mainstream language. C++ does this naturally: the new
`throw` leaves the `catch (...)` block.

## Step 5. The native-call boundary

Where the interpreter calls native code, translate foreign C++ errors
(tutorial [04](04_foreign_cpp_exceptions.md)):

```cpp
const ProtoObject* callNative(ProtoContext* ctx, NativeFn fn, Args a) {
    try {
        return fn(ctx, a);
    } catch (const proto::ProtoException&) {
        throw;
    } catch (const std::exception& e) {
        ctx->raise(newInstance(ctx, lang.RuntimeError, e.what()));
    }
}
```

Then install the error factory (tutorial [02](02_error_factory.md)), so
protoCore's own errors arrive as the language's classes without any
translation.

## Step 6. Leaving the runtime

Whatever the embedding API returns to C++ (an `eval`, a `call`, a
cross-language import) lets an unhandled error leave as the
`ProtoException`. It must not convert it into a string, a status code or a
private C++ type. A C++ embedder or another language then catches it by
prototype like any other error.

At the process's top level, and at every thread, actor and event-loop entry,
report it the language's way. Then report `std::exception` and `...` instead
of reaching `std::terminate`.

## Step 7. Prove it: conformance rule 14

A runtime's conformance host implements two capabilities
(`protoCoreConformance.h`). The reference host does it in C++:

```cpp
int catchForeignError(const ProtoObject* errorType, const ProtoObject* raising,
                      const ProtoObject** caught) override {
    ProtoContext* ctx = mainContext();
    try {
        (void) invokeMethodObject(ctx, raising);
    } catch (const ProtoException& e) {
        if (!e.isInstanceOf(ctx, errorType)) throw;
        *caught = e.value();
    }
    return 1;
}

int escapeLanguageError() override {
    ProtoContext* ctx = mainContext();
    ctx->raise(ctx->newObject(false));
}
```

A real runtime does the same through its own evaluator. It binds `errorType`
and `raising` to names, runs a source fragment such as
`try { raising() } catch (e) { if (!(e instanceof errorType)) throw e; caught = e }`,
and stores the caught value.

The cases:

| Case | Checks | Fails on |
|---|---|---|
| `exception.kernel_errors_are_proto` | protoCore's `1 / 0` is a `ProtoException` built by the space's factory | a C++ standard exception |
| `exception.value_pinned_in_flight` | a kept exception's value survives collections | a value collected in flight |
| `exception.foreign_type_caught` | a handler for a module's base type catches its subtype, with the same value; a handler for an unrelated type lets it leave unchanged | matching by C++ type or id, catching every value, copying |
| `exception.language_error_escapes` | an unhandled language error leaves as a `ProtoException` | a string, a `std::runtime_error`, a private type |

The self-check suite runs two mutant hosts, one that catches every value and
one that escapes as `std::runtime_error`, to prove that the cases fail when
they should (rule 10).

## Checklist

- [ ] `throw` and every VM-raised error raise a `ProtoException` (or
  subclass) carrying the language's object.
- [ ] Every handler catches `const proto::ProtoException&`.
- [ ] Selection is the prototype chain, so a module's or another language's
  type can be named.
- [ ] The catch-all form catches every value, and is documented.
- [ ] An unmatched error is rethrown unchanged.
- [ ] `finally` / `ensure:` use `catch (...)` and `throw;`.
- [ ] The error factory is installed, with all five kinds mapped.
- [ ] `std::exception` is translated at the native-call boundary;
  `ProtoException` passes through.
- [ ] Top level, threads, actors and event loops report `ProtoException`,
  `std::exception` and `...`.
- [ ] No runtime-side pin for errors in flight.
- [ ] The rule 14 cases pass.

Next: [06 — The same module from every language](06_languages.md).
