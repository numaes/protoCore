# 02 — protoCore's errors and the error factory

**Example:** [`02_error_factory.cpp`](../../../examples/exceptions/02_error_factory.cpp).

protoCore itself can fail: integer division by zero, a malformed integer
literal, an operand that is not an integer, a value too large for 64 bits, a
call with too many arguments. These errors are raised like any other: a
`ProtoException`, selected by prototype. A runtime decides which classes they
are instances of.

## Step 1. Without a factory: protoCore's own prototypes

```cpp
try {
    (void) ctx->fromInteger(7)->divide(ctx, ctx->fromInteger(0));
} catch (const ProtoException& e) {
    e.isInstanceOf(ctx, space.getErrorPrototype(ctx, ProtoErrorKind::DivisionByZero));  // true
    e.isInstanceOf(ctx, space.getErrorRootPrototype(ctx));                               // true
    // e.value()'s "message" is "Division by zero."
}
```

The default value is a child of the kind's prototype, which is a child of
the "ProtoCoreError" root. Each prototype has a `name` attribute: one of
"DivisionByZero", "InvalidArgument", "TypeMismatch", "Overflow" or
"ArityMismatch". The full table of what raises each kind is in
[EXCEPTIONS.md §3.3](../../EXCEPTIONS.md#33-protoerrorkind-the-errors-protocore-raises).

This is enough for a C++ embedder. A language, however, wants
`except ZeroDivisionError` to catch a protoCore division by zero.

## Step 2. A language's hierarchy

The example's toy language has:

```
Exception
 +-- ArithmeticError
 |    +-- ZeroDivisionError
 +-- ValueError
 +-- TypeError
```

```cpp
g_lang.Exception = named(ctx, nullptr, "Exception");
g_lang.ArithmeticError = named(ctx, g_lang.Exception, "ArithmeticError");
g_lang.ZeroDivisionError = named(ctx, g_lang.ArithmeticError, "ZeroDivisionError");
g_lang.ValueError = named(ctx, g_lang.Exception, "ValueError");
g_lang.TypeError = named(ctx, g_lang.Exception, "TypeError");
ProtoRootSet* classes = space.createRootSet("toy-language-classes");
for (const ProtoObject* c : {g_lang.Exception, g_lang.ArithmeticError, g_lang.ZeroDivisionError,
                             g_lang.ValueError, g_lang.TypeError})
    classes->add(c);
```

The classes are pinned in a `ProtoRootSet`. The factory reaches them through
a C++ pointer, and a C++ pointer does not keep an object alive.

## Step 3. The factory

```cpp
const ProtoObject* toyErrorFactory(ProtoContext* ctx, ProtoErrorKind kind, const char* message) {
    const ProtoObject* type = g_lang.Exception;
    switch (kind) {
        case ProtoErrorKind::DivisionByZero:  type = g_lang.ZeroDivisionError; break;
        case ProtoErrorKind::Overflow:        type = g_lang.ArithmeticError; break;
        case ProtoErrorKind::InvalidArgument: type = g_lang.ValueError; break;
        case ProtoErrorKind::TypeMismatch:
        case ProtoErrorKind::ArityMismatch:   type = g_lang.TypeError; break;
    }
    return type->newChild(ctx)->setAttribute(ctx, sym(ctx, "message"), ctx->fromUTF8String(message));
}

space.setErrorFactory(&toyErrorFactory);
```

Its contract ([EXCEPTIONS.md §3.4](../../EXCEPTIONS.md#34-the-error-factory)):

- **Install it once at start-up**, before other threads run code in the
  space.
- **It runs on the raising thread and may allocate.**
- **It must return a value and must not throw.** It builds the instance as
  the language's constructor would. In a real runtime that means the same
  attributes (`args` in Python, `message` and `stack` in JavaScript), but
  without running user code that could fail.
- **Handle every kind.** A `switch` without `default`, as above, lets the
  compiler warn when protoCore adds a kind.

## Step 4. The language's names now work

```cpp
try {
    (void) ctx->fromInteger(7)->modulo(ctx, ctx->fromInteger(0));
} catch (const ProtoException& e) {
    e.isInstanceOf(ctx, g_lang.ZeroDivisionError);   // true
    e.isInstanceOf(ctx, g_lang.ArithmeticError);     // true: a handler for the parent catches it
}
```

It also works through a module. The geometry module's `parseRadius` calls
`ctx->fromString(text, 10)` and does not catch anything:

```cpp
try {
    (void) callModule(ctx, geometry, "parseRadius", {ctx->fromUTF8String("ten")});
} catch (const ProtoException& e) {
    e.isInstanceOf(ctx, g_lang.ValueError);   // true
}
```

The module did not know the language, and the language did not know the
module. Each saw only protoCore.

## Step 5. A module can raise a protoCore kind on purpose

A C++ module whose error is one of the five kinds can raise it through the
factory, and the caller sees its own class:

```cpp
if (count < 0) ctx->raiseError(ProtoErrorKind::InvalidArgument, "count must not be negative");
```

A module error that is not one of the five kinds is the module's own type
(tutorial [01](01_raising_from_a_module.md)).

## What every runtime maps

Each runtime's table is in [06](06_languages.md). For example:

| Kind | Smalltalk | Scala | Clojure |
|---|---|---|---|
| `DivisionByZero` | `ZeroDivide` | `ArithmeticException` | `ArithmeticException` |
| `InvalidArgument` | `InvalidArgument` | `NumberFormatException` | `NumberFormatException` (parsing) / `IllegalArgumentException` |
| `TypeMismatch` | `InvalidArgument` | `ClassCastException` | `ClassCastException` |

06 has the full tables for every runtime.

Next: [03 — Unwinding, cleanup and lifetime](03_unwinding_and_lifetime.md).
