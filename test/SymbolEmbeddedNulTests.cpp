// SymbolEmbeddedNulTests.cpp — every std::string-taking text API of
// protoCore is length-aware: ProtoString::createSymbol(ctx, std::string),
// ProtoString::fromStdString, ProtoContext::fromUTF8String(std::string),
// ProtoString::fromCodepointTuple and ProtoContext::raiseError(kind,
// std::string) with the default and the length-aware error factory.
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
#include <vector>

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

// Trailing NUL bytes in the inline form: "ab", "ab\0" and "ab\0\0\0\0" are
// three spellings, and so are 7-byte table spellings with trailing NULs.
TEST(SymbolEmbeddedNul, TrailingNulBytesAreDistinctSpellings) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;
    const ProtoString* ab = ProtoString::createSymbol(ctx, std::string("ab"));
    const ProtoString* ab0 = ProtoString::createSymbol(ctx, std::string("ab\0", 3));
    const ProtoString* ab0000 = ProtoString::createSymbol(ctx, std::string("ab\0\0\0\0", 6));
    EXPECT_NE(ab, ab0);
    EXPECT_NE(ab0, ab0000);
    EXPECT_EQ(ab0->getSize(ctx), 3u);
    EXPECT_EQ(ab0000->getSize(ctx), 6u);
    EXPECT_NE(ab->getHash(ctx), ab0->getHash(ctx));
    EXPECT_EQ(ab0->cmp_to_string(ctx, ProtoString::createSymbol(ctx, std::string("ab\0", 3))), 0);
    EXPECT_NE(ab0->cmp_to_string(ctx, ab), 0);

    const std::string seven("ab\0\0\0\0\0", 7);
    const ProtoString* t7 = ProtoString::createSymbol(ctx, seven);
    EXPECT_TRUE(t7->isSymbol());
    EXPECT_EQ(t7->getSize(ctx), 7u);
    EXPECT_EQ(t7, ProtoString::createSymbol(ctx, seven));
    EXPECT_NE(t7, ProtoString::createSymbol(ctx, std::string("ab\0\0\0\0\0\0", 8)));
}

TEST(TextEmbeddedNul, FromStdStringKeepsEveryByte) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;
    for (const std::string& text : {std::string("a\0b", 3),
                                    std::string("\0", 1),
                                    std::string("a long text\0with a NUL in it", 28)}) {
        const ProtoString* s = ProtoString::fromStdString(ctx, text);
        ASSERT_NE(s, nullptr);
        EXPECT_EQ(s->getSize(ctx), text.size());
        EXPECT_EQ(utf8Of(ctx, s), text);
    }
    // No NUL: the same canonical inline string as fromUTF8.
    EXPECT_EQ(ProtoString::fromStdString(ctx, std::string("abc")), ProtoString::fromUTF8(ctx, "abc"));
}

TEST(TextEmbeddedNul, ContextFromUTF8StdStringKeepsEveryByte) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;
    const std::string text("key\0value", 9);
    const ProtoObject* o = ctx->fromUTF8String(text);
    ASSERT_TRUE(o->isString(ctx));
    EXPECT_EQ(o->asString(ctx)->getSize(ctx), 9u);
    EXPECT_EQ(utf8Of(ctx, o->asString(ctx)), text);
    // The zero-terminated overload is unchanged: it stops at the NUL.
    EXPECT_EQ(ctx->fromUTF8String(text.c_str())->asString(ctx)->getSize(ctx), 3u);
}

TEST(TextEmbeddedNul, CodepointTupleKeepsU0000) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;
    std::vector<const ProtoObject*> cps = {ctx->fromUnicodeChar('a'), ctx->fromUnicodeChar(0),
                                           ctx->fromUnicodeChar('b')};
    const ProtoString* s = ProtoString::fromCodepointTuple(ctx, ctx->newTuple(cps));
    EXPECT_EQ(s->getSize(ctx), 3u);
    EXPECT_EQ(utf8Of(ctx, s), std::string("a\0b", 3));
}

namespace {

std::string messageOf(ProtoContext* ctx, const ProtoObject* error) {
    const ProtoObject* m = error->getAttribute(ctx, ProtoString::createSymbol(ctx, "message"));
    return m && m->isString(ctx) ? utf8Of(ctx, m->asString(ctx)) : std::string("<none>");
}

std::string g_seenMessage;
proto_ulong g_seenLength = 0;

const ProtoObject* lengthFactory(ProtoContext* context, ProtoErrorKind kind,
                                 const char* message, proto_ulong length) {
    g_seenMessage.assign(message, length);
    g_seenLength = length;
    return context->space->getErrorPrototype(context, kind)->newChild(context);
}

const ProtoObject* legacyFactory(ProtoContext* context, ProtoErrorKind kind, const char* message) {
    g_seenMessage = message;
    g_seenLength = g_seenMessage.size();
    return context->space->getErrorPrototype(context, kind)->newChild(context);
}

}  // namespace

TEST(TextEmbeddedNul, RaiseErrorDefaultFactoryKeepsEveryByte) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;
    const std::string message("bad\0value", 9);
    try {
        ctx->raiseError(ProtoErrorKind::InvalidArgument, message);
        FAIL() << "raiseError returned";
    } catch (const ProtoException& e) {
        EXPECT_EQ(messageOf(ctx, e.value()), message);
    }
    // The zero-terminated overload still works as before.
    try {
        ctx->raiseError(ProtoErrorKind::InvalidArgument, "plain");
        FAIL() << "raiseError returned";
    } catch (const ProtoException& e) {
        EXPECT_EQ(messageOf(ctx, e.value()), "plain");
    }
}

TEST(TextEmbeddedNul, LengthAwareFactoryReceivesTheWholeMessage) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;
    const std::string message("bad\0value", 9);

    space.setErrorFactoryWithLength(lengthFactory);
    EXPECT_EQ(space.getErrorFactoryWithLength(), lengthFactory);
    EXPECT_THROW(ctx->raiseError(ProtoErrorKind::Overflow, message), ProtoException);
    EXPECT_EQ(g_seenLength, 9u);
    EXPECT_EQ(g_seenMessage, message);

    // A length-aware factory takes precedence over a zero-terminated one.
    space.setErrorFactory(legacyFactory);
    EXPECT_THROW(ctx->raiseError(ProtoErrorKind::Overflow, message), ProtoException);
    EXPECT_EQ(g_seenLength, 9u);

    // Uninstalled: the zero-terminated factory sees the message up to its NUL,
    // as its signature implies.
    space.setErrorFactoryWithLength(nullptr);
    EXPECT_THROW(ctx->raiseError(ProtoErrorKind::Overflow, message), ProtoException);
    EXPECT_EQ(g_seenMessage, "bad");
    space.setErrorFactory(nullptr);
}
