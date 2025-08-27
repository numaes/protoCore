#include <gtest/gtest.h>
#include "../headers/proto.h"

// Test fixture for ProtoObject tests
class ObjectTest : public ::testing::Test {
protected:
    proto::ProtoSpace* space;
    proto::ProtoContext* context;

    void SetUp() override {
        space = new proto::ProtoSpace(nullptr, 0, nullptr);
        context = new proto::ProtoContext(nullptr, nullptr, 0, nullptr, space);
    }

    void TearDown() override {
        delete context;
        delete space;
    }
};

TEST_F(ObjectTest, Creation) {
    proto::ProtoObject* obj = context->newObject();
    ASSERT_TRUE(obj != nullptr);
    // A new object should not be a primitive type
    ASSERT_FALSE(obj->isInteger(context));
    ASSERT_FALSE(obj->isBoolean(context));
}

TEST_F(ObjectTest, SetAndGetAttribute) {
    proto::ProtoObject* obj = context->newObject(true); // Mutable object
    proto::ProtoString* attr_name = context->fromUTF8String("my_attr");
    proto::ProtoObject* attr_value = context->fromInteger(123);

    obj->setAttribute(context, attr_name, attr_value);
    
    proto::ProtoObject* retrieved_value = obj->getAttribute(context, attr_name);
    ASSERT_EQ(retrieved_value, attr_value);
}

TEST_F(ObjectTest, GetMissingAttribute) {
    proto::ProtoObject* obj = context->newObject();
    proto::ProtoString* attr_name = context->fromUTF8String("non_existent_attr");

    proto::ProtoObject* retrieved_value = obj->getAttribute(context, attr_name);
    // Accessing a non-existent attribute should return PROTO_NONE (nullptr)
    ASSERT_EQ(retrieved_value, PROTO_NONE);
}

TEST_F(ObjectTest, SimpleInheritance) {
    proto::ProtoObject* parent = context->newObject(true);
    proto::ProtoString* parent_attr = context->fromUTF8String("parent_attr");
    parent->setAttribute(context, parent_attr, context->fromInteger(456));

    // Create a child object that inherits from parent
    proto::ProtoObject* child = parent->newChild(context, true);

    // The child should have access to the parent's attribute
    proto::ProtoObject* retrieved_value = child->getAttribute(context, parent_attr);
    ASSERT_TRUE(retrieved_value != nullptr);
    ASSERT_EQ(retrieved_value->asInteger(context), 456);
}

TEST_F(ObjectTest, AttributeOverriding) {
    proto::ProtoObject* parent = context->newObject(true);
    proto::ProtoString* attr_name = context->fromUTF8String("my_attr");
    parent->setAttribute(context, attr_name, context->fromInteger(100));

    proto::ProtoObject* child = parent->newChild(context, true);
    child->setAttribute(context, attr_name, context->fromInteger(200)); // Override the attribute

    // The child's own attribute should be found first
    ASSERT_EQ(child->getAttribute(context, attr_name)->asInteger(context), 200);
    // The parent's attribute should remain unchanged
    ASSERT_EQ(parent->getAttribute(context, attr_name)->asInteger(context), 100);
}

TEST_F(ObjectTest, HasAttribute) {
    proto::ProtoObject* parent = context->newObject(true);
    proto::ProtoString* parent_attr = context->fromUTF8String("parent_attr");
    parent->setAttribute(context, parent_attr, context->fromInteger(1));

    proto::ProtoObject* child = parent->newChild(context, true);
    proto::ProtoString* child_attr = context->fromUTF8String("child_attr");
    child->setAttribute(context, child_attr, context->fromInteger(2));

    ASSERT_TRUE(child->hasAttribute(context, child_attr)->asBoolean(context));
    ASSERT_TRUE(child->hasAttribute(context, parent_attr)->asBoolean(context));
    ASSERT_FALSE(child->hasAttribute(context, context->fromUTF8String("no_such_attr"))->asBoolean(context));

    ASSERT_TRUE(child->hasOwnAttribute(context, child_attr)->asBoolean(context));
    ASSERT_FALSE(child->hasOwnAttribute(context, parent_attr)->asBoolean(context));
}
