// HasAttributeChainTests.cpp — ProtoObject::hasAttribute chain walk.
//
// hasAttribute used a depth-first walk with a fixed-size (64-slot) sibling
// stack and an arbitrary 50-step cap, returning PROTO_FALSE (a false
// negative) for any hierarchy deeper than 50 links — the exact same class
// of bug ProtoObject::isInstanceOf had before it was fixed to a pure
// linear scan of the receiver's own (always-flattened, by the newChild/
// addParent/setParents invariant) chain, with no cap and no allocation.
//
// hasAttribute is now that same linear scan (mirroring getAttribute's own
// chain-navigation loop exactly, minus getAttribute's attribute cache),
// and resolves a mutable receiver's current snapshot at every step,
// exactly as getAttribute and the fixed isInstanceOf/hasParent do.
//
// getAttribute's own separate 500-step cap (which, when hasAttribute was
// first fixed, was the one place the two could still disagree) is gone
// too — see test/GetAttributeNoCapTests.cpp. Neither has a depth cap any
// more; they always agree, at any depth.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"

using namespace proto;

class HasAttributeChainTest : public ::testing::Test {
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

// --- Baseline: own / inherited / absent, unchanged by the fix -------------

TEST_F(HasAttributeChainTest, OwnAttributeIsTrue) {
    const ProtoString* a = sym("a");
    const ProtoObject* obj = context->newObject(false);
    obj = obj->setAttribute(context, a, context->fromInteger(1));
    EXPECT_EQ(obj->hasAttribute(context, a), PROTO_TRUE);
}

TEST_F(HasAttributeChainTest, InheritedAttributeIsTrue) {
    const ProtoString* a = sym("a");
    const ProtoObject* base = context->newObject(false);
    base = base->setAttribute(context, a, context->fromInteger(1));
    const ProtoObject* child = base->newChild(context);
    EXPECT_EQ(child->hasAttribute(context, a), PROTO_TRUE);
}

TEST_F(HasAttributeChainTest, AbsentAttributeIsFalse) {
    const ProtoString* a = sym("a");
    const ProtoObject* obj = context->newObject(false);
    EXPECT_EQ(obj->hasAttribute(context, a), PROTO_FALSE);
}

TEST_F(HasAttributeChainTest, NoneValuedAttributeIsStillTrue) {
    // An attribute explicitly set to PROTO_NONE is still "present" —
    // distinct from "absent". implGetAt returns PROTO_NONE (a value), not
    // nullptr, for this case.
    const ProtoString* a = sym("a");
    const ProtoObject* obj = context->newObject(false);
    obj = obj->setAttribute(context, a, PROTO_NONE);
    EXPECT_EQ(obj->hasAttribute(context, a), PROTO_TRUE);
}

// A direct addParent-diamond: attribute lives only on the SECOND direct
// parent (not the first). A DFS that only properly explores the first
// direct entry's own subtree (the bug the earlier isInstanceOf fix
// addressed) would miss this; hasAttribute's old sibling-stack DFS did
// push `this`'s own siblings (unlike old isInstanceOf, which bootstrapped
// via getPrototype() and skipped them), so this specific shape was never
// broken for hasAttribute — kept here as a regression guard for the
// rewrite to a plain linear scan.
TEST_F(HasAttributeChainTest, DiamondAddParentFindsSecondParentsAttribute) {
    const ProtoString* onB = sym("onB");
    const ProtoObject* a = context->newObject(false);
    const ProtoObject* b = a->newChild(context);
    b = b->setAttribute(context, onB, context->fromInteger(42));
    const ProtoObject* c = a->newChild(context);
    const ProtoObject* d = context->newObject(false);
    d = d->addParent(context, b);
    d = d->addParent(context, c);

    EXPECT_EQ(d->hasAttribute(context, onB), PROTO_TRUE);
}

// --- The bug: a step cap gives a false negative on a deep chain -----------

// The chain the OLD 50-step cap could not reach: the attribute lives on
// the ROOT of a 60-level newChild chain. hasAttribute must find it.
TEST_F(HasAttributeChainTest, ChainDeeperThan50LevelsStillFindsRootAttribute) {
    const ProtoString* a = sym("a");
    const ProtoObject* leaf = buildChainWithAttrAtRoot(60, a);

    const unsigned long before = context->allocatedCellsCount;
    const ProtoObject* result = leaf->hasAttribute(context, a);
    EXPECT_EQ(context->allocatedCellsCount, before) << "hasAttribute must not allocate";
    EXPECT_EQ(result, PROTO_TRUE)
        << "a 60-level chain exceeds the old 50-step cap (which used to "
           "return PROTO_FALSE here); the cap is gone, this must now be "
           "PROTO_TRUE";
}

// Deeper still: beyond getAttribute's OWN (separate, untouched) 500-step
// cap. hasAttribute has no cap at all, so this must ALSO be PROTO_TRUE.
TEST_F(HasAttributeChainTest, ChainDeeperThan500LevelsStillFindsRootAttribute) {
    const ProtoString* a = sym("a");
    const ProtoObject* leaf = buildChainWithAttrAtRoot(520, a);

    const unsigned long before = context->allocatedCellsCount;
    const ProtoObject* result = leaf->hasAttribute(context, a);
    EXPECT_EQ(context->allocatedCellsCount, before) << "hasAttribute must not allocate";
    EXPECT_EQ(result, PROTO_TRUE);
}

// --- Agreement with getAttribute (getAttribute has no cap either) ---------

TEST_F(HasAttributeChainTest, AgreesWithGetAttributeOnAShallowChain) {
    const ProtoString* a = sym("a");
    const ProtoString* missing = sym("missing");
    const ProtoObject* leaf = buildChainWithAttrAtRoot(10, a);

    EXPECT_EQ(leaf->hasAttribute(context, a) == PROTO_TRUE,
              leaf->getAttribute(context, a) != PROTO_NONE);
    EXPECT_EQ(leaf->hasAttribute(context, missing) == PROTO_TRUE,
              leaf->getAttribute(context, missing) != PROTO_NONE);
}

TEST_F(HasAttributeChainTest, AgreesWithGetAttributeJustUnderGetAttributesCap) {
    const ProtoString* a = sym("a");
    const ProtoObject* leaf = buildChainWithAttrAtRoot(490, a);

    EXPECT_EQ(leaf->hasAttribute(context, a), PROTO_TRUE);
    EXPECT_NE(leaf->getAttribute(context, a), PROTO_NONE);
}

// --- No more divergence: getAttribute's cap is gone too --------------------

// getAttribute used to cap its walk at 500 steps and give up
// (PROTO_NONE) on an attribute living further down the chain than that,
// even though hasAttribute (already uncapped) found it — the one
// documented exception to "hasAttribute agrees with getAttribute". That
// cap is gone (see test/GetAttributeNoCapTests.cpp): both now agree at
// any depth.
TEST_F(HasAttributeChainTest, AgreesWithGetAttributeBeyond500Levels) {
    const ProtoString* a = sym("a");
    const ProtoObject* leaf = buildChainWithAttrAtRoot(520, a);

    EXPECT_EQ(leaf->hasAttribute(context, a), PROTO_TRUE);
    EXPECT_EQ(leaf->getAttribute(context, a), context->fromInteger(1))
        << "getAttribute's 500-step cap is gone; this must now be found, "
           "agreeing with hasAttribute";
}

// --- Mutable receiver: current snapshot, not birth state -------------------

TEST_F(HasAttributeChainTest, MutableReceiverResolvesCurrentSnapshot) {
    const ProtoString* a = sym("a");
    const ProtoObject* base = context->newObject(false);
    base = base->setAttribute(context, a, context->fromInteger(9));

    auto* m = const_cast<ProtoObject*>(context->newObject(true));
    EXPECT_EQ(m->hasAttribute(context, a), PROTO_FALSE);

    m->addParent(context, base);
    EXPECT_EQ(m->hasAttribute(context, a), PROTO_TRUE);
}

// A newChild of a mutable prototype must see the prototype's CURRENT
// chain (the C1 fix from the previous round) — hasAttribute must agree.
TEST_F(HasAttributeChainTest, NewChildFromMutableClassSeesAttributeThroughHasAttribute) {
    const ProtoString* a = sym("a");
    const ProtoObject* base = context->newObject(false);
    base = base->setAttribute(context, a, context->fromInteger(3));

    auto* cls = const_cast<ProtoObject*>(context->newObject(true));
    cls->setParents(context, context->newList()->appendLast(context, base));

    const ProtoObject* inst = cls->newChild(context);
    EXPECT_EQ(inst->hasAttribute(context, a), PROTO_TRUE);
}

// --- Non-object receiver: answered through its prototype -------------------

TEST_F(HasAttributeChainTest, NonObjectReceiverAnsweredThroughPrototype) {
    const ProtoString* toStringAttr = sym("__nonexistent_marker__");
    const ProtoObject* smallInt = context->fromInteger(7);
    EXPECT_EQ(smallInt->hasAttribute(context, toStringAttr), PROTO_FALSE);

    const ProtoString* a = sym("onIntProto");
    context->space->objectPrototype->setAttribute(context, a, context->fromInteger(1));
    EXPECT_EQ(smallInt->hasAttribute(context, a), PROTO_TRUE);
}
