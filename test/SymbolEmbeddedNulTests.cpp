// SymbolEmbeddedNulTests.cpp — ProtoString::createSymbol(ctx, std::string)
// is length-aware.
//
// Before 3.5.1 the std::string overload forwarded s.c_str() to the
// zero-terminated overload, so a spelling with an embedded NUL byte was
// silently truncated at it: createSymbol(std::string("a\0b", 3)) returned the
// symbol "a".  Every step of the symbol path (wellFormedUTF8Prefix,
// createInlineStringUTF8, SymbolTable::internUTF8, buildStringFromUTF8Bytes)
// takes an explicit byte count, so U+0000 is an ordinary one-byte code point.
// These tests pin that down for inline (<= INLINE_STRING_MAX_BYTES bytes) and
// table (longer) spellings.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <string>

using namespace proto;

namespace {

std::string utf8Of(ProtoContext* ctx, const ProtoString* s) {
    std::string out;
    s->toUTF8String(ctx, out);
    return out;
}

}  // namespace

TEST(SymbolEmbeddedNul, ShortSpellingKeepsEveryByte) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;
    const std::string spelling("a\0b", 3);

    const ProtoString* sym = ProtoString::createSymbol(ctx, spelling);
    EXPECT_EQ(sym->getSize(ctx), 3u);
    EXPECT_EQ(utf8Of(ctx, sym), spelling);
    EXPECT_NE(sym, ProtoString::createSymbol(ctx, "a"));
    EXPECT_NE(sym, ProtoString::createSymbol(ctx, std::string("a")));
    EXPECT_EQ(sym, ProtoString::createSymbol(ctx, spelling));
    // The code point at index 1 is U+0000.
    EXPECT_EQ(sym->getAt(ctx, 1), ctx->fromUnicodeChar(0));
}

TEST(SymbolEmbeddedNul, LoneNulIsNotTheEmptySymbol) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;
    const std::string nul(1, '\0');

    const ProtoString* sym = ProtoString::createSymbol(ctx, nul);
    EXPECT_EQ(sym->getSize(ctx), 1u);
    EXPECT_NE(sym, ProtoString::createSymbol(ctx, ""));
    EXPECT_EQ(sym, ProtoString::createSymbol(ctx, nul));
}

TEST(SymbolEmbeddedNul, LongSpellingIsInternedWithEveryByte) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;
    // 24 bytes: beyond INLINE_STRING_MAX_BYTES, so it lives in the SymbolTable.
    const char raw[] = "attribute\0with\0two_nuls!";
    const std::string spelling(raw, sizeof(raw) - 1);
    ASSERT_GT(spelling.size(), static_cast<size_t>(INLINE_STRING_MAX_BYTES));

    const ProtoString* sym = ProtoString::createSymbol(ctx, spelling);
    EXPECT_TRUE(sym->isSymbol());
    EXPECT_EQ(sym->getSize(ctx), spelling.size());
    EXPECT_EQ(utf8Of(ctx, sym), spelling);
    EXPECT_EQ(sym, ProtoString::createSymbol(ctx, spelling));
    EXPECT_NE(sym, ProtoString::createSymbol(ctx, "attribute"));

    // Same prefix up to the NUL, different bytes after it: distinct symbols.
    const char rawOther[] = "attribute\0with\0two_nulz!";
    const std::string other(rawOther, sizeof(rawOther) - 1);
    EXPECT_NE(sym, ProtoString::createSymbol(ctx, other));
}

TEST(SymbolEmbeddedNul, SpellingWithoutNulMatchesTheCStringOverload) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;
    EXPECT_EQ(ProtoString::createSymbol(ctx, std::string("abc")),
              ProtoString::createSymbol(ctx, "abc"));
    EXPECT_EQ(ProtoString::createSymbol(ctx, std::string("a_rather_long_symbol_name")),
              ProtoString::createSymbol(ctx, "a_rather_long_symbol_name"));
}

TEST(SymbolEmbeddedNul, MalformedSpellingWithNulIsNormalisedNotTruncated) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;
    // A lead byte cut short by a NUL: the lead byte decodes as U+00C3 (the
    // historical tolerance of malformed input), the NUL stays a NUL.
    const std::string spelling("\xC3\0x", 3);
    const ProtoString* sym = ProtoString::createSymbol(ctx, spelling);
    EXPECT_EQ(sym->getSize(ctx), 3u);
    EXPECT_EQ(utf8Of(ctx, sym), std::string("\xC3\x83\0x", 4));
    EXPECT_EQ(sym, ProtoString::createSymbol(ctx, spelling));
}
