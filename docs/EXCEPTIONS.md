# Exceptions

**Normative** as of protoCore 3.0.0. Applies to protoCore itself and to every
runtime, module and embedder built on it. The tutorials are in
[tutorials/exceptions/](tutorials/exceptions/README.md); the compiled examples
are in [examples/exceptions/](../examples/exceptions/); conformance rule 14 is in
[EMBEDDER-CONFORMANCE.md](EMBEDDER-CONFORMANCE.md#the-rule-table).

---

## 1. Why: one error, many languages

On protoCore, one process can run modules written in C++, JavaScript, Python,
Smalltalk, Scala and Clojure. They share values, not copies: a list built in
Python is the same `ProtoList` a JavaScript function reads.

Errors must cross in the same way. Without a shared model, each language only
understands its own errors:

- a C++ module that reports "file not found" is a string to every caller;
- a Python `ValueError` is an opaque C++ object to JavaScript;
- a `catch` in Scala cannot name an error type that a Smalltalk module
  defines.

The model in this document makes an error a **value** whose **type is its
prototype chain**. Every language already selects values by prototype; with
this model it selects errors the same way:

```javascript
// JavaScript
const geometry = require('example/geometry');
try {
    geometry.circleArea(-1);
} catch (e) {
    if (!(e instanceof geometry.NegativeRadius)) throw e;   // not ours
}
```

```python
# Python
import example.geometry as geometry
try:
    geometry.circleArea(-1)
except geometry.GeometryError as e:     # the base type catches every subtype
    print(e.message)
```

```smalltalk
"Smalltalk"
geo := Import from: 'example/geometry'.
[geo circleArea(-1)] on: geo NegativeRadius do: [:e | e return: 0]
```

```scala
// Scala
import example.geometry as g
try g.circleArea(-1)
catch case e: g.GeometryError => 0.0
```

```clojure
;; Clojure
(def geometry (import-module "example/geometry"))
(try (geometry/circleArea -1)
     (catch geometry/NegativeRadius e (ex-message e)))
```

The module is C++ in all three cases (see
[examples/exceptions/geometry_module](../examples/exceptions/geometry_module/GeometryModule.cpp)).
Its error type is an exported prototype, and no language needs to know it in
advance.

---

## 2. The model

| # | Rule |
|---|---|
| **E1** | **One C++ type.** A language-level throw (`throw`, `raise`, an unhandled `signal`) that unwinds is a C++ `throw` of `proto::ProtoException`, or of a class derived from it, carrying the thrown value. |
| **E2** | **The type of an error is its value's prototype chain.** It is never a C++ class, an integer id, a string prefix or a marker attribute. |
| **E3** | **A handler catches the base type and selects by prototype.** It catches `const proto::ProtoException&`, then asks `e.isInstanceOf(ctx, T)` (or the language's own test over the same chain). What it does not select it rethrows unchanged (`throw;`). |
| **E4** | **Any value can be thrown.** An object, a string, a SmallInteger, `PROTO_NONE`. A language that only throws its own exception objects still catches foreign values with its catch-all form (`on: Exception do:` in Smalltalk, `case e: Throwable` in Scala, `(catch Object e)` in Clojure, `except Exception` in Python, `catch (e)` in JavaScript). [06_languages.md](tutorials/exceptions/06_languages.md) says exactly which form, per language. |
| **E5** | **protoCore's own errors follow the same model.** protoCore raises a `ProtoException` whose value is built by the space's **error factory**; it never throws a C++ standard exception at a caller. |
| **E6** | **A C++ standard exception is not a language error.** A `std::exception` from a C++ library is a module defect. A runtime translates it at its native-call boundary into its generic error, with `what()` as the message. At the top level, any other C++ exception is reported, never left to `std::terminate`. |
| **E7** | **An error that leaves a runtime leaves as a `ProtoException`.** An unhandled language error that returns to a C++ embedder, or into another language, carries the language's exception object, not a string. |
| **E8** | **The value is alive while the exception is in flight.** protoCore pins it until the last copy of the exception is destroyed. A runtime does not pin thrown values itself. |

E3 and E7 are what make the model work across languages: every catch site
selects by the same test, and everything that crosses a boundary can be
tested.

---

## 3. The API

All declarations are in `protoCore.h`.

### 3.1 `proto::ProtoException`

```cpp
class ProtoException {
public:
    ProtoException(ProtoContext* context, const ProtoObject* value);
    virtual ~ProtoException();
    ProtoException(const ProtoException&) = default;
    ProtoException& operator=(const ProtoException&) = default;

    const ProtoObject* value() const noexcept;
    bool isInstanceOf(ProtoContext* context, const ProtoObject* type) const;
};
```

**Construction.**
- `value` is the thrown value; `nullptr` is taken as `PROTO_NONE`.
- When the value is a heap cell, it is pinned in the root set
  `protocore-exceptions-in-flight` of `context->space`.
- Tagged values (SmallInteger, booleans, `PROTO_NONE`, Unicode characters)
  are never collected and are not pinned.

**Copies share the pin.** `throw` copies the exception, `std::exception_ptr`
keeps one more copy, and the last copy releases the pin. Copying costs one
`std::shared_ptr` increment.

**`isInstanceOf(context, type)`.**
- It is `value()->isInstanceOf(context, type) == PROTO_TRUE`, that is,
  whether `type` is on the value's prototype chain.
- `false` for a null or `PROTO_NONE` type.
- `false` when `type` is the value itself: an object is not an instance of
  itself.

**Not a `std::exception`.** A `catch (const std::exception&)` written for
library failures does not catch a language value. A runtime that already had
such catch sites keeps them for E6, its translation of foreign C++ errors.

**Subclasses.** A runtime may derive from `ProtoException` to carry extras,
such as a traceback, a source line or an "inside an actor" flag. Handlers
still catch the base (E3). An extra is never the only place the error's
meaning lives: another language sees only `value()`.

**One identity across shared libraries.** The virtual destructor is defined
in libprotoCore (it is the class's key function), so the vtable and the
`typeinfo` are emitted only there. A `throw` in a module's `.so` matches a
`catch` in a runtime's executable.
[`examples/exceptions/`](../examples/exceptions/CMakeLists.txt) builds its
module as a separate shared library to keep this tested. A runtime must not
define its own copy of the class or compile protoCore's sources into its own
binary.

**Lifetime.** An exception must not outlive its space. Destroying the space
releases its root sets; an exception destroyed after that would release a
pin in freed memory. A runtime that stores errors in futures or promises
destroys them before the space, which it already must do for every
`ProtoObject*` it holds.

### 3.2 Raising

```cpp
[[noreturn]] void ProtoContext::raise(const ProtoObject* value);
[[noreturn]] void ProtoContext::raiseError(ProtoErrorKind kind, const char* message);
```

- `ctx->raise(v)` is `throw ProtoException(ctx, v)`. It is the C++ spelling
  of a language's `throw v`.
- `ctx->raiseError(kind, message)` builds the value with the space's error
  factory (§3.4) and raises it. protoCore uses it for its own errors; a C++
  module may use it for an error that is one of the five kinds, so the caller
  sees the language's own class.

### 3.3 `ProtoErrorKind`: the errors protoCore raises

| Kind | Raised by | Message |
|---|---|---|
| `InvalidArgument` | `fromString` with an empty or malformed literal or a base outside 2..36; integer `toString` with a base outside 2..36; `shiftLeft`/`shiftRight` by a negative amount | "Invalid argument for Integer::fromString.", "Invalid base ...", "Shift amount cannot be negative." |
| `TypeMismatch` | An integer operation (`asLong`, `negate`, `abs`, `sign`, `compare`, `add`, `subtract`, `multiply`, `divide`, `modulo`, bitwise operations, shifts) on a value that is not an integer; `modulo` mixing an integer and a double | "Object is not an integer type.", "Objects are not integer types for ..." |
| `Overflow` | `asLong` of a LargeInteger outside the 64-bit range | "LargeInteger value exceeds long long range." |
| `DivisionByZero` | Integer `divide` and `modulo` by zero | "Division by zero." |
| `ArityMismatch` | A call that binds more positional arguments than the method's parameters (the `ProtoContext` constructor) | "Too many positional arguments provided." |

`kProtoErrorKindCount` is 5. `ProtoSpace::errorKindName(kind)` answers the
kind's name.

Two failures remain C++ exceptions because they are defects, not language
errors:
- `std::logic_error("Internal division by zero.")` is an internal invariant
  of the bignum division.
- `std::logic_error("Too many positional arguments for a root context.")` is
  thrown when a root context is built with arguments, which no runtime does.

**The arity error leaves the caller intact.** It is raised before the new
context takes slots or becomes the thread's current context, and it is built
in the caller's context. Until 3.0.0 the `std::invalid_argument` was thrown
after the context had registered itself. Because a constructor that throws
runs no destructor, the thread was left pointing at a dead stack object.

### 3.4 The error factory

```cpp
typedef const ProtoObject* (*ProtoErrorFactory)(
    ProtoContext* context, ProtoErrorKind kind, const char* message);

void              ProtoSpace::setErrorFactory(ProtoErrorFactory factory);
ProtoErrorFactory ProtoSpace::getErrorFactory() const;
const ProtoObject* ProtoSpace::getErrorPrototype(ProtoContext* context, ProtoErrorKind kind);
const ProtoObject* ProtoSpace::getErrorRootPrototype(ProtoContext* context);
static const char* ProtoSpace::errorKindName(ProtoErrorKind kind);
```

**The default factory** answers a child of `getErrorPrototype(context, kind)`
with a `message` attribute:
- every kind's prototype is a child of `getErrorRootPrototype`, the
  "ProtoCoreError" prototype;
- each prototype has a `name` attribute, the kind's name;
- the prototypes are built once per space, on first use, and pinned for the
  space's lifetime.

**A runtime installs its own factory** so that protoCore's errors arrive as
the language's exceptions: a division by zero becomes Python's
`ZeroDivisionError`, Scala's `ArithmeticException` or Smalltalk's
`ZeroDivide`. The language's handlers then catch them by their usual names.

The factory's contract:
- **Install it once**, while the runtime starts and before any other thread
  runs code in the space. `nullptr` restores the default.
- **It runs on the raising thread**, with the raising context. It may
  allocate.
- **It must return a value and must not throw.** It builds the instance the
  way the language's constructor would, message included, but without running
  user code that could fail.
- **It reaches the language's classes through state that is a GC root**: a
  `ProtoRootSet`, a module's exports, or the runtime's environment. A
  `static const ProtoObject*` alone does not keep a class alive
  ([EMBEDDER-CONFORMANCE.md](EMBEDDER-CONFORMANCE.md), rule 3).
- **One factory per space.** A space hosts one runtime's main program. A
  module written in another language raises its own language's classes, and
  those are caught under E4.

Tutorial [02](tutorials/exceptions/02_error_factory.md) builds a factory step
by step. Each runtime's mapping table is in
[tutorials/exceptions/06_languages.md](tutorials/exceptions/06_languages.md).

### 3.5 `ProtoThread` entry

A `ProtoThread`'s method runs inside protoCore's thread entry. If a
`ProtoException`, a `std::exception` or any other exception escapes the
method:
- the entry stops it;
- with `PROTO_THREAD_DIAG` set, it prints one line;
- the thread then leaves the space in order: the exit hook runs and the
  thread leaves the stop-the-world quorum.

A runtime catches its own errors inside the thread's method, to report them
the language's way or to reject a future. The entry is the last line of
defence, not the reporting mechanism.

---

## 4. Obligations of a runtime

The checklist a runtime meets. Conformance rule 14 checks the observable part.

1. **Throw.** The language's throw, and every runtime error the VM raises,
   becomes a `ProtoException` before it can leave the frame of the language
   that raised it. A lazy internal representation, such as a class name and a
   message materialised only when a handler needs the object, is allowed only
   if it is converted before any boundary: a native call into another
   runtime, a return to an embedder, a thread entry or a future.
2. **Catch.**
   - Every handler, including `finally`, `ensure:` and the REPL, catches
     `const proto::ProtoException&`.
   - Selection walks the value's prototype chain, so a type defined by a
     module, in C++ or in another language, can be named in the language's
     handler syntax.
   - A catch-all form catches every value (E4).
   - A value that is not selected is rethrown with `throw;`, never copied
     into a new exception and never converted into a string.
3. **Install an error factory** for protoCore's five kinds (§3.4).
4. **Translate foreign C++ exceptions** at the native-call boundary:
   - `std::exception` becomes the language's generic runtime error, with
     `what()` as the message;
   - a `ProtoException` passes through untouched;
   - at the top level and at every thread, actor or event-loop entry, `...`
     is reported instead of reaching `std::terminate`.
5. **Leave as a `ProtoException`.** An unhandled error that leaves the
   runtime (embedding API, cross-language import) carries the language's
   exception object.
6. **Do not pin thrown values.** protoCore does (E8). A runtime's own root set
   for in-flight errors is redundant and should be removed.
7. **Document** in the runtime's own docs: the catch-all form, how a module's
   type is named in a handler, the factory's mapping table and the top-level
   behaviour.

---

## 5. Cost

C++ exceptions are table-driven ("zero-cost").

**Code that does not throw** executes no extra instruction for a `try`. The
`try` adds unwind-table entries, which can change code layout.

**A throw** costs the unwinder's work for every frame it crosses:
- finding the frame's FDE (`_Unwind_Find_FDE` / `_dl_find_object` on Linux);
- running the personality routine;
- scanning the frame's call-site table, twice: once in the search phase and
  once in the cleanup phase.

That is much more than a flag test per instruction. A large interpreter
function with hundreds of call sites pays it on every frame.

Measured, on protoJS's bytecode interpreter (2026-10-21,
`protoJS/benchmarks/reports/2026-10-21-cpp-exceptions/`), against its
previous pending-flag design:

| Case | Cost |
|---|---|
| A throw caught in the same function | 11.5x |
| A thrown `Error` | 1.7x |
| A throw across five frames | 4.7x |
| Entering a `try` that does not throw | 1.52x |
| The geomean of the standard benchmarks | +5.8 % |

protoPython's raise-and-catch went the other way, from 192,657 to 83,539
cycles, because its previous mechanism was heavier.

This model was chosen knowingly. It applies everywhere, without internal
shortcuts, because one mechanism is what lets errors cross every boundary,
and a runtime cannot know in advance which frames a given error will cross.
The unification itself adds nothing to the cost: catching the base class
costs the same as catching a derived one, the prototype test is the same
chain walk a language handler already did, and the pin was already paid by
the runtimes that pinned.

---

## 6. Migrating from protoCore 2.x

| 2.x | 3.0.0 |
|---|---|
| Integer operations threw `std::runtime_error` / `std::invalid_argument` / `std::overflow_error` | They raise `ProtoException`; the value comes from the error factory (§3.3, §3.4) |
| `catch (const std::overflow_error&)` around `asLong` | `catch (const ProtoException& e)` and `e.isInstanceOf(ctx, space->getErrorPrototype(ctx, ProtoErrorKind::Overflow))`, or the language's class when a factory is installed |
| Parsing `what()` ("Division by zero.") to pick a language class | Install a factory: the kind arrives as an enum |
| Too many positional arguments threw `std::invalid_argument` from the `ProtoContext` constructor, after registration | `ProtoException` of kind `ArityMismatch`, raised before registration and built in the caller's context |
| Each runtime's own exception class and its own pin (`JSException`, `PyException`, `ClojureThrow`, ...) | A subclass of `ProtoException`, or the base itself; protoCore pins |
| An escaping error was a string or a runtime-private C++ type | A `ProtoException` carrying the language's exception object |

---

## 7. Questions

**Why not derive from `std::exception`?**
A language value has no meaningful `what()`: a thrown `42` or a Python
exception object is not a C string. Also, every runtime has
`catch (const std::exception&)` sites written for C++ library failures (E6),
and those would swallow language errors. protoScala hit exactly this before
3.0.0 and made its own throw type avoid `std::runtime_error` on purpose.

**Why not a pending-exception flag, as CPython does?**
A flag must be checked after every call that can fail, in every runtime and
every module, forever. A missed check is a silent wrong answer. A flag is
also per-thread state that every boundary must save and restore. C++
unwinding cannot be forgotten, crosses every C++ frame, and runs destructors.
The embedders on protoCore are C++, so there is no C boundary to stop it.

**Can a module raise an error without knowing which language calls it?**
Yes. That is the point of E2. The module exports its error prototypes and
raises instances of them (tutorial
[01](tutorials/exceptions/01_raising_from_a_module.md)). For the five
protoCore kinds, `ctx->raiseError(kind, message)` gives the caller its own
class.

**What does a language see when it catches another language's exception?**
The same value: there is no conversion and no copy. A module's error caught
in Python, Scala or Clojure is the very object the module raised. Its
attributes (`message`, `name`, ...) are readable, and the language's type test
against the module's type is true. Each runtime has a test for this against
the geometry module. Errors crossing directly between two languages, such as
a Python exception caught in JavaScript, follow the same rule, but in October
2026 no cross-runtime test exercises them yet.

**Can an exception carry a value of another space?**
The pin is taken in the space of the raising context. Values cross spaces
under the rules of [MemoryModel.md](MemoryModel.md) and
[GLOBAL_MUTABLE_TABLE.md](GLOBAL_MUTABLE_TABLE.md). A runtime that hands an
error to another space hands it over like any other value.

**Does `resume:` (Smalltalk) fit this model?**
Yes. A resumable signal runs its handler before any unwinding, on the
runtime's own handler stack, and only the decision to unwind throws. That
unwind is internal to the runtime's frames, so it does not cross a boundary.
A foreign error arriving from a module is signalled as a non-resumable
exception with the same value. See the protoST section of
[06_languages.md](tutorials/exceptions/06_languages.md).
