#include <gtest/gtest.h>
#include "protoCore.h"
#include "proto_internal.h"

using namespace proto;

class StringTest : public ::testing::Test {
protected:
    ProtoSpace* space;
    ProtoContext* ctx;
    ProtoContext* c;  // Alias used by iterator and public-API style tests.

    void SetUp() override {
        space = new ProtoSpace();
        ctx = new ProtoContext(space);
        c = ctx;
    }

    void TearDown() override {
        delete ctx;
        delete space;
    }

    const ProtoString* str(const char* s) {
        return ProtoString::fromUTF8String(c, s);
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

TEST_F(StringAVLTest, StringImplFromUTF8) {
    const char* src = "hello";
    auto* s = proto::ProtoStringImplementation::fromUTF8Bytes(
        c, reinterpret_cast<const uint8_t*>(src), 5);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->implGetSize(), 5u);
}

TEST_F(StringAVLTest, StringImplHashIsRootHash) {
    const char* src = "abc";
    auto* s = proto::ProtoStringImplementation::fromUTF8Bytes(
        c, reinterpret_cast<const uint8_t*>(src), 3);
    EXPECT_EQ(s->implGetHash(),
              proto::StringInternalNode::subtreeHash(s->avl_root));
}

TEST_F(StringAVLTest, StringImplLargeFromUTF8) {
    std::string long_str(100, 'a');
    auto* s = proto::ProtoStringImplementation::fromUTF8Bytes(
        c, reinterpret_cast<const uint8_t*>(long_str.data()), long_str.size());
    EXPECT_EQ(s->implGetSize(), 100u);
    EXPECT_LE(proto::StringInternalNode::nodeHeight(s->avl_root), 2);
}

// ===== ProtoString Public API Tests (Task 7) =================================

class StringPublicAPITest : public ::testing::Test {
protected:
    ProtoSpace space;
    ProtoContext ctx{&space};
    ProtoContext* c = &ctx;

    const ProtoString* str(const char* s) {
        return ProtoString::fromUTF8String(c, s);
    }
};

TEST_F(StringPublicAPITest, GetSize) {
    EXPECT_EQ(str("hello")->getSize(c), 5u);
    EXPECT_EQ(str("")->getSize(c), 0u);
    EXPECT_EQ(str("hello world")->getSize(c), 11u);
}

TEST_F(StringPublicAPITest, GetAt) {
    auto* s = str("hello");
    auto* ch = s->getAt(c, 0);
    ASSERT_NE(ch, nullptr);
    ProtoObjectPointer pa{}; pa.oid = ch;
    EXPECT_EQ(pa.unicodeChar.unicodeValue, static_cast<unsigned long>('h'));
}

TEST_F(StringPublicAPITest, GetAtLastChar) {
    auto* s = str("hello");
    auto* ch = s->getAt(c, 4);
    ASSERT_NE(ch, nullptr);
    ProtoObjectPointer pa{}; pa.oid = ch;
    EXPECT_EQ(pa.unicodeChar.unicodeValue, static_cast<unsigned long>('o'));
}

TEST_F(StringPublicAPITest, AppendLast) {
    auto* s = str("hello")->appendLast(c, str(" world"));
    EXPECT_EQ(s->getSize(c), 11u);
    std::string out;
    s->toUTF8String(c, out);
    EXPECT_EQ(out, "hello world");
}

TEST_F(StringPublicAPITest, AppendFirst) {
    auto* s = str("world")->appendFirst(c, str("hello "));
    EXPECT_EQ(s->getSize(c), 11u);
    std::string out;
    s->toUTF8String(c, out);
    EXPECT_EQ(out, "hello world");
}

TEST_F(StringPublicAPITest, GetSlice) {
    auto* s = str("hello world")->getSlice(c, 6, 11);
    EXPECT_EQ(s->getSize(c), 5u);
    std::string out;
    s->toUTF8String(c, out);
    EXPECT_EQ(out, "world");
}

TEST_F(StringPublicAPITest, GetSliceFromBeginning) {
    auto* s = str("hello world")->getSlice(c, 0, 5);
    std::string out;
    s->toUTF8String(c, out);
    EXPECT_EQ(out, "hello");
}

TEST_F(StringPublicAPITest, RemoveAt) {
    auto* s = str("hello")->removeAt(c, 1);
    EXPECT_EQ(s->getSize(c), 4u);
    std::string out;
    s->toUTF8String(c, out);
    EXPECT_EQ(out, "hllo");
}

TEST_F(StringPublicAPITest, RemoveSlice) {
    auto* s = str("hello world")->removeSlice(c, 5, 6);
    std::string out;
    s->toUTF8String(c, out);
    EXPECT_EQ(out, "helloworld");
}

TEST_F(StringPublicAPITest, InsertAtString) {
    auto* s = str("hllo")->insertAtString(c, 1, str("e"));
    EXPECT_EQ(s->getSize(c), 5u);
    std::string out;
    s->toUTF8String(c, out);
    EXPECT_EQ(out, "hello");
}

TEST_F(StringPublicAPITest, InsertAt) {
    auto* s = str("hllo");
    // Create unicode char for 'e'
    auto* e_char = c->fromUnicodeChar('e');
    auto* result = s->insertAt(c, 1, e_char);
    EXPECT_EQ(result->getSize(c), 5u);
    std::string out;
    result->toUTF8String(c, out);
    EXPECT_EQ(out, "hello");
}

TEST_F(StringPublicAPITest, CmpToString) {
    EXPECT_EQ(str("abc")->cmp_to_string(c, str("abc")), 0);
    EXPECT_LT(str("abc")->cmp_to_string(c, str("abd")), 0);
    EXPECT_GT(str("abd")->cmp_to_string(c, str("abc")), 0);
}

TEST_F(StringPublicAPITest, CmpToStringDifferentLengths) {
    EXPECT_LT(str("abc")->cmp_to_string(c, str("abcd")), 0);
    EXPECT_GT(str("abcd")->cmp_to_string(c, str("abc")), 0);
}

TEST_F(StringPublicAPITest, ToUTF8RoundTrip) {
    std::string out;
    str("hello world")->toUTF8String(c, out);
    EXPECT_EQ(out, "hello world");
}

TEST_F(StringPublicAPITest, ToUTF8RoundTripShort) {
    std::string out;
    str("hi")->toUTF8String(c, out);
    EXPECT_EQ(out, "hi");
}

TEST_F(StringPublicAPITest, EmptyStringOperations) {
    auto* empty = str("");
    EXPECT_EQ(empty->getSize(c), 0u);
    auto* result = empty->appendLast(c, str("x"));
    EXPECT_EQ(result->getSize(c), 1u);
}

TEST_F(StringPublicAPITest, SplitFirst) {
    auto* s = str("hello")->splitFirst(c, 3);
    std::string out;
    s->toUTF8String(c, out);
    EXPECT_EQ(out, "hel");
}

TEST_F(StringPublicAPITest, SplitLast) {
    auto* s = str("hello")->splitLast(c, 3);
    std::string out;
    s->toUTF8String(c, out);
    EXPECT_EQ(out, "llo");
}

TEST_F(StringPublicAPITest, RemoveFirst) {
    auto* s = str("hello")->removeFirst(c, 2);
    std::string out;
    s->toUTF8String(c, out);
    EXPECT_EQ(out, "llo");
}

TEST_F(StringPublicAPITest, RemoveLast) {
    auto* s = str("hello")->removeLast(c, 2);
    std::string out;
    s->toUTF8String(c, out);
    EXPECT_EQ(out, "hel");
}

TEST_F(StringPublicAPITest, Multiply) {
    auto* count = c->fromInteger(3);
    auto* s = str("ab")->multiply(c, count);
    EXPECT_EQ(s->getSize(c), 6u);
    std::string out;
    s->toUTF8String(c, out);
    EXPECT_EQ(out, "ababab");
}

TEST_F(StringPublicAPITest, SetAt) {
    auto* x_char = c->fromUnicodeChar('x');
    auto* s = str("hello")->setAt(c, 1, x_char);
    std::string out;
    s->toUTF8String(c, out);
    EXPECT_EQ(out, "hxllo");
}

// ---- Iterator tests (Task 8) ------------------------------------------------

TEST_F(StringTest, IteratorForward) {
    auto* s = str("abc");
    // next() is mutable — it returns current value and advances the index.
    auto* it = const_cast<ProtoStringIterator*>(s->getIterator(c));
    ASSERT_TRUE(it->hasNext(c));
    auto* a = it->next(c);
    ASSERT_TRUE(it->hasNext(c));
    auto* b = it->next(c);
    ASSERT_TRUE(it->hasNext(c));
    auto* c_obj = it->next(c);
    EXPECT_FALSE(it->hasNext(c));
    // Verify codepoint values via the unicode char objects
    ProtoObjectPointer pa{}; pa.oid = a;
    EXPECT_EQ(pa.unicodeChar.unicodeValue & 0x1FFFFFu, (uint32_t)'a');
    pa.oid = b;
    EXPECT_EQ(pa.unicodeChar.unicodeValue & 0x1FFFFFu, (uint32_t)'b');
    pa.oid = c_obj;
    EXPECT_EQ(pa.unicodeChar.unicodeValue & 0x1FFFFFu, (uint32_t)'c');
}

TEST_F(StringTest, IteratorMultibyte) {
    // e-acute (U+00E9) + a-grave (U+00E0)
    auto* s = str("\xC3\xA9\xC3\xA0");
    auto* it = const_cast<ProtoStringIterator*>(s->getIterator(c));
    ASSERT_TRUE(it->hasNext(c));
    auto* ch1 = it->next(c);
    ASSERT_TRUE(it->hasNext(c));
    auto* ch2 = it->next(c);
    EXPECT_FALSE(it->hasNext(c));
    ProtoObjectPointer pa{}; pa.oid = ch1;
    EXPECT_EQ(pa.unicodeChar.unicodeValue & 0x1FFFFFu, 0x00E9u);
    pa.oid = ch2;
    EXPECT_EQ(pa.unicodeChar.unicodeValue & 0x1FFFFFu, 0x00E0u);
}

TEST_F(StringTest, IteratorLargeString) {
    std::string src(200, 'x');
    auto* s = ProtoString::fromUTF8String(c, src.c_str());
    auto* it = const_cast<ProtoStringIterator*>(s->getIterator(c));
    int count = 0;
    while (it->hasNext(c)) {
        it->next(c);
        ++count;
    }
    EXPECT_EQ(count, 200);
}

TEST_F(StringPublicAPITest, ToUTF8StringMultibyte) {
    // Verifies O(N) toUTF8String with multibyte codepoints (uses RopeCharacterIterator)
    auto* s = str("\xC3\xA9\xC3\xA0");  // e-acute + a-grave
    std::string out;
    s->toUTF8String(c, out);
    EXPECT_EQ(out, "\xC3\xA9\xC3\xA0");
}

TEST_F(StringPublicAPITest, AsListMultibyte) {
    auto* s = str("\xC3\xA9\xC3\xA0");  // e-acute + a-grave
    auto* list = s->asList(c);
    EXPECT_EQ(list->getSize(c), 2u);
    ProtoObjectPointer pa{}; pa.oid = list->getAt(c, 0);
    EXPECT_EQ(pa.unicodeChar.unicodeValue & 0x1FFFFFu, 0x00E9u);
    pa.oid = list->getAt(c, 1);
    EXPECT_EQ(pa.unicodeChar.unicodeValue & 0x1FFFFFu, 0x00E0u);
}

TEST_F(StringPublicAPITest, CmpToStringConsistency) {
    auto* a = str("abc");
    auto* b = str("abd");
    EXPECT_LT(a->cmp_to_string(c, b), 0);
    EXPECT_GT(b->cmp_to_string(c, a), 0);
    EXPECT_EQ(a->cmp_to_string(c, a), 0);
}

// ---- Task 9: createInlineStringUTF8 tests -----------------------------------

TEST_F(StringTest, InlineStringASCII) {
    auto* s = ProtoString::fromUTF8String(c, "hi");
    auto* obj = reinterpret_cast<const ProtoObject*>(s);
    EXPECT_TRUE(isInlineString(obj));
    std::string out;
    s->toUTF8String(c, out);
    EXPECT_EQ(out, "hi");
}

TEST_F(StringTest, InlineStringMaxSixBytes) {
    auto* s = ProtoString::fromUTF8String(c, "abcdef");
    auto* obj = reinterpret_cast<const ProtoObject*>(s);
    EXPECT_TRUE(isInlineString(obj));
}

TEST_F(StringTest, SevenBytesNotInline) {
    auto* s = ProtoString::fromUTF8String(c, "abcdefg");
    auto* obj = reinterpret_cast<const ProtoObject*>(s);
    EXPECT_FALSE(isInlineString(obj));
}

TEST_F(StringTest, InlineStringTwoByte) {
    // é = U+00E9 = 0xC3 0xA9 (2 UTF-8 bytes, 1 codepoint)
    const uint8_t bytes[] = {0xC3, 0xA9};
    auto* obj = createInlineStringUTF8(c, bytes, 2);
    EXPECT_TRUE(isInlineString(obj));
    auto* s = reinterpret_cast<const ProtoString*>(obj);
    EXPECT_EQ(s->getSize(c), 1u);
}

// ---- Task 10: SymbolTable tests ---------------------------------------------

class SymbolTest : public ::testing::Test {
protected:
    proto::ProtoSpace space;
    proto::ProtoContext ctx{&space};
    ProtoContext* c = &ctx;
};

TEST_F(SymbolTest, SameSymbolSamePointer) {
    // Two calls with identical content must return pointer-equal results.
    auto* s1 = ProtoString::createSymbol(c, "hello world!!");
    auto* s2 = ProtoString::createSymbol(c, "hello world!!");
    EXPECT_EQ(s1, s2);
}

TEST_F(SymbolTest, DifferentSymbolsDifferentPointers) {
    auto* s1 = ProtoString::createSymbol(c, "hello world!!");
    auto* s2 = ProtoString::createSymbol(c, "world hello!!");
    EXPECT_NE(s1, s2);
}

TEST_F(SymbolTest, SymbolEqualsStringByContent) {
    // A symbol and a non-interned string with the same content must compare equal.
    auto* sym = ProtoString::createSymbol(c, "hello world!!");
    auto* str = ProtoString::fromUTF8String(c, "hello world!!");
    EXPECT_EQ(sym->cmp_to_string(c, str), 0);
}

TEST_F(SymbolTest, NonInternedStringNotSymbol) {
    // A regular (non-interned) string longer than 6 bytes must not be a symbol.
    auto* str = ProtoString::fromUTF8String(c, "hello world!!");
    auto* obj = reinterpret_cast<const ProtoObject*>(str);
    EXPECT_FALSE(SymbolTable::isSymbol(obj));
}

TEST_F(SymbolTest, SymbolIsSymbol) {
    auto* sym = ProtoString::createSymbol(c, "hello world!!");
    auto* obj = reinterpret_cast<const ProtoObject*>(sym);
    EXPECT_TRUE(SymbolTable::isSymbol(obj));
}

TEST_F(SymbolTest, AutoInternOnSetAttribute) {
    // "myLongKey" is 9 bytes — exceeds INLINE_STRING_MAX_BYTES (6), so it goes
    // through ProtoStringImplementation (heap allocation, POINTER_TAG_STRING).
    // After setAttribute auto-interns it, a lookup with the matching symbol
    // (POINTER_TAG_SYMBOL) must find the stored value.
    auto* obj = c->newObject();
    auto* non_interned_key = ProtoString::fromUTF8String(c, "myLongKey");

    auto* val = c->space->objectPrototype;
    auto* obj2 = obj->setAttribute(c, non_interned_key, val);

    // Retrieve with a freshly-created symbol that has identical content.
    auto* sym_key = ProtoString::createSymbol(c, "myLongKey");
    auto* retrieved = obj2->getAttribute(c, sym_key);
    EXPECT_EQ(retrieved, val);
}
