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

// ===== StringAVLTest ========================================================
// Tests for the new AVL-based string implementation.
// These tests use internal classes directly via proto_internal.h.

class StringAVLTest : public ::testing::Test {
protected:
    ProtoSpace space;
    ProtoContext ctx{&space};
    ProtoContext* c = &ctx;
};

TEST_F(StringAVLTest, LeafNodeASCII) {
    const uint8_t bytes[] = {'h','e','l','l','o'};
    auto* leaf = new(c) proto::StringLeafNode(c, bytes, 5, 5, false);
    EXPECT_EQ(leaf->byte_count, 5);
    EXPECT_EQ(leaf->char_count, 5);
    EXPECT_FALSE(leaf->isPartial());
    EXPECT_EQ(leaf->codepointAt(0), (uint32_t)'h');
    EXPECT_EQ(leaf->codepointAt(leaf->charToByteOffset(4)), (uint32_t)'o');
}

TEST_F(StringAVLTest, LeafNodeUTF8TwoByte) {
    // U+00E9 (é) = 0xC3 0xA9
    const uint8_t bytes[] = {0xC3, 0xA9, 'x'};
    auto* leaf = new(c) proto::StringLeafNode(c, bytes, 3, 2, false);
    EXPECT_EQ(leaf->byte_count, 3);
    EXPECT_EQ(leaf->char_count, 2);
    EXPECT_EQ(leaf->codepointAt(0), 0x00E9u);  // é
    EXPECT_EQ(leaf->codepointAt(leaf->charToByteOffset(1)), (uint32_t)'x');
}

TEST_F(StringAVLTest, LeafNodePartialFlag) {
    const uint8_t bytes[] = {'a'};
    auto* leaf = new(c) proto::StringLeafNode(c, bytes, 1, 1, true);
    EXPECT_TRUE(leaf->isPartial());
}

TEST_F(StringAVLTest, LeafNodeHashDiffers) {
    const uint8_t b1[] = {'a','b'};
    const uint8_t b2[] = {'a','c'};
    auto* l1 = new(c) proto::StringLeafNode(c, b1, 2, 2);
    auto* l2 = new(c) proto::StringLeafNode(c, b2, 2, 2);
    EXPECT_NE(l1->content_hash, l2->content_hash);
}

TEST_F(StringAVLTest, InternalNodeCharsAndHash) {
    const uint8_t b1[] = {'a','b','c'};
    const uint8_t b2[] = {'d','e'};
    auto* l1 = new(c) proto::StringLeafNode(c, b1, 3, 3);
    auto* l2 = new(c) proto::StringLeafNode(c, b2, 2, 2);
    auto* node = new(c) proto::StringInternalNode(c, l1->asObject(), l2->asObject());

    EXPECT_EQ(node->total_chars, 5u);
    EXPECT_EQ(node->left_chars,  3u);
    EXPECT_EQ(node->total_bytes, 5u);
    EXPECT_EQ(node->height,      1u);

    // Hash must be deterministic: two nodes with the same children yield the same subtree_hash.
    auto* node2 = new(c) proto::StringInternalNode(c, l1->asObject(), l2->asObject());
    EXPECT_EQ(node->subtree_hash, node2->subtree_hash);
    // And it must differ from either leaf hash alone.
    EXPECT_NE(node->subtree_hash, l1->content_hash);
    EXPECT_NE(node->subtree_hash, l2->content_hash);
}

TEST_F(StringAVLTest, InternalNodeBalanceLeaves) {
    const uint8_t b[] = {'x'};
    auto* l = new(c) proto::StringLeafNode(c, b, 1, 1);
    auto* node = new(c) proto::StringInternalNode(c, l->asObject(), l->asObject());
    EXPECT_EQ(proto::StringInternalNode::nodeHeight(node->asObject()), 1);
    EXPECT_EQ(proto::StringInternalNode::balance(node->asObject()), 0);
}

// Forward declaration for strConcat (defined in namespace proto in ProtoString.cpp)
namespace proto {
    const ProtoObject* strConcat(ProtoContext* ctx,
                                  const ProtoObject* a,
                                  const ProtoObject* b);
}

TEST_F(StringAVLTest, ConcatTwoLeaves) {
    const uint8_t b1[] = {'a','b'};
    const uint8_t b2[] = {'c','d'};
    auto* l1 = new(c) proto::StringLeafNode(c, b1, 2, 2);
    auto* l2 = new(c) proto::StringLeafNode(c, b2, 2, 2);

    const ProtoObject* result = proto::strConcat(c, l1->asObject(), l2->asObject());
    EXPECT_EQ(proto::StringInternalNode::charCount(result), 4u);
    EXPECT_EQ(proto::StringInternalNode::nodeHeight(result), 1);
}

TEST_F(StringAVLTest, ConcatBalancesOnImbalance) {
    const uint8_t b[] = {'x'};
    auto* l = new(c) proto::StringLeafNode(c, b, 1, 1);
    const ProtoObject* acc = l->asObject();
    for (int i = 0; i < 7; ++i) {
        auto* li = new(c) proto::StringLeafNode(c, b, 1, 1);
        acc = proto::strConcat(c, acc, li->asObject());
    }
    // 8 leaves: balanced AVL height should be <= 3 (log2(8))
    EXPECT_LE(proto::StringInternalNode::nodeHeight(acc), 3);
}

TEST_F(StringAVLTest, ConcatNullLeft) {
    const uint8_t b[] = {'a'};
    auto* l = new(c) proto::StringLeafNode(c, b, 1, 1);
    EXPECT_EQ(proto::strConcat(c, nullptr, l->asObject()), l->asObject());
}

TEST_F(StringAVLTest, ConcatNullRight) {
    const uint8_t b[] = {'a'};
    auto* l = new(c) proto::StringLeafNode(c, b, 1, 1);
    EXPECT_EQ(proto::strConcat(c, l->asObject(), nullptr), l->asObject());
}

// Forward declarations for Task 5
namespace proto {
    struct SplitResult { const ProtoObject* left; const ProtoObject* right; };
    SplitResult strSplit(ProtoContext* ctx, const ProtoObject* node, uint32_t char_index);
    void strCharAt(const ProtoObject* node, uint32_t index, uint32_t* out_codepoint);
}

TEST_F(StringAVLTest, SplitLeafInMiddle) {
    const uint8_t b[] = {'h','e','l','l','o'};
    auto* leaf = new(c) proto::StringLeafNode(c, b, 5, 5);
    auto [left, right] = proto::strSplit(c, leaf->asObject(), 2);

    ASSERT_NE(left, nullptr);
    ASSERT_NE(right, nullptr);
    EXPECT_EQ(proto::StringInternalNode::charCount(left),  2u);
    EXPECT_EQ(proto::StringInternalNode::charCount(right), 3u);
}

TEST_F(StringAVLTest, SplitAtZero) {
    const uint8_t b[] = {'a','b'};
    auto* l = new(c) proto::StringLeafNode(c, b, 2, 2);
    auto [left, right] = proto::strSplit(c, l->asObject(), 0);
    EXPECT_EQ(left, nullptr);
    EXPECT_EQ(proto::StringInternalNode::charCount(right), 2u);
}

TEST_F(StringAVLTest, SplitAtEnd) {
    const uint8_t b[] = {'a','b'};
    auto* l = new(c) proto::StringLeafNode(c, b, 2, 2);
    auto [left, right] = proto::strSplit(c, l->asObject(), 2);
    EXPECT_EQ(proto::StringInternalNode::charCount(left), 2u);
    EXPECT_EQ(right, nullptr);
}

TEST_F(StringAVLTest, SplitTreeReconstitutesViaConcat) {
    const uint8_t b[] = {'a','b','c','d','e','f'};
    auto* leaf = new(c) proto::StringLeafNode(c, b, 6, 6);
    auto [left, right] = proto::strSplit(c, leaf->asObject(), 3);
    const ProtoObject* rejoined = proto::strConcat(c, left, right);
    EXPECT_EQ(proto::StringInternalNode::charCount(rejoined), 6u);

    // Verify content roundtrip via strCharAt (hash may differ due to tree structure)
    uint32_t cp;
    proto::strCharAt(rejoined, 0, &cp); EXPECT_EQ(cp, (uint32_t)'a');
    proto::strCharAt(rejoined, 2, &cp); EXPECT_EQ(cp, (uint32_t)'c');
    proto::strCharAt(rejoined, 3, &cp); EXPECT_EQ(cp, (uint32_t)'d');
    proto::strCharAt(rejoined, 5, &cp); EXPECT_EQ(cp, (uint32_t)'f');
}

TEST_F(StringAVLTest, CharAtInTree) {
    const uint8_t b1[] = {'a','b','c'};
    const uint8_t b2[] = {'d','e'};
    auto* l1 = new(c) proto::StringLeafNode(c, b1, 3, 3);
    auto* l2 = new(c) proto::StringLeafNode(c, b2, 2, 2);
    const ProtoObject* tree = proto::strConcat(c, l1->asObject(), l2->asObject());

    uint32_t cp;
    proto::strCharAt(tree, 0, &cp); EXPECT_EQ(cp, (uint32_t)'a');
    proto::strCharAt(tree, 2, &cp); EXPECT_EQ(cp, (uint32_t)'c');
    proto::strCharAt(tree, 3, &cp); EXPECT_EQ(cp, (uint32_t)'d');
    proto::strCharAt(tree, 4, &cp); EXPECT_EQ(cp, (uint32_t)'e');
}
