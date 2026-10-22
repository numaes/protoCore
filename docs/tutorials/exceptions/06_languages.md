# 06 — The same module from every language

The geometry module of tutorials 01–04
([`GeometryModule.cpp`](../../../examples/exceptions/geometry_module/GeometryModule.cpp))
is written in C++ and knows no language. This page catches its errors from
each runtime on protoCore.

For each language it gives:
- **a program**: each was run against the module and its output checked,
  and the same checks are in that runtime's own suite;
- **the catch-all form** (EXCEPTIONS.md, E4);
- **the error factory's table**: which class protoCore's own errors become.

Every runtime makes the same checks on the module:

| Call | Raises | Caught as |
|---|---|---|
| `circleArea(-1)` | the module's `NegativeRadius` | `NegativeRadius`, and `GeometryError`; not an unrelated type |
| `triangleArea(1, 2, 10)` | the module's `DegenerateTriangle` | `DegenerateTriangle` / `GeometryError` |
| `parseRadius("ten")` | protoCore's `InvalidArgument`, through the space's factory | the language's ValueError-like class |
| integer `1 / 0` in the language | protoCore's `DivisionByZero`, through the factory | the language's division-by-zero class |
| `legacyArea(-1)` | `std::runtime_error` (the anti-pattern) | the language's generic runtime error, message `legacyArea: negative radius` |

**Loading the module.** The runtimes' release binaries do not load native
providers by themselves yet. Each runtime's test suite uses a small host or
plug-in that links `libprotocore_example_geometry` and calls
`example::registerGeometryProvider()`. Each section names that host or
plug-in.

---

## Smalltalk (protoST)

```smalltalk
"geometry.st -- catch a C++ module's error types by name"
geo := Import from: 'example/geometry'.
{ [geo circleArea(-1)] on: geo NegativeRadius do: [:e | e messageText].
  [geo circleArea(-1)] on: geo GeometryError do: [:e | e name].
  [[geo circleArea(-1)] on: Error do: [:e | 'an Error?']]
      on: geo GeometryError do: [:e | 'not an Error'].
  [geo triangleArea(1, 2, 10)] on: geo DegenerateTriangle do: [:e | e messageText].
  [geo parseRadius('ten')] on: InvalidArgument do: [:e | e class name].
  [10 // 0] on: ZeroDivide do: [:e | e class name].
  [geo legacyArea(-1)] on: Error do: [:e | e messageText].
  [geo circleArea(-1)] on: geo GeometryError do: [:e | e return: 0] }
```

Run (protoST built against protoCore's `build_release`):

```bash
cmake -S . -B build -DCMAKE_DISABLE_FIND_PACKAGE_protoCore=ON && cmake --build build
build/tests/protost_umd_host --print-last geometry.st
```

It prints `#('circleArea: the radius -1.000000 is negative' 'NegativeRadius'
'not an Error' 'triangleArea: the sides do not make a triangle'
'InvalidArgument' 'ZeroDivide' 'legacyArea: negative radius' 0)`.

**How the model meets Smalltalk's resumable exceptions.**
- `signal` still runs the handler before any unwinding, on protoST's own
  handler stack.
- A `ProtoException` that arrives from a primitive or a module method is
  signalled as a Smalltalk exception with the same value, matched by
  `hasParent`.
- A module's error value is **never resumable**. Within its handler,
  `messageText`, `return:`, `retry`, `pass` and `signal` work.
- If no handler takes it, the original exception leaves protoST unchanged.
- An unhandled Smalltalk `Error` leaves as `UnhandledSTException`, a subclass
  of `ProtoException` that carries the exception object plus protoST's
  trace.

**The catch-all form is `on: Exception do:`.** It catches every signalled
value: protoST's exceptions, a module's error type, another language's
exception, and a raised value that is not an exception at all (a
SmallInteger, a plain object). It also works as a member of an exception set.
`on: Error do:` catches only protoST's errors and protoCore's own; a module's
error passes it (the `'not an Error'` line above).

| protoCore error | protoST class | Resumable |
|---|---|---|
| `DivisionByZero` | `ZeroDivide` | yes |
| `Overflow` | `ArithmeticError` | yes |
| `InvalidArgument` | `InvalidArgument` (a subclass of `Error`) | no |
| `TypeMismatch` | `InvalidArgument` | no |
| `ArityMismatch` | `Error` | no |

Details: protoST `docs/LANGUAGE.md` §8.6–8.7 and `docs/INTEROP.md`.

---

## Scala (protoScala)

```scala
import example.geometry as g

@main def run(): Unit =
  def area(r: Int): String =
    try "area " + g.circleArea(r)
    catch
      case e: g.NegativeRadius => "rejected: " + e.message
      case e: g.GeometryError  => "geometry error: " + e.name
  println(area(-1))
  val parsed =
    try g.parseRadius("ten").toString
    catch case e: NumberFormatException => "not a number"
  val legacy =
    try g.legacyArea(-1).toString
    catch case e: RuntimeException => e.getMessage
  println(parsed + "; " + legacy)
```

Run (protoScala's test plug-in registers the provider):

```bash
PROTOSCALA_PROVIDERS=build_release/tests/unit/protoscala_geometry_provider.so \
  build_release/protoscala tests/conformance/tutorial/11-exceptions-from-a-module.scala
```

Output:

```
rejected: circleArea: the radius -1.000000 is negative
not a number; legacyArea: negative radius
```

**Naming a module's type in a pattern** (protoScala decision D133):
- `case e: g.NegativeRadius` tests the value's prototype chain.
- scalac accepts this path syntax only for real types and rejects a value
  there, so no valid Scala program changes meaning.

**The catch-all forms are `case e: Throwable`, `case e: Exception`, `case e =>`
and `Try`** (decision D134). All four catch every value: a module's error,
another language's exception, and a thrown value that is not a Scala object.
On the JVM everything thrown is a `Throwable`, and a module error is not a
fatal one, so `Exception` also catches it. The more specific classes
(`RuntimeException`, `ArithmeticException`, ...) catch only their own
instances and the protoCore errors the factory maps onto them, and the fatal
VM classes (`Error`, `StackOverflowError`, `OutOfMemoryError`) never catch
foreign values. Outside a `catch`, in a `match` or `isInstanceOf`, a foreign
object also counts as an `Exception`. `throw e` rethrows a caught foreign
error. The maintainer decided this (D134 in protoScala's
`docs/DECISIONS-LOG.md`).

| protoCore error | Scala class |
|---|---|
| `DivisionByZero` | `ArithmeticException` |
| `Overflow` | `ArithmeticException` |
| `InvalidArgument` | `NumberFormatException` (an `IllegalArgumentException`) |
| `TypeMismatch` | `ClassCastException` |
| `ArityMismatch` | `IllegalArgumentException` |

The message is protoCore's text, for example "Invalid argument for
Integer::fromString."; the JVM's would be `For input string: "ten"`.

A `catch` that matches nothing, and a `finally`, rethrow the original
exception, both interpreted and in transpiled modules, so another runtime's
`ProtoException` subclass leaves protoScala intact. Details: protoScala
`docs/DESIGN.md` §7, `docs/INTEROP.md` §9, `docs/tutorial` §11.9.

**Errors between Scala and Smalltalk** (decision D135). A Scala program can
import a protoST module and send its messages (`` Counter.`new`() ``,
`c.increment()`). A Smalltalk error is then caught in Scala by its class,
exactly as a module's error is:

```scala
import st.send_lib.{Counter, CounterOverflow, UnrelatedError}

@main def run(): Unit =
  val c = Counter.`new`()
  c.increment(); c.increment()
  try c.increment()
  catch
    case e: UnrelatedError  => println("wrong")
    case e: CounterOverflow => println("CounterOverflow: " + e.getMessage)
```

The reverse also holds: a Scala exception thrown through a Smalltalk block is
caught there by `on: Exception do:` as the same object. These are protoScala's
`ProtoSTInterop` tests (`tests/unit/protost_interop.cpp`, built with
`-DPROTOSCALA_PROTOST_INTEROP=ON`). They also check unwinding across both
runtimes, with `ensure:` and `finally`.

---

## Clojure (protoClojure)

```clojure
(def geometry (import-module "example/geometry"))

(println (try (geometry/circleArea -1)
              (catch geometry/NegativeRadius e (ex-message e))))
;; circleArea: the radius -1.000000 is negative
(println (try (geometry/triangleArea 1 2 10)
              (catch geometry/GeometryError e :geometry-error)))   ;; :geometry-error
(println (try (try (geometry/circleArea -1)
                   (catch ArithmeticException e :wrong))            ;; not caught here
              (catch geometry/NegativeRadius e :rethrown-and-caught)))
(println (try (geometry/parseRadius "ten")
              (catch NumberFormatException e :factory)))            ;; :factory
(println (try (/ 1 0) (catch ArithmeticException e (ex-message e))))  ;; Divide by zero
(println (try (geometry/legacyArea -1)
              (catch RuntimeException e (ex-message e))))  ;; legacyArea: negative radius
```

Run with `build/protoclj_geometry geometry.clj`. That binary is protoClojure's
`main.cpp` linked with the example module, and is built when protoClojure is
configured against `../protoCore` with
`-DCMAKE_DISABLE_FIND_PACKAGE_protoCore=ON`.

**Naming a module's type** (protoClojure deviation D34):
- A `catch` naming something that is not a built-in class is resolved **at
  run time**, when an exception reaches that clause. It can be a local, a var,
  or a module member such as `geometry/NegativeRadius`.
- It never fails silently. An unresolved name raises
  `RuntimeException: Unable to resolve classname: X`, and a value that is not
  a prototype raises `IllegalArgumentException`.
- `(import-module "path")` loads a UMD module; `alias/member` reads a member,
  and `(alias/member args…)` calls it with the module as receiver.

**The catch-all form is `(catch Object e …)`** (or `Throwable`), which
catches any value. `Exception` also catches foreign errors. Other built-in
classes catch only their own instances. `ex-message` reads a foreign
error's `message` attribute.

| protoCore error | Clojure class |
|---|---|
| `DivisionByZero` | `ArithmeticException`, "Divide by zero" |
| `Overflow` | `ArithmeticException` |
| `InvalidArgument` (integer parsing) | `NumberFormatException` |
| `InvalidArgument` (other) | `IllegalArgumentException` |
| `TypeMismatch` | `ClassCastException` |
| `ArityMismatch` | `ArityException` |

Every built-in exception class is a protoCore prototype whose parent is its
superclass. A `try` that does not handle an exception rethrows the original,
so another runtime's subclass leaves intact. Details: protoClojure
`docs/DESIGN.md` §4.1, `LANGUAGE.md` §14, `INTEROP.md` §0.

---

## JavaScript (protoJS)

```js
require(process.argv[2]);                        // the addon registers provider:example_geometry
const geometry = require('example/geometry');    // imported through UMD

for (const r of [2, -1]) {
  try {
    console.log('area', geometry.circleArea(r));
  } catch (e) {
    if (!(e instanceof geometry.GeometryError)) throw e;   // not ours: let it go
    console.log('caught', String(e));            // NegativeRadius: circleArea: ...
  }
}

try { geometry.parseRadius('ten'); }             // protoCore's InvalidArgument
catch (e) { console.log(e instanceof SyntaxError, e.message); }

try { geometry.legacyArea(-1); }                 // throws std::runtime_error
catch (e) { console.log(e.constructor === Error, e.message); }
```

Run it with protoJS's test addon, which compiles the geometry module in.
Loading it registers the provider and puts `provider:example_geometry`
first in the resolution chain. The path must be absolute, because a
relative one is taken as a bare specifier:

```bash
protojs geometry_demo.js /abs/path/build/tests/native_addons/geometry/geometry.so
```

Output:

```
area 12.566370614359172
caught NegativeRadius: circleArea: the radius -1.000000 is negative
true Invalid argument for Integer::fromString.
true legacyArea: negative radius
```

**Naming a module's type: an `instanceof` extension.** In ECMAScript the
right side of `instanceof` must be callable. A module's type is a plain
protoCore prototype.
- **The extension:** when `T` is a non-callable protoCore object defined
  outside JavaScript, `x instanceof T` tests `T` against `x`'s prototype
  chain. A value counts as foreign when it has no JavaScript
  `Object.prototype` on its chain and no JavaScript prototype override, so
  no valid JavaScript program changes meaning.
- **The portable form** is `Object.prototype.isPrototypeOf.call(T, e)`.
  `T.isPrototypeOf(e)` does not work, because these objects inherit no
  JavaScript methods.
- **Conversions:** a foreign value converts to `"<name>: <message>"` as a
  string and to `NaN` as a number.

The deviation is documented in protoJS's `docs/API_REFERENCE.md`.

**The catch-all form is `catch (e)`**, as in the language: it catches every
value. Selection is an explicit test and `throw e`. JavaScript has no bare
rethrow, so `throw e` is a new throw of the same value, as in every
JavaScript engine.

| protoCore error | JavaScript class |
|---|---|
| `DivisionByZero` | `RangeError`, "Division by zero" |
| `Overflow` | `RangeError` |
| `InvalidArgument` | `SyntaxError` (as `BigInt("x")`) |
| `TypeMismatch` | `TypeError` |
| `ArityMismatch` | `TypeError` |

`BigInt("x")` and `1n / 0n` keep protoJS's own checks and messages. Before,
`1n % 0n` ended the process in `std::terminate`; it is now a `RangeError`.

A `std::exception` from a native function becomes an `Error` with `what()`
at the native-call boundary. Each thread converts an uncaught exception in
its own way:

| Where it happens | What it becomes |
|---|---|
| Deferred pool | a rejected `Deferred` |
| Worker thread | the worker's `error` event |
| Top level | a report, and exit code 1, inside the space's lifetime |

Details: protoJS `docs/EXCEPTIONS.md` and `docs/GC_BRIDGING.md`.

## Python (protoPython)

```python
import example.geometry as geometry      # the C++ UMD module at "example/geometry"

def area(r):
    try:
        return geometry.circleArea(r)
    except geometry.NegativeRadius as e:  # the module's own error type
        print("negative radius:", e.message)
    except geometry.GeometryError as e:   # its base type
        print("geometry error:", e)

area(-1)
try:
    geometry.triangleArea(1, 2, 10)
except geometry.GeometryError as e:
    print(type(e) is geometry.DegenerateTriangle, e)
try:
    geometry.parseRadius("ten")          # protoCore's InvalidArgument -> ValueError
except ValueError as e:
    print("ValueError:", e)
try:
    geometry.legacyArea(-1)              # a C++ std::runtime_error -> RuntimeError
except RuntimeError as e:
    print("RuntimeError:", e)
```

Run (protoPython's test host registers the provider):

```bash
cmake --build build_release --target protopy_umd_host
build_release/protopy_umd_host geometry_errors.py
```

Output:

```
negative radius: circleArea: the radius -1.000000 is negative
True triangleArea: the sides do not make a triangle
ValueError: Invalid argument for Integer::fromString.
RuntimeError: legacyArea: negative radius
```

**Importing a UMD module.** When no Python module provides a dotted name,
protoPython tries it as a slash path, so `example.geometry` loads
`example/geometry`. The result is a Python module object that holds the same
objects, with its methods bound to the module.

**Naming a module's type in `except`.**
- A Python exception class keeps CPython's `isinstance` semantics.
- A foreign type (a protoCore object that is not a Python object) matches by
  prototype chain.
- Any other Python object as the `except` type raises CPython's `TypeError`.
  Before 3.0.0 it silently did not match.

**A foreign value in Python.**
- `e.message` works.
- `str(e)`, `print(e)` and f-strings give its `message`;
  `repr(e)` is `NegativeRadius('…')`; `type(e)` is the module's type.
- `sys.exc_info()` and a bare `raise` see the value itself.
- protoPython never writes to a foreign value. It sets no `__traceback__` and
  no `__context__` on it, but the value can be a Python exception's
  `__context__` or `__cause__`.

**The catch-all forms are `except Exception`, `except BaseException` and a
bare `except:`.** All three catch a foreign value; `Exception` does so
because it is Python's usual catch-all.

| protoCore error | Python exception |
|---|---|
| `DivisionByZero` | `ZeroDivisionError("division by zero")` (CPython 3.14's text) |
| `Overflow` | `OverflowError` (protoCore's message) |
| `InvalidArgument` | `ValueError` (protoCore's message) |
| `TypeMismatch` | `TypeError` (protoCore's message) |
| `ArityMismatch` | `TypeError` (protoCore's message) |

A C++ `std::exception` at a native boundary (the opcode loop, a call from
compiled code, an HPy function) becomes `RuntimeError(what())`.
`protopyc`-compiled code follows the same rules. Details: protoPython
`docs/EXCEPTIONS.md` and `docs/CPYTHON_CONFORMANCE.md` (divergences).

---

## Comparing the catch-all forms

| Language | Catches every value | Catches only the language's errors and protoCore's |
|---|---|---|
| JavaScript | `catch (e)` | — (selection is `instanceof` and `throw e`) |
| Python | `except Exception`, `except BaseException`, bare `except:` | the other built-in classes |
| Smalltalk | `on: Exception do:` | `on: Error do:` |
| Scala | `case e: Throwable`, `case e: Exception`, `case e =>`, `Try` | the other built-in classes |
| Clojure | `(catch Object e …)`, `(catch Throwable e …)` | the other built-in classes (`Exception` also catches foreign errors) |
