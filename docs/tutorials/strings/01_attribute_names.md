# 01 — Attribute names: intern once, compare by pointer

Example: [`examples/strings/01_attribute_names.cpp`](../../../examples/strings/01_attribute_names.cpp),
test `example.strings.01_attribute_names`. Requires protoCore **3.2.0** or later.

An object stores each attribute under the **pointer** of its name, so every
spelling must have exactly one canonical pointer. This tutorial shows how a
runtime gets that pointer for the two kinds of access a language has:
`obj.balance`, whose name is in the source, and `obj['bal' + 'ance']`, whose
name exists only at run time.

## The canonical form of a string

A `ProtoString` has two representations:

| Representation | When | Canonical? |
|---|---|---|
| Inline string, in the pointer word | the spelling is at most `INLINE_STRING_MAX_BYTES` (6) UTF-8 bytes, ASCII or not | always: the content *is* the pointer |
| Heap string, a persistent AVL tree (one leaf up to 36 bytes, a rope beyond) | longer spellings | only when it is a **symbol** (`POINTER_TAG_SYMBOL`): the one copy the `SymbolTable` holds |

A heap string built at run time (`fromUTF8`, concatenation, slicing) is not a
symbol, because interning every string would hash every rope it builds. A
short spelling never reaches the table: every constructor produces the inline
form for it, so `"año"` from `createSymbol`, from `fromUTF8` and from
`"a" + "ño"` is one pointer.

## Names from the source: intern at module initialization

The names a module spells in its source are constants of the program. Intern
them once, when the module is initialized, and keep the pointers:

```cpp
struct ModuleNames {
    const ProtoString* name;
    const ProtoString* balance;
    const ProtoString* year;     // "año": 4 UTF-8 bytes, an inline string

    explicit ModuleNames(ProtoContext* ctx)
        : name(ProtoString::createSymbol(ctx, "name")),
          balance(ProtoString::createSymbol(ctx, "balance")),
          year(ProtoString::createSymbol(ctx, "año")) {}
};
```

Symbols are perennial: they are allocated outside the collector and keep their
pointer for the life of the process, so these fields need no rooting. An
access compiled from `account.balance` then costs no lookup in the table at
all:

```cpp
account->getOwnAttributeDirect(ctx, names.balance);
```

## Names computed at run time: `intern` before the access

`obj['bal' + 'ance']` produces a heap string that is not the symbol. Before
using it as a name, ask for its canonical form:

```cpp
const ProtoString* computed = concat(ctx, "bal", "ance");   // a plain heap string
const ProtoString* key = computed->intern(ctx);             // == names.balance
account->getOwnAttributeDirect(ctx, key);
```

`ProtoString::intern`:

- returns **the same pointer** when the string is already canonical (an inline
  string or a symbol), so calling it on every computed access costs a tag check
  in the common case;
- otherwise looks the content up in the `SymbolTable` **from the string
  itself** (no C string is built) and returns the symbol; a lookup of a spelling
  that is already interned allocates nothing;
- interns the spelling on first sight. That copy is perennial, which is right
  for a name and wrong for arbitrary data: intern what is used as a name, not
  every string a program builds.

## Why not let `getAttribute` do it?

`setAttribute` interns a non-canonical name, and `getAttribute` falls back to a
content lookup, so both work with a plain string, only more slowly.
`getOwnAttributeDirect`, the fast path for compiled accesses, does neither: a
plain heap string misses there **silently** and returns `nullptr`
([EMBEDDER-CONFORMANCE.md](../../EMBEDDER-CONFORMANCE.md), rule 4). Interning
in the runtime, once per source name and once per computed access, makes every
path receive a canonical name.

## Checklist for a runtime

1. Every attribute name in compiled code is a `createSymbol` result made at
   module initialization, never during the access.
2. Every computed name goes through `ProtoString::intern` before the access.
3. The runtime requires protoCore ≥ 3.2.0 (`find_package(protoCore 3.2 REQUIRED)`).
