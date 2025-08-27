#include <gtest/gtest.h>
#include "../headers/proto.h"

// Test fixture for primitive type tests
class PrimitivesTest : public ::testing::Test {
protected:
    proto::ProtoSpace* space;
    proto::ProtoContext* context;

    void SetUp() override {
        // The main function for the space is not relevant for these tests
        space = new proto::ProtoSpace(nullptr, 0, nullptr);
        context = new proto::ProtoContext(nullptr, nullptr, 0, nullptr, space);
    }

    void TearDown() override {
        delete context;
        delete space;
    }
};

TEST_F(PrimitivesTest, IntegerHandling) {
    proto::ProtoObject* i = context->fromInteger(42);
    ASSERT_TRUE(i->isInteger(context));
    ASSERT_EQ(i->asInteger(context), 42);

    proto::ProtoObject* neg_i = context->fromInteger(-100);
    ASSERT_TRUE(neg_i->isInteger(context));
    ASSERT_EQ(neg_i->asInteger(context), -100);
}

TEST_F(PrimitivesTest, BooleanHandling) {
    proto::ProtoObject* t = context->fromBoolean(true);
    ASSERT_TRUE(t->isBoolean(context));
    ASSERT_EQ(t->asBoolean(context), true);
    ASSERT_EQ(t, PROTO_TRUE);

    proto::ProtoObject* f = context->fromBoolean(false);
    ASSERT_TRUE(f->isBoolean(context));
    ASSERT_EQ(f->asBoolean(context), false);
    ASSERT_EQ(f, PROTO_FALSE);
}

TEST_F(PrimitivesTest, NoneHandling) {
    proto::ProtoObject* n = PROTO_NONE;
    // Note: There isn't an isNone() method, as checks are usually done by pointer comparison
    ASSERT_EQ(n, nullptr);
}

TEST_F(PrimitivesTest, StringHandling) {
    const char* test_str = "hello, world!";
    proto::ProtoString* s = context->fromUTF8String(test_str);
    
    ASSERT_FALSE(s->isInteger(context));
    ASSERT_TRUE(s->asString(context) != nullptr);
    ASSERT_EQ(s->getSize(context), strlen(test_str));

    // Test that two identical strings are the same object (interning)
    proto::ProtoString* s2 = context->fromUTF8String(test_str);
    ASSERT_EQ(s, s2);
}
