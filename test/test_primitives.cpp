#include <gtest/gtest.h>
#include "../headers/protoCore.h"

using namespace proto;

class PrimitivesTest : public ::testing::Test {
protected:
    proto::ProtoSpace* space;
    proto::ProtoContext* context;

    void SetUp() override {
        space = new proto::ProtoSpace();
        context = space->rootContext;
    }

    void TearDown() override {
        delete space;
    }
};

TEST_F(PrimitivesTest, IntegerHandling) {
    const proto::ProtoObject* i = context->fromInteger(42);
    ASSERT_TRUE(i->isInteger(context));
    ASSERT_EQ(i->asLong(context), 42);

    const proto::ProtoObject* neg_i = context->fromInteger(-100);
    ASSERT_TRUE(neg_i->isInteger(context));
    ASSERT_EQ(neg_i->asLong(context), -100);
}

TEST_F(PrimitivesTest, BooleanHandling) {
    const proto::ProtoObject* t = context->fromBoolean(true);
    ASSERT_TRUE(t->isBoolean(context));
    ASSERT_TRUE(t->asBoolean(context));

    const proto::ProtoObject* f = context->fromBoolean(false);
    ASSERT_TRUE(f->isBoolean(context));
    ASSERT_FALSE(f->asBoolean(context));
}

TEST_F(PrimitivesTest, NoneHandling) {
    const proto::ProtoObject* n = PROTO_NONE;
    ASSERT_TRUE(n->isNone(context));
    ASSERT_FALSE(n->isInteger(context));
}

TEST_F(PrimitivesTest, StringHandling) {
    const char* test_str = "hello";
    const proto::ProtoObject* s_obj = context->fromUTF8String(test_str);
    
    ASSERT_TRUE(s_obj->isString(context));
    ASSERT_FALSE(s_obj->isInteger(context));
    
    const proto::ProtoString* s = s_obj->asString(context);
    ASSERT_TRUE(s != nullptr);
    ASSERT_EQ(s->getSize(context), 5);

    const proto::ProtoObject* s2_obj = context->fromUTF8String(test_str);
    const proto::ProtoString* s2 = s2_obj->asString(context);
    ASSERT_EQ(s, s2); // Should be the same due to interning
}
