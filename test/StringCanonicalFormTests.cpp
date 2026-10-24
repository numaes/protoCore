// StringCanonicalFormTests.cpp — one canonical form per short spelling, and
// ProtoString::intern.
//
// Attribute keys are compared by pointer, so every spelling must have exactly
// one canonical pointer.  A spelling of at most INLINE_STRING_MAX_BYTES UTF-8
// bytes is canonical as an inline string, whatever its codepoints: every
// constructor must produce the inline form for it, and the symbol table must
// never hold it.  Longer spellings are canonical as a symbol of the table.
//
// Before 3.2.0 only wrapRoot (concatenation, slices) inlined non-ASCII short
// strings; createSymbol, fromUTF8 and create(list) inlined ASCII only, so
// "año" had two pointers depending on how it was built.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <string>

using namespace proto;

namespace {

const ProtoObject* obj(const ProtoString* s) {
    return reinterpret_cast<const ProtoObject*>(s);
}

}  // namespace

// "año" is 4 UTF-8 bytes: inline, whichever constructor builds it.
TEST(StringCanonicalForm, ShortNonAsciiSpellingIsInlineFromEveryConstructor) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    const ProtoString* viaFromUTF8 = ProtoString::fromUTF8(ctx, "año");
    const ProtoString* viaContext =
        reinterpret_cast<const ProtoString*>(ctx->fromUTF8String("año"));
    const ProtoString* viaSymbol = ProtoString::createSymbol(ctx, "año");
    const ProtoString* viaConcat = ProtoString::fromUTF8(ctx, "a")->appendLast(
        ctx, ProtoString::fromUTF8(ctx, "ño"));

    const ProtoList* chars = ctx->newList()
        ->appendLast(ctx, ctx->fromUnicodeChar('a'))
        ->appendLast(ctx, ctx->fromUnicodeChar(0xF1))   // ñ
        ->appendLast(ctx, ctx->fromUnicodeChar('o'));
    const ProtoString* viaList = ProtoString::create(ctx, chars);

    EXPECT_TRUE(isInlineString(obj(viaFromUTF8)));
    EXPECT_TRUE(isInlineString(obj(viaContext)));
    EXPECT_TRUE(isInlineString(obj(viaSymbol)));
    EXPECT_TRUE(isInlineString(obj(viaConcat)));
    EXPECT_TRUE(isInlineString(obj(viaList)));

    EXPECT_EQ(viaSymbol, viaFromUTF8);
    EXPECT_EQ(viaSymbol, viaContext);
    EXPECT_EQ(viaSymbol, viaConcat);
    EXPECT_EQ(viaSymbol, viaList);
}

// Six bytes is the inline limit; seven goes to the heap.
TEST(StringCanonicalForm, InlineLimitIsCountedInBytes) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;
    EXPECT_FALSE(isInlineString(obj(ProtoString::createSymbol(ctx, "ñañá"))));  // 8 bytes
    EXPECT_TRUE(isInlineString(obj(ProtoString::createSymbol(ctx, "ñaña"))));    // 6 bytes
    EXPECT_FALSE(isInlineString(obj(ProtoString::createSymbol(ctx, "ñañaa"))));  // 7 bytes
    EXPECT_TRUE(ProtoString::createSymbol(ctx, "ñañaa")->isSymbol());
}

// intern() returns the same pointer for a string that is already canonical.
TEST(StringCanonicalForm, InternOfACanonicalStringIsTheSamePointer) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    const ProtoString* inl = ProtoString::createSymbol(ctx, "año");
    EXPECT_EQ(inl->intern(ctx), inl);

    const ProtoString* sym = ProtoString::createSymbol(ctx, "canonical_symbol_name");
    ASSERT_TRUE(sym->isSymbol());
    EXPECT_EQ(sym->intern(ctx), sym);
}

// A heap string built at run time interns to the symbol createSymbol returns.
TEST(StringCanonicalForm, InternOfARuntimeStringFindsTheSymbol) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    const ProtoString* sym = ProtoString::createSymbol(ctx, "attribute_name_1");
    const ProtoString* built = ProtoString::fromUTF8(ctx, "attribute_name_")
        ->appendLast(ctx, ProtoString::fromUTF8(ctx, "1"));
    ASSERT_FALSE(built->isSymbol());
    ASSERT_NE(built, sym);

    EXPECT_EQ(built->intern(ctx), sym);
    EXPECT_TRUE(built->intern(ctx)->isSymbol());
}

// intern() of a spelling nobody interned yet creates the symbol, and
// createSymbol then returns that same pointer.
TEST(StringCanonicalForm, InternOfANewSpellingCreatesTheSymbol) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    const std::string spelling = "fresh_runtime_spelling_for_intern";
    const ProtoString* built = ProtoString::fromUTF8(ctx, "fresh_runtime_")
        ->appendLast(ctx, ProtoString::fromUTF8(ctx, "spelling_for_intern"));
    const ProtoString* sym = built->intern(ctx);
    ASSERT_TRUE(sym->isSymbol());
    EXPECT_EQ(ProtoString::createSymbol(ctx, spelling), sym);
}

// obj.a1 (a symbol made at module initialization) and obj['a' + '1'] (a
// string built at run time, interned before the access) reach one attribute.
TEST(StringCanonicalForm, AttributeSetBySymbolIsFoundByInternedRuntimeString) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    const ProtoString* key = ProtoString::createSymbol(ctx, "año_attribute");
    const ProtoObject* o = ctx->newObject(false)->setAttribute(
        ctx, key, ctx->fromInteger(42));

    const ProtoString* runtime = ProtoString::fromUTF8(ctx, "año_")
        ->appendLast(ctx, ProtoString::fromUTF8(ctx, "attribute"));
    const ProtoObject* v = o->getAttribute(ctx, runtime->intern(ctx));
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->asLong(ctx), 42);

    // The same for a short non-ASCII key, canonical as an inline string.
    const ProtoObject* o2 = ctx->newObject(false)->setAttribute(
        ctx, ProtoString::createSymbol(ctx, "año"), ctx->fromInteger(7));
    const ProtoString* shortRuntime = ProtoString::fromUTF8(ctx, "a")
        ->appendLast(ctx, ProtoString::fromUTF8(ctx, "ño"));
    const ProtoObject* v2 = o2->getAttribute(ctx, shortRuntime->intern(ctx));
    ASSERT_NE(v2, nullptr);
    EXPECT_EQ(v2->asLong(ctx), 7);
}
