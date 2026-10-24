// 01_attribute_names.cpp -- tutorial 01 (docs/tutorials/strings/).
//
// Attribute names are canonical strings, compared by pointer:
//   - the names a program spells in its source are interned once, when the
//     module is initialized, with ProtoString::createSymbol;
//   - a name computed at run time (obj['na' + 'me']) is interned with
//     ProtoString::intern before the access;
//   - intern of a string that is already canonical is the same pointer, and a
//     short spelling (at most 6 UTF-8 bytes, ASCII or not) is canonical as an
//     inline string, whichever way it was built.
// Checks print ok / FAIL, so the example runs as a test.
#include "protoCore.h"
#include "proto_internal.h"   // isInlineString, only to show the representation

#include <cstdio>
#include <cstdlib>

using namespace proto;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

// The names a compiled module refers to, interned once at initialization.
// Symbols are perennial, so the pointers stay valid for the whole process and
// need no rooting.
struct ModuleNames {
    const ProtoString* name;
    const ProtoString* balance;
    const ProtoString* year;     // "año": 4 UTF-8 bytes, an inline string

    explicit ModuleNames(ProtoContext* ctx)
        : name(ProtoString::createSymbol(ctx, "name")),
          balance(ProtoString::createSymbol(ctx, "balance")),
          year(ProtoString::createSymbol(ctx, "año")) {}
};

const ProtoString* concat(ProtoContext* ctx, const char* a, const char* b) {
    return ProtoString::fromUTF8(ctx, a)->appendLast(ctx, ProtoString::fromUTF8(ctx, b));
}

}  // namespace

int main() {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;
    const ModuleNames names(ctx);

    // account.name = "Ada"; account.balance = 100; account.año = 2026
    const ProtoObject* account = ctx->newObject(false)
        ->setAttribute(ctx, names.name, ctx->fromUTF8String("Ada"))
        ->setAttribute(ctx, names.balance, ctx->fromInteger(100))
        ->setAttribute(ctx, names.year, ctx->fromInteger(2026));

    // account.balance: a name from the source, already canonical.
    check(account->getOwnAttributeDirect(ctx, names.balance)->asLong(ctx) == 100,
          "a source name reads with the symbol interned at initialization");

    // account['bal' + 'ance']: a heap string built at run time is not the
    // symbol.  Intern it first; the lookup finds the symbol from the string
    // itself and allocates nothing.
    const ProtoString* computed = concat(ctx, "bal", "ance");
    check(!computed->isSymbol() && computed != names.balance,
          "a name built at run time is a plain heap string");
    const ProtoString* key = computed->intern(ctx);
    check(key == names.balance, "intern returns the symbol createSymbol returned");
    check(account->getOwnAttributeDirect(ctx, key)->asLong(ctx) == 100,
          "the interned run-time name reads the attribute");

    // intern of a canonical string is the same pointer: calling it on every
    // computed access costs one tag check when the name is already canonical.
    check(names.balance->intern(ctx) == names.balance, "intern of a symbol is the same pointer");
    check(key->intern(ctx) == key, "intern is idempotent");

    // Short spellings are inline strings, ASCII or not, however they are built:
    // createSymbol, fromUTF8 and concatenation give the same pointer.
    const ProtoString* yearComputed = concat(ctx, "a", "ño");
    check(isInlineString(reinterpret_cast<const ProtoObject*>(names.year)),
          "\"año\" (4 bytes) is an inline string");
    check(yearComputed == names.year && yearComputed->intern(ctx) == names.year,
          "\"a\" + \"ño\" is already the canonical pointer");
    check(account->getOwnAttributeDirect(ctx, yearComputed->intern(ctx))->asLong(ctx) == 2026,
          "the short non-ASCII name reads the attribute");

    std::printf("01_attribute_names: %s\n", g_failures ? "FAILED" : "PASS");
    return g_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
