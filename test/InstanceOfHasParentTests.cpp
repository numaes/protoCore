// InstanceOfHasParentTests.cpp — ProtoObject::isInstanceOf / ProtoObject::hasParent.
//
// isInstanceOf used to walk with a fixed-size (64-slot) sibling stack, an
// arbitrary 50-step cap, and returned PROTO_FALSE (a third, distinct value)
// once that cap was exceeded — giving a WRONG answer for any hierarchy
// deeper than 50 links.  hasParent allocated a ProtoList via getParents()
// on every call just to test membership.
//
// This file pins down the corrected contract across every construction
// path that can shape an object's parent chain. newChild, addParent AND
// setParents (see SetParentsFlattenTests.cpp for setParents's own
// flattening algorithm) all now guarantee that an object's own chain
// already contains every one of its ancestors as a direct entry, so
// isInstanceOf is a pure, allocation-free, single-level linear scan for
// every object, with no recursion and no length limit — the same chain
// getAttribute and hasParent (also allocation-free, no list built) walk.
// Mutable objects, after addParent/setParents, must be answered for their
// CURRENT snapshot by isInstanceOf and hasParent alike (getPrototype()
// does not resolve one; isInstanceOf must not depend on getPrototype() for
// object receivers for this reason).
//
// Two latent bugs surfaced while characterizing the OLD implementation and
// are intentionally NOT reproduced here (see the report for detail):
//   (a) isInstanceOf only ever explored the RECEIVER's own chain through
//       getPrototype(), which returns just the first entry — a second or
//       third parent added via addParent (e.g. the classic diamond,
//       chain=[C,B,A]) was silently unreachable even though hasParent
//       correctly reported it present.
//   (b) isInstanceOf never resolved a mutable receiver's current snapshot
//       (getPrototype() does not), so it answered false for every parent
//       ever added to a mutable object.
// Both are fixed as an unavoidable consequence of scanning the receiver's
// own resolved chain directly (what this fix does, and what getAttribute
// already does) instead of bootstrapping from getPrototype().

#include <gtest/gtest.h>
#include "../headers/protoCore.h"

using namespace proto;

class InstanceOfHasParentTest : public ::testing::Test {
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
};

// --- newChild: flat by construction ----------------------------------------

TEST_F(InstanceOfHasParentTest, NewChildDirectParent) {
    const ProtoObject* base = context->newObject(false);
    const ProtoObject* child = base->newChild(context);

    EXPECT_EQ(child->isInstanceOf(context, base), PROTO_TRUE);
    EXPECT_EQ(child->hasParent(context, base), 1);
}

TEST_F(InstanceOfHasParentTest, NewChildGrandParent) {
    const ProtoObject* grandparent = context->newObject(false);
    const ProtoObject* parent = grandparent->newChild(context);
    const ProtoObject* child = parent->newChild(context);

    EXPECT_EQ(child->isInstanceOf(context, grandparent), PROTO_TRUE);
    EXPECT_EQ(child->isInstanceOf(context, parent), PROTO_TRUE);
    // hasParent is shallow: grandparent is NOT a direct entry of child's
    // own chain unless newChild copied it in, which it does (structural
    // sharing) — so this one IS found, unlike the setParents case below.
    EXPECT_EQ(child->hasParent(context, grandparent), 1);
    EXPECT_EQ(child->hasParent(context, parent), 1);
}

TEST_F(InstanceOfHasParentTest, UnrelatedIsFalse) {
    const ProtoObject* base = context->newObject(false);
    const ProtoObject* child = base->newChild(context);
    const ProtoObject* unrelated = context->newObject(false);

    EXPECT_EQ(child->isInstanceOf(context, unrelated), PROTO_NONE);
    EXPECT_EQ(child->hasParent(context, unrelated), 0);
}

TEST_F(InstanceOfHasParentTest, NullPrototypeOrTargetIsFalse) {
    const ProtoObject* base = context->newObject(false);
    const ProtoObject* child = base->newChild(context);

    EXPECT_EQ(child->isInstanceOf(context, nullptr), PROTO_NONE);
    EXPECT_EQ(child->hasParent(context, nullptr), 0);
}

TEST_F(InstanceOfHasParentTest, HasParentTrueForSelf) {
    const ProtoObject* obj = context->newObject(false);
    EXPECT_EQ(obj->hasParent(context, obj), 1);
}

// --- addParent, including the diamond / multi-parent case ------------------

// The classic diamond: D gets B then C added as parents. addParent
// flattens transitively (D's own chain becomes [C, B, A], all three as
// direct entries — see MultipleInheritanceTests.cpp for the construction
// trace). isInstanceOf must find ALL THREE direct/transitive ancestors:
// this is the case where the old getPrototype()-only-sees-the-first-entry
// implementation silently missed B (bug (a) above).
TEST_F(InstanceOfHasParentTest, DiamondAddParentFindsEveryAncestor) {
    const ProtoObject* a = context->newObject(false);
    const ProtoObject* b = a->newChild(context);
    const ProtoObject* c = a->newChild(context);
    const ProtoObject* d = context->newObject(false);
    d = d->addParent(context, b);
    d = d->addParent(context, c);

    EXPECT_EQ(d->isInstanceOf(context, a), PROTO_TRUE);
    EXPECT_EQ(d->isInstanceOf(context, b), PROTO_TRUE)
        << "B is a direct addParent'd parent of D; must be found";
    EXPECT_EQ(d->isInstanceOf(context, c), PROTO_TRUE);

    EXPECT_EQ(d->hasParent(context, a), 1);
    EXPECT_EQ(d->hasParent(context, b), 1);
    EXPECT_EQ(d->hasParent(context, c), 1);
}

TEST_F(InstanceOfHasParentTest, MixinAddedWithAddParentToImmutableChild) {
    const ProtoObject* base = context->newObject(false);
    const ProtoObject* child = base->newChild(context);
    const ProtoObject* mixin = context->newObject(false);

    const ProtoObject* withMixin = child->addParent(context, mixin);

    EXPECT_EQ(withMixin->isInstanceOf(context, mixin), PROTO_TRUE);
    EXPECT_EQ(withMixin->isInstanceOf(context, base), PROTO_TRUE)
        << "adding a mixin must not drop the existing chain";
}

// --- setParents: now flattens too (see SetParentsFlattenTests.cpp for the
// dedicated coverage of the flattening algorithm itself) -------------------

// C's chain is set from [P] (a single-entry, non-flattened-looking list).
// setParents now flattens it: C's own chain becomes [P, G] (G copied in
// from P's own chain). isInstanceOf, hasParent AND getAttribute must all
// now agree that G is present — this is the behaviour change from
// SetParents flattening; see SetParentsFlattenTests.cpp for the dedicated
// "getAttribute now finds a grand-parent's attribute" test.
TEST_F(InstanceOfHasParentTest, SetParentsFlattensNonFlatInput) {
    const ProtoString* gAttr = sym("gAttr");
    const ProtoObject* g = context->newObject(false);
    g = g->setAttribute(context, gAttr, context->fromInteger(99));
    const ProtoObject* p = g->newChild(context);

    const ProtoObject* c = context->newObject(false);
    const ProtoList* plist = context->newList()->appendLast(context, p);
    c = c->setParents(context, plist);

    EXPECT_EQ(c->isInstanceOf(context, p), PROTO_TRUE);
    EXPECT_EQ(c->isInstanceOf(context, g), PROTO_TRUE);

    EXPECT_EQ(c->hasParent(context, p), 1);
    EXPECT_EQ(c->hasParent(context, g), 1)
        << "setParents now flattens: G is a direct entry of C's own chain";

    EXPECT_EQ(c->getAttribute(context, gAttr), context->fromInteger(99))
        << "setParents flattening makes G's attribute visible through C";
}

// Two setParents/newChild hops away: leaf = leafBase.setParents([mid]),
// grandleaf = leaf.newChild(). Since setParents now flattens, leaf's own
// chain already includes root (copied in from mid's chain), and newChild
// carries that whole flat chain into grandleaf — so root is a DIRECT entry
// of grandleaf's own chain too, not just isInstanceOf-reachable.
TEST_F(InstanceOfHasParentTest, SetParentsThenNewChildTwoHopsDeep) {
    const ProtoObject* root = context->newObject(false);
    const ProtoObject* mid = root->newChild(context);
    const ProtoObject* leafBase = context->newObject(false);
    const ProtoList* pl = context->newList()->appendLast(context, mid);
    const ProtoObject* leaf = leafBase->setParents(context, pl);
    const ProtoObject* grandleaf = leaf->newChild(context);

    EXPECT_EQ(grandleaf->isInstanceOf(context, leaf), PROTO_TRUE);
    EXPECT_EQ(grandleaf->isInstanceOf(context, mid), PROTO_TRUE);
    EXPECT_EQ(grandleaf->isInstanceOf(context, root), PROTO_TRUE);

    EXPECT_EQ(grandleaf->hasParent(context, leaf), 1);
    EXPECT_EQ(grandleaf->hasParent(context, mid), 1);
    EXPECT_EQ(grandleaf->hasParent(context, root), 1)
        << "setParents flattening means root is now a direct entry, not "
           "just transitively reachable";
}

// --- The intended behaviour change: no more length limit -------------------

TEST_F(InstanceOfHasParentTest, ThousandLevelNewChildChainNowFindsRoot) {
    const ProtoObject* root = context->newObject(false);
    const ProtoObject* current = root;
    for (int i = 0; i < 1000; ++i) {
        current = current->newChild(context);
    }

    const unsigned long before = context->allocatedCellsCount;
    const ProtoObject* result = current->isInstanceOf(context, root);
    EXPECT_EQ(context->allocatedCellsCount, before)
        << "isInstanceOf must not allocate";
    EXPECT_EQ(result, PROTO_TRUE)
        << "a 1000-level chain exceeds the old 50-step cap (which used to "
           "return PROTO_FALSE here); the cap is gone, this must now be "
           "PROTO_TRUE";

    const ProtoObject* other = context->newObject(false);
    EXPECT_EQ(current->isInstanceOf(context, other), PROTO_NONE);

    EXPECT_EQ(current->hasParent(context, root), 1);
}

// --- Mutable objects: addParent and setParents ------------------------------

// getPrototype() (unlike getFirstParent/getParents/getAttribute) never
// resolved a mutable object's current snapshot. isInstanceOf must not
// depend on it for object receivers, or every mutable object with an added
// parent would silently answer false (bug (b) above).
TEST_F(InstanceOfHasParentTest, MutableObjectAfterAddParent) {
    auto* m = const_cast<ProtoObject*>(context->newObject(true));
    const ProtoObject* p = context->newObject(false);
    m->addParent(context, p);

    EXPECT_EQ(m->isInstanceOf(context, p), PROTO_TRUE);
    EXPECT_EQ(m->hasParent(context, p), 1);
}

TEST_F(InstanceOfHasParentTest, MutableObjectAfterSetParentsReplacesChain) {
    auto* m = const_cast<ProtoObject*>(context->newObject(true));
    const ProtoObject* oldP = context->newObject(false);
    const ProtoObject* newP = context->newObject(false);

    m->addParent(context, oldP);
    ASSERT_EQ(m->isInstanceOf(context, oldP), PROTO_TRUE);

    const ProtoList* replacement = context->newList()->appendLast(context, newP);
    m->setParents(context, replacement);

    EXPECT_EQ(m->isInstanceOf(context, newP), PROTO_TRUE);
    EXPECT_EQ(m->isInstanceOf(context, oldP), PROTO_NONE)
        << "setParents replaces the chain wholesale";

    EXPECT_EQ(m->hasParent(context, newP), 1);
    EXPECT_EQ(m->hasParent(context, oldP), 0);
}

// --- clone: preserves whatever chain shape the original had ----------------

TEST_F(InstanceOfHasParentTest, CloneAnswersLikeTheOriginal) {
    const ProtoObject* base = context->newObject(false);
    const ProtoObject* child = base->newChild(context);
    const ProtoObject* clone = child->clone(context, false);

    EXPECT_EQ(clone->isInstanceOf(context, base), PROTO_TRUE);
    EXPECT_EQ(clone->hasParent(context, base), 1);
}

// --- Non-object receivers: answered through their prototype ---------------

TEST_F(InstanceOfHasParentTest, EmbeddedValueAnsweredThroughPrototype) {
    const ProtoObject* smallInt = context->fromInteger(42);

    EXPECT_EQ(smallInt->isInstanceOf(context, context->space->objectPrototype), PROTO_TRUE);
    EXPECT_EQ(smallInt->isInstanceOf(context, context->space->stringPrototype), PROTO_NONE);
    // hasParent's shallow, own-chain-only contract does not extend through
    // a non-object receiver's prototype at all (matches its pre-fix
    // behaviour: getParents() on a non-object returns an empty list).
    EXPECT_EQ(smallInt->hasParent(context, context->space->objectPrototype), 0);
}

TEST_F(InstanceOfHasParentTest, StringAnsweredThroughPrototype) {
    const ProtoObject* str = context->fromUTF8String("a string long enough to not be inlined");

    EXPECT_EQ(str->isInstanceOf(context, context->space->stringPrototype), PROTO_TRUE);
    EXPECT_EQ(str->isInstanceOf(context, context->space->listPrototype), PROTO_NONE);
}

// --- C1: newChild from a MUTABLE prototype must see its CURRENT chain -----
//
// newChild used to read the prototype handle cell's own `parent` field
// directly, which for a mutable object is fixed at newObject(true) time and
// never updated in place (mutation publishes into the mutable shard
// instead) -- so a child of a mutable class that was reparented AFTER the
// class was made mutable (but still BEFORE the child was created) silently
// lost every ancestor. Fixed by resolving the prototype's current snapshot
// first, matching getAttribute/getParents/hasParent/isInstanceOf.

TEST_F(InstanceOfHasParentTest, NewChildFromMutableClassSeesAncestorsAddedBeforeCreation) {
    const ProtoString* baseAttr = sym("baseAttr");
    const ProtoObject* base = context->newObject(false);
    base = base->setAttribute(context, baseAttr, context->fromInteger(11));

    auto* cls = const_cast<ProtoObject*>(context->newObject(true));
    cls->setParents(context, context->newList()->appendLast(context, base));

    const ProtoObject* inst = cls->newChild(context);

    EXPECT_EQ(inst->isInstanceOf(context, base), PROTO_TRUE);
    EXPECT_EQ(inst->hasParent(context, base), 1);
    EXPECT_EQ(inst->getAttribute(context, baseAttr), context->fromInteger(11));
}

// protoPython's exact pattern: newObject(true), then addParent (not
// setParents), then newChild for each instance.
TEST_F(InstanceOfHasParentTest, NewChildFromMutableClassBuiltWithAddParent) {
    const ProtoString* baseAttr = sym("baseAttr");
    const ProtoObject* base = context->newObject(false);
    base = base->setAttribute(context, baseAttr, context->fromInteger(22));

    auto* cls = const_cast<ProtoObject*>(context->newObject(true));
    cls->addParent(context, base);

    const ProtoObject* inst1 = cls->newChild(context);
    const ProtoObject* inst2 = cls->newChild(context, /*isMutable=*/true);

    EXPECT_EQ(inst1->isInstanceOf(context, base), PROTO_TRUE);
    EXPECT_EQ(inst1->getAttribute(context, baseAttr), context->fromInteger(22));
    EXPECT_EQ(inst2->isInstanceOf(context, base), PROTO_TRUE);
    EXPECT_EQ(inst2->getAttribute(context, baseAttr), context->fromInteger(22));
}

// A mutable class re-parented AFTER an instance already exists: the
// EXISTING instance's chain was captured by value at its own creation and
// does NOT retroactively gain the new ancestor; a NEW instance created
// AFTER the re-parenting DOES see it. This is the "capture at creation
// time" contract newChild's doc comment states, and the shape protoST's
// object_prims.cpp (D21) documents relying on for its OWN "future
// instances" semantics.
TEST_F(InstanceOfHasParentTest, MutableClassReparentedAfterInstanceExists) {
    const ProtoObject* oldBase = context->newObject(false);
    const ProtoObject* newBase = context->newObject(false);

    auto* cls = const_cast<ProtoObject*>(context->newObject(true));
    cls->setParents(context, context->newList()->appendLast(context, oldBase));

    const ProtoObject* earlyInstance = cls->newChild(context);
    ASSERT_EQ(earlyInstance->isInstanceOf(context, oldBase), PROTO_TRUE);

    // Re-parent the class.
    cls->setParents(context, context->newList()->appendLast(context, newBase));

    // The early instance's chain is unaffected.
    EXPECT_EQ(earlyInstance->isInstanceOf(context, oldBase), PROTO_TRUE);
    EXPECT_EQ(earlyInstance->isInstanceOf(context, newBase), PROTO_NONE)
        << "an instance created before a re-parenting does not "
           "retroactively see the new ancestor";

    // A new instance, created after the re-parenting, sees the new base
    // and NOT the old one.
    const ProtoObject* lateInstance = cls->newChild(context);
    EXPECT_EQ(lateInstance->isInstanceOf(context, newBase), PROTO_TRUE);
    EXPECT_EQ(lateInstance->isInstanceOf(context, oldBase), PROTO_NONE);
}

// --- C2: universal-root fallback --------------------------------------

TEST_F(InstanceOfHasParentTest, ParentlessObjectIsInstanceOfObjectPrototype) {
    const ProtoObject* plain = context->newObject(false);
    EXPECT_EQ(plain->isInstanceOf(context, context->space->objectPrototype), PROTO_TRUE);

    auto* plainMutable = const_cast<ProtoObject*>(context->newObject(true));
    EXPECT_EQ(plainMutable->isInstanceOf(context, context->space->objectPrototype), PROTO_TRUE);
}

TEST_F(InstanceOfHasParentTest, ObjectPrototypeIsNotItsOwnInstance) {
    EXPECT_EQ(context->space->objectPrototype->isInstanceOf(context, context->space->objectPrototype), PROTO_NONE);
}

TEST_F(InstanceOfHasParentTest, ObjectWithExplicitChainIsNotImplicitlyRootedAtObjectPrototype) {
    // An object with an explicit chain of its own is NOT implicitly
    // considered a descendant of objectPrototype unless its own
    // construction put objectPrototype there — the universal-root
    // fallback applies only to a genuinely parentless object.
    const ProtoObject* base = context->newObject(false);
    const ProtoObject* child = base->newChild(context);

    EXPECT_EQ(child->isInstanceOf(context, context->space->objectPrototype), PROTO_NONE);
}
