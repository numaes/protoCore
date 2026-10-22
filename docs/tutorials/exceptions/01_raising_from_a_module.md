# 01 — Raising from a module, catching by prototype

**Example:** [`01_catch_by_prototype.cpp`](../../../examples/exceptions/01_catch_by_prototype.cpp)
and the module [`geometry_module/`](../../../examples/exceptions/geometry_module/).

You will write a module that defines its own error types and raises them,
and a C++ caller that catches them. Neither side names a language.

## Step 1. Error types are prototypes

An error type is an ordinary protoCore object. Subtypes are its children:

```cpp
const ProtoObject* base = ctx->newObject(false)->setAttribute(
    ctx, sym(ctx, "name"), ctx->fromUTF8String("GeometryError"));
const ProtoObject* negative = base->newChild(ctx)->setAttribute(
    ctx, sym(ctx, "name"), ctx->fromUTF8String("NegativeRadius"));
const ProtoObject* degenerate = base->newChild(ctx)->setAttribute(
    ctx, sym(ctx, "name"), ctx->fromUTF8String("DegenerateTriangle"));
```

The `name` attribute is only for messages and printing; selection never reads
it. The module exports the types like any other value, as attributes of the
module object, so callers can name them:

```cpp
m = m->setAttribute(ctx, sym(ctx, "GeometryError"), base);
m = m->setAttribute(ctx, sym(ctx, "NegativeRadius"), negative);
m = m->setAttribute(ctx, sym(ctx, "DegenerateTriangle"), degenerate);
```

`sym` is `ProtoString::createSymbol`. Attribute keys are always interned
symbols ([EMBEDDER-CONFORMANCE.md](../../EMBEDDER-CONFORMANCE.md), rule 4).

## Step 2. Raise an instance

An error is an instance (a child) of its type, with whatever attributes the
module wants. By convention it has a `message`:

```cpp
[[noreturn]] void raiseGeometryError(ProtoContext* ctx, const ProtoObject* module,
                                     const char* typeName, const std::string& message) {
    const ProtoObject* type = module->getAttribute(ctx, sym(ctx, typeName));
    const ProtoObject* error = type->newChild(ctx)->setAttribute(
        ctx, sym(ctx, "message"), ctx->fromUTF8String(message.c_str()));
    ctx->raise(error);
}

const ProtoObject* circleArea(ProtoContext* ctx, const ProtoObject* self, const ParentLink*,
                              const ProtoArguments* args, const ProtoKeywordArguments*) {
    const double r = number(ctx, args, 0);
    if (r < 0)
        raiseGeometryError(ctx, self, "NegativeRadius",
                           "circleArea: the radius " + std::to_string(r) + " is negative");
    return ctx->fromDouble(M_PI * r * r);
}
```

Notes:
- **`ctx->raise(error)` is `throw ProtoException(ctx, error)`.** The
  exception pins `error` until it is caught and destroyed, so nothing else
  needs to keep it alive.
- **The method finds its types through `self`**, the module object it was
  called on. A module may instead keep them in a `ProtoRootSet`. A bare
  `static const ProtoObject*` is not enough: a C++ pointer is not a GC root.
- **Nothing here is language-specific.** JavaScript, Python, Smalltalk, Scala
  and Clojure all catch this error by naming `NegativeRadius` or
  `GeometryError` (tutorial [06](06_languages.md)).

## Step 3. Catch by prototype

The caller catches one C++ type and asks the value's prototype chain:

```cpp
try {
    (void) callModule(ctx, geometry, "circleArea", {ctx->fromInteger(-1)});
} catch (const ProtoException& e) {
    check(e.isInstanceOf(ctx, NegativeRadius), "the value is a NegativeRadius");
    check(e.isInstanceOf(ctx, GeometryError), "... and therefore a GeometryError");
    check(!e.isInstanceOf(ctx, DegenerateTriangle), "... and not a DegenerateTriangle");
}
```

`isInstanceOf` is protoCore's chain test (`ProtoObject::isInstanceOf`). An
instance of `NegativeRadius` is an instance of `GeometryError` because
`GeometryError` is on its chain.

## Step 4. Catch the base type, rethrow the rest

A handler written for the module's base type catches every present and
future subtype. What it does not handle it rethrows **unchanged**:

```cpp
auto areaOrZero = [&](double a, double b, double c) -> double {
    try {
        return callModule(ctx, geometry, "triangleArea",
                          {ctx->fromDouble(a), ctx->fromDouble(b), ctx->fromDouble(c)})
            ->asDouble(ctx);
    } catch (const ProtoException& e) {
        if (!e.isInstanceOf(ctx, GeometryError)) throw;   // not ours: let it go
        return 0.0;
    }
};
```

Write `throw;`, never `throw e;` and never `ctx->raise(e.value())`. `throw;`
rethrows the same exception object: no copy, no new pin, and any extras a
runtime's subclass carries (a traceback, a line number) are preserved.

The example then checks that a handler for an unrelated type
(`NetworkError`) lets the error through with the same value:

```cpp
try {
    try {
        (void) callModule(ctx, geometry, "circleArea", {ctx->fromInteger(-5)});
    } catch (const ProtoException& e) {
        inner = e.value();
        if (!e.isInstanceOf(ctx, NetworkError)) throw;
    }
} catch (const ProtoException& e) {
    check(e.value() == inner, "the rethrown exception carries the same value");
}
```

## What you have

- A module that defines and raises its own error types without knowing who
  calls it.
- A caller that selects errors by prototype, with one `catch` type.
- The two rules every handler follows: catch the base, rethrow with `throw;`.

Next: [02 — protoCore's errors and the error factory](02_error_factory.md).
