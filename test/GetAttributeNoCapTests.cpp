// GetAttributeNoCapTests.cpp — ProtoObject::getAttribute, depth cap removed.
//
// getAttribute used a `iterationCount > 500` cap in its chain-navigation
// loop and returned PROTO_NONE ("not found") once exceeded -- a false
// negative for any attribute living more than 500 own-chain entries away
// from the receiver, the same class of bug isInstanceOf's old 50-step cap
// and hasAttribute's old 50-step cap both had (and were fixed for, in
// earlier rounds). The cap is now gone.
//
// Termination without a step cap is safe because this loop -- like every
// chain-lookup method -- is a single-level walk of ONE receiver's own,
// already-built ParentLinkImplementation list: built once, forward only,
// by newChild/addParent/setParents, and never mutated afterward, so its
// length is fixed and finite the moment it is built. The walk never
// follows a visited link's object into THAT object's own separate chain,
// so it is unaffected by whatever any OTHER object's chain references --
// including a case where several separate setParents calls end up
// pointing chains at each other in a way that looks cyclic in the
// abstract (see test/SetParentsFlattenTests.cpp's
// ThreeObjectCycleIsNotDetectedButCausesNoHarm). So the loop always
// terminates: it walks a strictly finite list once, forward only.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"

using namespace proto;

class GetAttributeNoCapTest : public ::testing::Test {
protected:
    proto::ProtoSpace* space = nullptr;
    proto::ProtoContext* context = nullptr;

    void SetUp() override {
        space = new proto::ProtoSpace();
        context = space->rootContext;
    }
    void TearDown() override {
        delete space;
    }

    const ProtoString* sym(const char* s) { return ProtoString::createSymbol(context, s); }

    // Builds a chain of `depth` newChild levels; the ROOT object carries
    // `attr`. Returns the object at the far (leaf) end.
    const ProtoObject* buildChainWithAttrAtRoot(int depth, const ProtoString* attr) {
        const ProtoObject* root = context->newObject(false);
        root = root->setAttribute(context, attr, context->fromInteger(1));
        const ProtoObject* current = root;
        for (int i = 0; i < depth; ++i) {
            current = current->newChild(context);
        }
        return current;
    }
};

// --- The fix: no more false negative past 500 levels -----------------------

TEST_F(GetAttributeNoCapTest, AttributeFoundAtDepth501) {
    const ProtoString* a = sym("a");
    const ProtoObject* leaf = buildChainWithAttrAtRoot(501, a);

    const unsigned long before = context->allocatedCellsCount;
    const ProtoObject* result = leaf->getAttribute(context, a);
    EXPECT_EQ(context->allocatedCellsCount, before) << "getAttribute must not allocate";
    EXPECT_EQ(result, context->fromInteger(1))
        << "depth 501 exceeds the old 500-step cap (which used to return "
           "PROTO_NONE here); the cap is gone, this must now be found";
}

TEST_F(GetAttributeNoCapTest, AttributeFoundAtDepth2000) {
    const ProtoString* a = sym("a");
    const ProtoObject* leaf = buildChainWithAttrAtRoot(2000, a);

    const ProtoObject* result = leaf->getAttribute(context, a);
    EXPECT_EQ(result, context->fromInteger(1));
}

// --- Agreement: the documented divergence is closed ------------------------

TEST_F(GetAttributeNoCapTest, AgreesWithHasAttributeIsInstanceOfAndGetAttributesOnADeepChain) {
    const ProtoString* a = sym("a");
    const ProtoObject* root = context->newObject(false);
    root = root->setAttribute(context, a, context->fromInteger(42));
    const ProtoObject* current = root;
    for (int i = 0; i < 900; ++i) {
        current = current->newChild(context);
    }

    EXPECT_EQ(current->getAttribute(context, a), context->fromInteger(42));
    EXPECT_EQ(current->hasAttribute(context, a), PROTO_TRUE);
    EXPECT_EQ(current->isInstanceOf(context, root), PROTO_TRUE);

    const ProtoSparseList* attrs = current->getAttributes(context);
    EXPECT_EQ(attrs->getAt(context, reinterpret_cast<uintptr_t>(a)), context->fromInteger(42));

    // All four must now agree: the attribute (and the deep ancestor) are
    // found, with no exception left for "past the cap".
    EXPECT_EQ(current->getAttribute(context, a) != PROTO_NONE, current->hasAttribute(context, a) == PROTO_TRUE);
    EXPECT_EQ(current->getAttribute(context, a) != PROTO_NONE, attrs->has(context, reinterpret_cast<uintptr_t>(a)));
}

// --- Not-found on a deep chain still terminates and answers PROTO_NONE ----

TEST_F(GetAttributeNoCapTest, NotFoundOnADeepChainStillTerminatesAsNone) {
    const ProtoString* missing = sym("missing");
    const ProtoObject* root = context->newObject(false);
    const ProtoObject* current = root;
    for (int i = 0; i < 2000; ++i) {
        current = current->newChild(context);
    }

    const ProtoObject* result = current->getAttribute(context, missing);
    EXPECT_EQ(result, PROTO_NONE);
    EXPECT_EQ(current->hasAttribute(context, missing), PROTO_FALSE);
}

// --- Baseline: shallow lookups unaffected -----------------------------------

TEST_F(GetAttributeNoCapTest, ShallowOwnAndInheritedLookupsUnaffected) {
    const ProtoString* a = sym("a");
    const ProtoObject* base = context->newObject(false);
    base = base->setAttribute(context, a, context->fromInteger(7));
    const ProtoObject* child = base->newChild(context);

    EXPECT_EQ(child->getAttribute(context, a), context->fromInteger(7));
    EXPECT_EQ(base->getAttribute(context, a), context->fromInteger(7));

    const ProtoString* missing = sym("missing");
    EXPECT_EQ(child->getAttribute(context, missing), PROTO_NONE);
}
