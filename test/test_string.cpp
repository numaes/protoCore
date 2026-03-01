#include <gtest/gtest.h>
#include "protoCore.h"
#include "proto_internal.h"

using namespace proto;

class StringTest : public ::testing::Test {
protected:
    ProtoSpace* space;
    ProtoContext* ctx;

    void SetUp() override {
        space = new ProtoSpace();
        ctx = new ProtoContext(space);
    }

    void TearDown() override {
        delete ctx;
        delete space;
    }
};

TEST_F(StringTest, InlineStringInvariant) {
    const ProtoObject* shortAscii = ctx->fromUTF8String("hello");
    ASSERT_NE(shortAscii, nullptr);
    ASSERT_TRUE(shortAscii->isString(ctx));
    
    // len <= 6 and all ascii = inline
    ProtoObjectPointer pa{}; pa.oid = shortAscii;
    ASSERT_EQ(pa.op.pointer_tag, POINTER_TAG_EMBEDDED_VALUE);
    ASSERT_EQ(pa.op.embedded_type, EMBEDDED_TYPE_INLINE_STRING);
}

TEST_F(StringTest, TupleStringInvariantLength) {
    const ProtoObject* longAscii = ctx->fromUTF8String("hello world");
    ASSERT_NE(longAscii, nullptr);
    ASSERT_TRUE(longAscii->isString(ctx));
    
    // len > 6 = tuple backed string
    ProtoObjectPointer pa{}; pa.oid = longAscii;
    ASSERT_EQ(pa.op.pointer_tag, POINTER_TAG_STRING);
}

TEST_F(StringTest, TupleStringInvariantNonAscii) {
    const ProtoObject* shortNonAscii = ctx->fromUTF8String("héllo"); // Note the accent
    ASSERT_NE(shortNonAscii, nullptr);
    ASSERT_TRUE(shortNonAscii->isString(ctx));
    
    // contains non-ascii = tuple backed string
    ProtoObjectPointer pa{}; pa.oid = shortNonAscii;
    ASSERT_EQ(pa.op.pointer_tag, POINTER_TAG_STRING);
}

TEST_F(StringTest, StringInterningTupleStrings) {
    const ProtoObject* str1 = ctx->fromUTF8String("this is a test string");
    const ProtoObject* str2 = ctx->fromUTF8String("this is a test string");
    
    // Both are tuple strings
    ProtoObjectPointer pa1{}; pa1.oid = str1;
    ProtoObjectPointer pa2{}; pa2.oid = str2;
    ASSERT_EQ(pa1.op.pointer_tag, POINTER_TAG_STRING);
    ASSERT_EQ(pa2.op.pointer_tag, POINTER_TAG_STRING);
    
    // Interning guarantees pointer equality for identical strings
    ASSERT_EQ(str1, str2);
}

TEST_F(StringTest, StringInterningInlineStrings) {
    const ProtoObject* str1 = ctx->fromUTF8String("hello");
    const ProtoObject* str2 = ctx->fromUTF8String("hello");
    
    // Both are inline strings
    ProtoObjectPointer pa1{}; pa1.oid = str1;
    ProtoObjectPointer pa2{}; pa2.oid = str2;
    ASSERT_EQ(pa1.op.pointer_tag, POINTER_TAG_EMBEDDED_VALUE);
    ASSERT_EQ(pa2.op.pointer_tag, POINTER_TAG_EMBEDDED_VALUE);
    
    // Inline strings encode data in the pointer, so equality is pointer equality
    ASSERT_EQ(str1, str2);
}

TEST_F(StringTest, StringConcatenationInvariant) {
    const ProtoString* a = ctx->fromUTF8String("123")->asString(ctx);
    const ProtoString* b = ctx->fromUTF8String("456")->asString(ctx);
    
    const ProtoString* ab = a->appendLast(ctx, b);
    
    // "123456" is 6 chars, all ascii -> MUST be inline under new invariants!
    ProtoObjectPointer pa{}; pa.oid = ab->asObject(ctx);
    EXPECT_EQ(pa.op.pointer_tag, POINTER_TAG_EMBEDDED_VALUE);
    EXPECT_EQ(pa.op.embedded_type, EMBEDDED_TYPE_INLINE_STRING);
}

TEST_F(StringTest, StringTupleInterningConcatenation) {
    const ProtoString* a = ctx->fromUTF8String("1234")->asString(ctx);
    const ProtoString* b = ctx->fromUTF8String("5678")->asString(ctx);
    const ProtoString* ab = a->appendLast(ctx, b);
    
    const ProtoObject* directString = ctx->fromUTF8String("12345678");
    
    // Must map to the exact same pointer through interning!
    EXPECT_EQ(ab->asObject(ctx), directString);
}
