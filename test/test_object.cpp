#include <gtest/gtest.h>
#include "../headers/protoCore.h"

using namespace proto;

class ObjectTest : public ::testing::Test {
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

TEST_F(ObjectTest, Creation) {
    const proto::ProtoObject* obj = context->newObject();
    ASSERT_NE(obj, nullptr);
}

TEST_F(ObjectTest, SetAndGetAttribute) {
    const proto::ProtoObject* obj = context->newObject(true); // Mutable object
    const proto::ProtoString* attr_name = context->fromUTF8String("my_attr")->asString(context);
    const proto::ProtoObject* attr_value = context->fromInteger(123);

    obj->setAttribute(context, const_cast<ProtoString*>(attr_name), attr_value);

    const proto::ProtoObject* retrieved_value = obj->getAttribute(context, const_cast<ProtoString*>(attr_name));
    ASSERT_EQ(retrieved_value->asLong(context), 123);
}

TEST_F(ObjectTest, GetMissingAttribute) {
    const proto::ProtoObject* obj = context->newObject();
    const proto::ProtoString* attr_name = context->fromUTF8String("non_existent_attr")->asString(context);

    const proto::ProtoObject* retrieved_value = obj->getAttribute(context, const_cast<ProtoString*>(attr_name));
    ASSERT_EQ(retrieved_value, PROTO_NONE);
}

TEST_F(ObjectTest, SimpleInheritance) {
    proto::ProtoObject* parent = const_cast<proto::ProtoObject*>(context->newObject(true));
    const proto::ProtoString* parent_attr = context->fromUTF8String("parent_attr")->asString(context);
    parent->setAttribute(context, const_cast<ProtoString*>(parent_attr), context->fromInteger(42));

    const proto::ProtoObject* child = parent->newChild(context, true);

    const proto::ProtoObject* retrieved_value = child->getAttribute(context, const_cast<ProtoString*>(parent_attr));
    ASSERT_NE(retrieved_value, PROTO_NONE);
    ASSERT_EQ(retrieved_value->asLong(context), 42);
}

TEST_F(ObjectTest, AttributeOverriding) {
    proto::ProtoObject* parent = const_cast<proto::ProtoObject*>(context->newObject(true));
    const proto::ProtoString* attr_name = context->fromUTF8String("my_attr")->asString(context);
    parent->setAttribute(context, const_cast<ProtoString*>(attr_name), context->fromInteger(100));

    const proto::ProtoObject* child = parent->newChild(context, true);
    child->setAttribute(context, const_cast<ProtoString*>(attr_name), context->fromInteger(200));

    ASSERT_EQ(parent->getAttribute(context, const_cast<ProtoString*>(attr_name))->asLong(context), 100);
    ASSERT_EQ(child->getAttribute(context, const_cast<ProtoString*>(attr_name))->asLong(context), 200);
}

TEST_F(ObjectTest, HasAttribute) {
    proto::ProtoObject* parent = const_cast<proto::ProtoObject*>(context->newObject(true));
    const proto::ProtoString* parent_attr = context->fromUTF8String("parent_attr")->asString(context);
    parent->setAttribute(context, const_cast<ProtoString*>(parent_attr), context->fromInteger(1));

    const proto::ProtoObject* child = parent->newChild(context, true);
    const proto::ProtoString* child_attr = context->fromUTF8String("child_attr")->asString(context);
    child->setAttribute(context, const_cast<ProtoString*>(child_attr), context->fromInteger(2));

    // Corrected: No const_cast needed as hasAttribute now accepts const ProtoString*
    ASSERT_TRUE(child->hasAttribute(context, parent_attr)->asBoolean(context));
    ASSERT_TRUE(child->hasAttribute(context, child_attr)->asBoolean(context));
    ASSERT_FALSE(child->hasAttribute(context, context->fromUTF8String("no_such_attr")->asString(context))->asBoolean(context));
}
