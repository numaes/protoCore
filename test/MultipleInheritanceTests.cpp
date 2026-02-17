#include <gtest/gtest.h>
#include "../headers/protoCore.h"

using namespace proto;

class MultipleInheritanceTest : public ::testing::Test {
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
    
    const ProtoString* s(const char* str) {
        return context->fromUTF8String(str)->asString(context);
    }
};

/**
 * Test Diamond Inheritance:
 *   A
 *  / \
 * B   C
 *  \ /
 *   D
 * Linearized MRO for D should be: D, B, A, C
 * Note: Our addParent prepends. 
 * If we add B then C to D:
 * addParent(B) -> D chain becomes [B, A]
 * addParent(C) -> D checks C ancestors: [A]. A is already in D chain.
 *   So it just prepends C.
 *   D chain becomes [C, B, A]
 * Search order: D, C, B, A
 */
TEST_F(MultipleInheritanceTest, DiamondInheritance) {
    proto::ProtoObject* a = const_cast<proto::ProtoObject*>(context->newObject(true));
    proto::ProtoObject* b = const_cast<proto::ProtoObject*>(context->newObject(true));
    proto::ProtoObject* c = const_cast<proto::ProtoObject*>(context->newObject(true));
    proto::ProtoObject* d = const_cast<proto::ProtoObject*>(context->newObject(true));

    a->setAttribute(context, s("attr_a"), context->fromInteger(1));
    b->addParent(context, a);
    b->setAttribute(context, s("attr_b"), context->fromInteger(2));
    c->addParent(context, a);
    c->setAttribute(context, s("attr_c"), context->fromInteger(3));

    // D inherits from B and C
    d->addParent(context, b);
    d->addParent(context, c);

    // Verify all attributes are accessible
    ASSERT_EQ(d->getAttribute(context, s("attr_a"))->asLong(context), 1);
    ASSERT_EQ(d->getAttribute(context, s("attr_b"))->asLong(context), 2);
    ASSERT_EQ(d->getAttribute(context, s("attr_c"))->asLong(context), 3);
    
    // Check MRO order: if both B and C had the same attribute, C should win because it was added last (prepended)
    b->setAttribute(context, s("overlap"), context->fromInteger(20));
    c->setAttribute(context, s("overlap"), context->fromInteger(30));
    ASSERT_EQ(d->getAttribute(context, s("overlap"))->asLong(context), 30);
}

/**
 * Test Python-like order: class D(B, C)
 * In Python, D's MRO is D, B, ..., C, ...
 * Our addParent prepends. To get B before C, we must add C then B.
 */
TEST_F(MultipleInheritanceTest, PythonOrder) {
    proto::ProtoObject* b = const_cast<proto::ProtoObject*>(context->newObject(true));
    proto::ProtoObject* c = const_cast<proto::ProtoObject*>(context->newObject(true));
    proto::ProtoObject* d = const_cast<proto::ProtoObject*>(context->newObject(true));

    b->setAttribute(context, s("val"), context->fromInteger(10));
    c->setAttribute(context, s("val"), context->fromInteger(20));

    // To have B before C, we add them in reverse order
    d->addParent(context, c);
    d->addParent(context, b);

    ASSERT_EQ(d->getAttribute(context, s("val"))->asLong(context), 10);
}

/**
 * Test Linearization of deep ancestors
 */
TEST_F(MultipleInheritanceTest, DeepLinearization) {
    proto::ProtoObject* a1 = const_cast<proto::ProtoObject*>(context->newObject(true));
    proto::ProtoObject* a2 = const_cast<proto::ProtoObject*>(context->newObject(true));
    proto::ProtoObject* a3 = const_cast<proto::ProtoObject*>(context->newObject(true));

    a1->setAttribute(context, s("a1"), context->fromInteger(1));
    a2->addParent(context, a1);
    a2->setAttribute(context, s("a2"), context->fromInteger(2));
    a3->addParent(context, a2);
    a3->setAttribute(context, s("a3"), context->fromInteger(3));

    proto::ProtoObject* target = const_cast<proto::ProtoObject*>(context->newObject(true));
    target->addParent(context, a3);

    // Chain should be [a3, a2, a1]
    ASSERT_EQ(target->getAttribute(context, s("a1"))->asLong(context), 1);
    ASSERT_EQ(target->getAttribute(context, s("a2"))->asLong(context), 2);
    ASSERT_EQ(target->getAttribute(context, s("a3"))->asLong(context), 3);
}
