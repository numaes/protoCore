// GetAttributesMergeTests.cpp — ProtoObject::getAttributes merged view.
//
// getAttributes() used to recurse into only the FIRST parent link
// (`pl->getObject(context)->getAttributes(context)`), never visiting a
// second or later DIRECT parent at all. For an object built via more than
// one addParent call (a diamond) or via setParents with more than one
// listed parent, every attribute that lived only on the second-or-later
// parent (or one of ITS ancestors, since the recursion covers whatever
// that first-missed parent's own chain holds) was silently absent from
// the merged view — even though getAttribute/hasAttribute/isInstanceOf
// (all fixed in earlier rounds to walk the receiver's own FLATTENED
// chain directly) already saw it. The three disagreed.
//
// getAttributes() is now the same flattened-chain walk (own attributes
// seeded first, then each chain entry in order), so it always agrees with
// getAttribute/hasAttribute on what an object has, with the SAME
// precedence rule: own attributes win over any ancestor's, and a NEARER
// ancestor (earlier in the chain) wins over a FARTHER one for the same
// key.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"

using namespace proto;

class GetAttributesMergeTest : public ::testing::Test {
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

    static bool contains(const ProtoSparseList* attrs, ProtoContext* ctx, const ProtoString* name) {
        return attrs->has(ctx, reinterpret_cast<uintptr_t>(name));
    }
    static const ProtoObject* valueOf(const ProtoSparseList* attrs, ProtoContext* ctx, const ProtoString* name) {
        return attrs->getAt(ctx, reinterpret_cast<uintptr_t>(name));
    }
};

// --- Baseline: own + single-parent chain, unchanged by the fix ------------

TEST_F(GetAttributesMergeTest, OwnAttributesOnly) {
    const ProtoString* a = sym("a");
    const ProtoObject* obj = context->newObject(false);
    obj = obj->setAttribute(context, a, context->fromInteger(1));

    const ProtoSparseList* attrs = obj->getAttributes(context);
    ASSERT_TRUE(contains(attrs, context, a));
    EXPECT_EQ(valueOf(attrs, context, a), context->fromInteger(1));
}

TEST_F(GetAttributesMergeTest, SingleParentChainMerges) {
    const ProtoString* a = sym("a");
    const ProtoString* b = sym("b");
    const ProtoObject* base = context->newObject(false);
    base = base->setAttribute(context, a, context->fromInteger(1));
    const ProtoObject* child = base->newChild(context);
    child = child->setAttribute(context, b, context->fromInteger(2));

    const ProtoSparseList* attrs = child->getAttributes(context);
    EXPECT_EQ(valueOf(attrs, context, a), context->fromInteger(1));
    EXPECT_EQ(valueOf(attrs, context, b), context->fromInteger(2));
}

// --- The bug: a second-or-later direct parent was never visited -----------

// addParent diamond: D gets B then C. D's own chain (flattened) is
// [C, B, A] (see MultipleInheritanceTests.cpp for the construction
// trace). The OLD code recursed only into C (D's FIRST direct parent) and
// never visited B at all — even though B is a DIRECT entry of D's own
// chain, not something reachable only through C.
TEST_F(GetAttributesMergeTest, AddParentDiamondMergesEveryDirectParent) {
    const ProtoString* onA = sym("onA");
    const ProtoString* onB = sym("onB");
    const ProtoString* onC = sym("onC");

    const ProtoObject* a = context->newObject(false);
    a = a->setAttribute(context, onA, context->fromInteger(1));
    const ProtoObject* b = a->newChild(context);
    b = b->setAttribute(context, onB, context->fromInteger(2));
    const ProtoObject* c = a->newChild(context);
    c = c->setAttribute(context, onC, context->fromInteger(3));

    const ProtoObject* d = context->newObject(false);
    d = d->addParent(context, b);
    d = d->addParent(context, c);

    const ProtoSparseList* attrs = d->getAttributes(context);
    EXPECT_EQ(valueOf(attrs, context, onA), context->fromInteger(1));
    EXPECT_EQ(valueOf(attrs, context, onB), context->fromInteger(2))
        << "B is D's second direct parent (added first, then C); its own "
           "attribute must be in the merged view";
    EXPECT_EQ(valueOf(attrs, context, onC), context->fromInteger(3));
}

// setParents with more than one listed parent: same shape, different
// construction path.
TEST_F(GetAttributesMergeTest, SetParentsMultipleListedParentsAllMerge) {
    const ProtoString* onB = sym("onB");
    const ProtoString* onC = sym("onC");

    const ProtoObject* b = context->newObject(false);
    b = b->setAttribute(context, onB, context->fromInteger(10));
    const ProtoObject* c = context->newObject(false);
    c = c->setAttribute(context, onC, context->fromInteger(20));

    const ProtoObject* d = context->newObject(false);
    const ProtoList* plist = context->newList()->appendLast(context, c)->appendLast(context, b);
    d = d->setParents(context, plist);

    const ProtoSparseList* attrs = d->getAttributes(context);
    EXPECT_EQ(valueOf(attrs, context, onB), context->fromInteger(10))
        << "B is the SECOND listed parent; the old first-parent-only "
           "recursion would have missed it";
    EXPECT_EQ(valueOf(attrs, context, onC), context->fromInteger(20));
}

// --- Shadowing precedence: own > nearer ancestor > farther ancestor -------

TEST_F(GetAttributesMergeTest, OwnAttributeShadowsAnyAncestor) {
    const ProtoString* shared = sym("shared");
    const ProtoObject* base = context->newObject(false);
    base = base->setAttribute(context, shared, context->fromInteger(100));
    const ProtoObject* child = base->newChild(context);
    child = child->setAttribute(context, shared, context->fromInteger(1));

    const ProtoSparseList* attrs = child->getAttributes(context);
    EXPECT_EQ(valueOf(attrs, context, shared), context->fromInteger(1));
}

// D's chain (via addParent(b); addParent(c)) is [C, B, A]: C is nearer
// than B. Both C and B define `shared` directly (not through an
// ancestor) — C (nearer) must win.
TEST_F(GetAttributesMergeTest, NearerListedParentShadowsFartherOne) {
    const ProtoString* shared = sym("shared");
    const ProtoObject* a = context->newObject(false);
    const ProtoObject* b = a->newChild(context);
    b = b->setAttribute(context, shared, context->fromInteger(2));
    const ProtoObject* c = a->newChild(context);
    c = c->setAttribute(context, shared, context->fromInteger(3));

    const ProtoObject* d = context->newObject(false);
    d = d->addParent(context, b);
    d = d->addParent(context, c);
    // D's own chain: [C, B, A] -- C nearer than B.

    const ProtoSparseList* attrs = d->getAttributes(context);
    EXPECT_EQ(valueOf(attrs, context, shared), context->fromInteger(3))
        << "C is nearer than B in D's own chain; C's value must win";
}

// Exercises the exact ordering divergence documented for setParents vs
// addParent (see addParent's and setParents's header doc comments):
// setParents([c, b]) with b's own ancestor ba and c's own ancestor ca,
// where ba and ca both define `shared` -- setParents places ca AFTER
// b (all listed parents first, then missing ancestors), so if b itself
// also defines `shared`, b's own value wins over ca's.
TEST_F(GetAttributesMergeTest, SetParentsBatchedAncestorOrderMatchesGetAttribute) {
    const ProtoString* shared = sym("shared");

    const ProtoObject* ba = context->newObject(false);
    ba = ba->setAttribute(context, shared, context->fromInteger(1));
    const ProtoObject* b = ba->newChild(context);
    b = b->setAttribute(context, shared, context->fromInteger(30));

    const ProtoObject* ca = context->newObject(false);
    ca = ca->setAttribute(context, shared, context->fromInteger(2));
    const ProtoObject* c = ca->newChild(context);

    const ProtoObject* d = context->newObject(false);
    const ProtoList* plist = context->newList()->appendLast(context, c)->appendLast(context, b);
    d = d->setParents(context, plist);
    // d's own chain (setParents flattening): [c, b, ca, ba].

    const ProtoSparseList* attrs = d->getAttributes(context);
    EXPECT_EQ(valueOf(attrs, context, shared), d->getAttribute(context, shared))
        << "getAttributes' merge must agree with getAttribute's own precedence";
    EXPECT_EQ(valueOf(attrs, context, shared), context->fromInteger(30))
        << "b (listed, position 2) is nearer than ca (an ancestor, appended "
           "after all listed parents), so b's own value wins";
}

// --- Deep chain: no stack-depth concern from the old recursion ------------

TEST_F(GetAttributesMergeTest, DeepSingleParentChainStillMergesTheRootAttribute) {
    const ProtoString* a = sym("a");
    const ProtoObject* root = context->newObject(false);
    root = root->setAttribute(context, a, context->fromInteger(7));
    const ProtoObject* current = root;
    for (int i = 0; i < 1000; ++i) {
        current = current->newChild(context);
    }

    const ProtoSparseList* attrs = current->getAttributes(context);
    EXPECT_EQ(valueOf(attrs, context, a), context->fromInteger(7));
}

// --- Mutable receiver: current snapshot ------------------------------------

TEST_F(GetAttributesMergeTest, MutableReceiverResolvesCurrentSnapshot) {
    const ProtoString* a = sym("a");
    const ProtoObject* base = context->newObject(false);
    base = base->setAttribute(context, a, context->fromInteger(5));

    auto* m = const_cast<ProtoObject*>(context->newObject(true));
    m->addParent(context, base);

    const ProtoSparseList* attrs = m->getAttributes(context);
    EXPECT_EQ(valueOf(attrs, context, a), context->fromInteger(5));
}

// --- Agreement with getAttribute/hasAttribute on a diamond -----------------

TEST_F(GetAttributesMergeTest, AgreesWithGetAttributeAndHasAttributeOnADiamond) {
    const ProtoString* onB = sym("onB");
    const ProtoObject* a = context->newObject(false);
    const ProtoObject* b = a->newChild(context);
    b = b->setAttribute(context, onB, context->fromInteger(9));
    const ProtoObject* c = a->newChild(context);

    const ProtoObject* d = context->newObject(false);
    d = d->addParent(context, b);
    d = d->addParent(context, c);

    const ProtoSparseList* attrs = d->getAttributes(context);
    EXPECT_EQ(valueOf(attrs, context, onB), d->getAttribute(context, onB));
    EXPECT_EQ(contains(attrs, context, onB), d->hasAttribute(context, onB) == PROTO_TRUE);
}
