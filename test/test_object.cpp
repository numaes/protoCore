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

TEST_F(ObjectTest, RemoveAttributeMutable) {
    proto::ProtoObject* obj = const_cast<proto::ProtoObject*>(context->newObject(true));
    const proto::ProtoString* a = context->fromUTF8String("a")->asString(context);
    const proto::ProtoString* b = context->fromUTF8String("b")->asString(context);
    obj->setAttribute(context, a, context->fromInteger(1));
    obj->setAttribute(context, b, context->fromInteger(2));

    // Mutable: removeAttribute mutates in place and returns the same handle.
    const proto::ProtoObject* result = obj->removeAttribute(context, a);
    ASSERT_EQ(result, obj);
    ASSERT_FALSE(obj->hasOwnAttribute(context, a)->asBoolean(context));
    ASSERT_TRUE(obj->hasOwnAttribute(context, b)->asBoolean(context));
    ASSERT_EQ(obj->getAttribute(context, b)->asLong(context), 2);
}

TEST_F(ObjectTest, RemoveAttributeImmutable) {
    const proto::ProtoObject* obj = context->newObject(false);
    const proto::ProtoString* a = context->fromUTF8String("a")->asString(context);
    const proto::ProtoString* b = context->fromUTF8String("b")->asString(context);
    obj = obj->setAttribute(context, a, context->fromInteger(1));
    obj = obj->setAttribute(context, b, context->fromInteger(2));

    // Immutable: returns a NEW object without `a`; original keeps `a`.
    const proto::ProtoObject* obj2 = obj->removeAttribute(context, a);
    ASSERT_NE(obj2, obj);
    ASSERT_TRUE(obj->hasOwnAttribute(context, a)->asBoolean(context));
    ASSERT_FALSE(obj2->hasOwnAttribute(context, a)->asBoolean(context));
    ASSERT_TRUE(obj2->hasOwnAttribute(context, b)->asBoolean(context));
    ASSERT_EQ(obj2->getAttribute(context, b)->asLong(context), 2);
}

TEST_F(ObjectTest, RemoveAttributeMissingIsNoOp) {
    proto::ProtoObject* obj = const_cast<proto::ProtoObject*>(context->newObject(true));
    const proto::ProtoString* missing = context->fromUTF8String("nope")->asString(context);

    // No-op: receiver returned unchanged, no exception.
    const proto::ProtoObject* result = obj->removeAttribute(context, missing);
    ASSERT_EQ(result, obj);
    ASSERT_FALSE(obj->hasOwnAttribute(context, missing)->asBoolean(context));
}

TEST_F(ObjectTest, RemoveAttributeOnlyAffectsOwn) {
    // Parent carries `x`; child overrides it and then removes its own copy.
    // The chain walk should fall back to the parent's value, NOT report
    // missing — del at the OWN level only.
    proto::ProtoObject* parent = const_cast<proto::ProtoObject*>(context->newObject(true));
    const proto::ProtoString* x = context->fromUTF8String("x")->asString(context);
    parent->setAttribute(context, x, context->fromInteger(100));

    proto::ProtoObject* child = const_cast<proto::ProtoObject*>(parent->newChild(context, true));
    child->setAttribute(context, x, context->fromInteger(200));

    ASSERT_EQ(child->getAttribute(context, x)->asLong(context), 200);
    child->removeAttribute(context, x);
    ASSERT_FALSE(child->hasOwnAttribute(context, x)->asBoolean(context));
    // Parent binding survives and is now what the chain walk surfaces.
    ASSERT_TRUE(child->hasAttribute(context, x)->asBoolean(context));
    ASSERT_EQ(child->getAttribute(context, x)->asLong(context), 100);
    ASSERT_TRUE(parent->hasOwnAttribute(context, x)->asBoolean(context));
}

TEST_F(ObjectTest, RemoveAttributePreservesNoneOverride) {
    // PROTO_NONE is a legitimate user value — assigning it must NOT delete
    // the attribute, so a None set in a child still shadows a parent value.
    // Only removeAttribute removes; setAttribute(... PROTO_NONE) keeps the
    // entry.
    proto::ProtoObject* parent = const_cast<proto::ProtoObject*>(context->newObject(true));
    const proto::ProtoString* x = context->fromUTF8String("x")->asString(context);
    parent->setAttribute(context, x, context->fromInteger(42));

    proto::ProtoObject* child = const_cast<proto::ProtoObject*>(parent->newChild(context, true));
    child->setAttribute(context, x, PROTO_NONE);

    // The child shadows parent's 42 with explicit None.
    ASSERT_TRUE(child->hasOwnAttribute(context, x)->asBoolean(context));
    ASSERT_EQ(child->getAttribute(context, x), PROTO_NONE);

    // Now remove explicitly — chain walk falls back to parent's 42.
    child->removeAttribute(context, x);
    ASSERT_FALSE(child->hasOwnAttribute(context, x)->asBoolean(context));
    ASSERT_EQ(child->getAttribute(context, x)->asLong(context), 42);
}
