// SetParentsFlattenTests.cpp — ProtoObjectCell::setParents flattening.
//
// setParents used to install exactly the given list, without copying in
// each listed parent's own ancestors — the one construction path that left
// an object's own chain incomplete (newChild and addParent both always
// left every transitive ancestor as a direct entry). That made
// getAttribute/hasParent silently blind to a grand-parent's attributes
// through a setParents-built object, even though isInstanceOf's DFS could
// still find it by separately walking a listed parent's own chain.
//
// setParents now flattens, giving every construction path the same
// invariant: an object's own chain always contains every one of its
// ancestors as a direct entry. The new chain is, in order:
//   1. the entries of the given list, in the given order, de-duplicated;
//   2. every ancestor of each of those listed parents — walking each
//      parent's own chain in that parent's own order, in the same order
//      the parents were listed — that is not already present.
//
// This file covers:
//   - the no-op property: a list that already contains every ancestor of
//     every listed parent (e.g. a full linearization) is installed exactly
//     as given, in the exact same order;
//   - a non-flat list is flattened;
//   - the resulting visibility change: getAttribute now finds a
//     grand-parent's attribute that was invisible before this fix
//     (documented here as the INTENDED behaviour change, not a
//     regression);
//   - isInstanceOf/hasParent/getAttribute all agree on what is visible,
//     now that every chain is flat by construction;
//   - mutable objects after setParents;
//   - cycle detection: setParents on a mutable object throws
//     std::invalid_argument rather than building (or looping while
//     building) a self-referential chain.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include <stdexcept>

using namespace proto;

class SetParentsFlattenTest : public ::testing::Test {
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

    // Asserts `obj`'s own chain (getParents(), head-first) is exactly
    // `expected`, in that exact order.
    void expectChainOrder(const ProtoObject* obj, std::initializer_list<const ProtoObject*> expected) {
        const ProtoList* parents = obj->getParents(context);
        ASSERT_EQ(static_cast<size_t>(parents->getSize(context)), expected.size());
        size_t i = 0;
        for (const ProtoObject* e : expected) {
            EXPECT_EQ(parents->getAt(context, static_cast<int>(i)), e) << "at index " << i;
            ++i;
        }
    }
};

// --- Order-preserving, no-op for an already-complete linearization --------

// A Scala-style C3-ish linearization [C, T2, T1, B, A, Root] that already
// lists every ancestor of every entry (T2's ancestor T1 is already listed;
// B's and A's ancestor Root is already listed). Flattening must add
// NOTHING and must not reorder anything: this is a no-op relative to
// installing the list directly, exactly as it always was for a complete
// list.
TEST_F(SetParentsFlattenTest, NoOpForAlreadyCompleteLinearization) {
    const ProtoObject* root = context->newObject(false);
    const ProtoObject* a = root->newChild(context);
    const ProtoObject* b = root->newChild(context);
    const ProtoObject* t1 = context->newObject(false);
    const ProtoObject* t2 = t1->newChild(context);
    const ProtoObject* c = context->newObject(false);

    const ProtoList* linearization = context->newList()
        ->appendLast(context, c)
        ->appendLast(context, t2)
        ->appendLast(context, t1)
        ->appendLast(context, b)
        ->appendLast(context, a)
        ->appendLast(context, root);

    const ProtoObject* x = context->newObject(false);
    x = x->setParents(context, linearization);

    expectChainOrder(x, {c, t2, t1, b, a, root});
}

// --- Non-flat input is flattened -------------------------------------------

TEST_F(SetParentsFlattenTest, NonFlatListIsFlattened) {
    const ProtoObject* g = context->newObject(false);
    const ProtoObject* p = g->newChild(context);

    const ProtoObject* x = context->newObject(false);
    const ProtoList* plist = context->newList()->appendLast(context, p);
    x = x->setParents(context, plist);

    // P's own ancestor G is now copied in — a direct entry, not just
    // transitively reachable.
    expectChainOrder(x, {p, g});
    EXPECT_EQ(x->hasParent(context, g), 1);
}

// Order preservation with multiple listed parents whose own ancestors
// overlap: listed parents come first (in order, de-duplicated), then each
// one's own missing ancestors, in listed-parent order.
TEST_F(SetParentsFlattenTest, MultipleListedParentsAncestorsAppendedInListedOrder) {
    const ProtoObject* root = context->newObject(false);
    const ProtoObject* p1 = root->newChild(context);   // p1.chain = [root]
    const ProtoObject* p2 = root->newChild(context);   // p2.chain = [root]

    const ProtoObject* x = context->newObject(false);
    const ProtoList* plist = context->newList()
        ->appendLast(context, p1)
        ->appendLast(context, p2);
    x = x->setParents(context, plist);

    // p1 and p2 first (listed order), then root once (p1's own ancestor;
    // p2's own ancestor root is already present by the time p2 is
    // processed, so it is not duplicated).
    expectChainOrder(x, {p1, p2, root});
}

// A duplicate in the given list itself is dropped, keeping the first
// occurrence's position.
TEST_F(SetParentsFlattenTest, DuplicateListedParentIsDropped) {
    const ProtoObject* p = context->newObject(false);
    const ProtoObject* q = context->newObject(false);

    const ProtoObject* x = context->newObject(false);
    const ProtoList* plist = context->newList()
        ->appendLast(context, p)
        ->appendLast(context, q)
        ->appendLast(context, p);
    x = x->setParents(context, plist);

    expectChainOrder(x, {p, q});
}

// --- Intended behaviour change: getAttribute now sees a grand-parent's
// attribute through a setParents-built chain -------------------------------

TEST_F(SetParentsFlattenTest, GetAttributeNowFindsGrandParentAttributeThroughSetParents) {
    const ProtoString* gAttr = sym("gAttr");
    const ProtoObject* g = context->newObject(false);
    g = g->setAttribute(context, gAttr, context->fromInteger(99));
    const ProtoObject* p = g->newChild(context);

    const ProtoObject* x = context->newObject(false);
    const ProtoList* plist = context->newList()->appendLast(context, p);
    x = x->setParents(context, plist);

    // Before this fix, x's own chain was [p] only, so getAttribute never
    // reached g and this returned PROTO_NONE. setParents now flattens g
    // into x's own chain, so this is now found — an INTENDED behaviour
    // change, not a bug.
    EXPECT_EQ(x->getAttribute(context, gAttr), context->fromInteger(99));
}

// --- isInstanceOf / hasParent / getAttribute now agree --------------------

TEST_F(SetParentsFlattenTest, IsInstanceOfHasParentAndGetAttributeAgree) {
    const ProtoString* gAttr = sym("gAttr");
    const ProtoObject* g = context->newObject(false);
    g = g->setAttribute(context, gAttr, context->fromInteger(7));
    const ProtoObject* p = g->newChild(context);

    const ProtoObject* x = context->newObject(false);
    const ProtoList* plist = context->newList()->appendLast(context, p);
    x = x->setParents(context, plist);

    const bool viaIsInstanceOf = (x->isInstanceOf(context, g) == PROTO_TRUE);
    const bool viaHasParent = (x->hasParent(context, g) == 1);
    const bool viaGetAttribute = (x->getAttribute(context, gAttr) != PROTO_NONE);

    EXPECT_TRUE(viaIsInstanceOf);
    EXPECT_TRUE(viaHasParent);
    EXPECT_TRUE(viaGetAttribute);
    EXPECT_EQ(viaIsInstanceOf, viaHasParent);
    EXPECT_EQ(viaHasParent, viaGetAttribute);
}

// --- Mutable objects ---------------------------------------------------

TEST_F(SetParentsFlattenTest, MutableObjectAfterSetParentsFlattens) {
    const ProtoString* gAttr = sym("gAttr");
    const ProtoObject* g = context->newObject(false);
    g = g->setAttribute(context, gAttr, context->fromInteger(5));
    const ProtoObject* p = g->newChild(context);

    auto* m = const_cast<ProtoObject*>(context->newObject(true));
    const ProtoList* plist = context->newList()->appendLast(context, p);
    m->setParents(context, plist);

    expectChainOrder(m, {p, g});
    EXPECT_EQ(m->isInstanceOf(context, g), PROTO_TRUE);
    EXPECT_EQ(m->hasParent(context, g), 1);
    EXPECT_EQ(m->getAttribute(context, gAttr), context->fromInteger(5));
}

// --- Cycle detection --------------------------------------------------

// A mutable object's handle is stable across mutation, so it is the only
// case where setParents could be asked to make an object its own ancestor
// (directly, or through another mutable object's chain). setParents must
// detect this and throw rather than building a self-referential chain (or
// looping while trying to).
TEST_F(SetParentsFlattenTest, MutualMutableCycleThrows) {
    auto* a = const_cast<ProtoObject*>(context->newObject(true));
    auto* b = const_cast<ProtoObject*>(context->newObject(true));

    const ProtoList* aParents = context->newList()->appendLast(context, b);
    a->setParents(context, aParents); // a.chain = [b] — fine, no cycle yet.

    // b.setParents([a]) would flatten in a's own chain, which contains b
    // (the object currently being reshaped) — a genuine cycle.
    const ProtoList* bParents = context->newList()->appendLast(context, a);
    EXPECT_THROW(b->setParents(context, bParents), std::invalid_argument);

    // b must be left unchanged (the exception is thrown before any
    // mutable-shard publish): still has no parents of its own.
    EXPECT_EQ(b->hasParent(context, a), 0);
}

TEST_F(SetParentsFlattenTest, DirectSelfReferenceThrows) {
    auto* a = const_cast<ProtoObject*>(context->newObject(true));
    const ProtoList* selfList = context->newList()->appendLast(context, a);
    EXPECT_THROW(a->setParents(context, selfList), std::invalid_argument);
}

// An immutable receiver can never become its own ancestor: setParents on
// an immutable object always builds a brand-new handle nothing could have
// referenced yet, so listing the OLD handle among the new parents is not a
// cycle (the new object and the old one are different identities) and must
// not throw.
TEST_F(SetParentsFlattenTest, ImmutableReceiverInItsOwnNewParentsListDoesNotThrow) {
    const ProtoObject* a = context->newObject(false);
    const ProtoObject* b = a->newChild(context); // b.chain = [a]

    const ProtoList* plist = context->newList()->appendLast(context, b);
    const ProtoObject* newA = nullptr;
    EXPECT_NO_THROW(newA = a->setParents(context, plist));
    ASSERT_NE(newA, nullptr);
    // newA's chain is [b, a] (b flattened in its own ancestor a) — a
    // reference to the OLD `a` handle, a different, ordinary ancestor of
    // the NEW `a`, not a self-reference.
    expectChainOrder(newA, {b, a});
}
