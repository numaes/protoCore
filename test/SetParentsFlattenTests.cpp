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
#include <atomic>
#include <stdexcept>
#include <thread>
#include <vector>

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
    const ProtoObject* priorParent = context->newObject(false);
    a->setParents(context, context->newList()->appendLast(context, priorParent));

    const ProtoList* selfList = context->newList()->appendLast(context, a);
    EXPECT_THROW(a->setParents(context, selfList), std::invalid_argument);

    // The receiver is left exactly as it was before the failed call: the
    // exception is thrown while computing the flattened order, before the
    // critical section that builds the chain even opens, so nothing was
    // ever published to a's mutable shard.
    expectChainOrder(a, {priorParent});
    EXPECT_EQ(a->hasParent(context, priorParent), 1);
    EXPECT_EQ(a->hasParent(context, a), 1) << "trivial self-case, unaffected either way";
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

// --- I3: setParents' ordering differs from addParent's ---------------------
//
// setParents appends ALL missing ancestors AFTER ALL listed parents;
// addParent, called once per parent, interleaves each call's own missing
// ancestors immediately after that call's parent. Demonstrated here with
// the exact shape documented in addParent's header doc comment: b has its
// own ancestor ba, c has its own ancestor ca, neither ba nor ca is listed
// directly. Both ba and ca define the SAME attribute name with DIFFERENT
// values, so the ordering difference is directly observable as an
// attribute-precedence difference, not just a getParents() order
// difference.
TEST_F(SetParentsFlattenTest, OrderingDiffersFromAddParentAndAffectsAttributePrecedence) {
    const ProtoString* sharedAttr = sym("shared");

    const ProtoObject* ba = context->newObject(false);
    ba = ba->setAttribute(context, sharedAttr, context->fromInteger(1));
    const ProtoObject* b = ba->newChild(context); // b.chain = [ba]

    const ProtoObject* ca = context->newObject(false);
    ca = ca->setAttribute(context, sharedAttr, context->fromInteger(2));
    const ProtoObject* c = ca->newChild(context); // c.chain = [ca]

    // addParent, called once per parent: d.addParent(b); d.addParent(c);
    // interleaves each call's own ancestor right after that call's parent:
    // [c, ca, b, ba] — ca (attribute value 2) appears BEFORE b.
    const ProtoObject* dAdd = context->newObject(false);
    const ProtoObject* dAddB = dAdd->addParent(context, b);
    const ProtoObject* dAddFinal = dAddB->addParent(context, c);
    expectChainOrder(dAddFinal, {c, ca, b, ba});
    EXPECT_EQ(dAddFinal->getAttribute(context, sharedAttr), context->fromInteger(2))
        << "addParent: ca (from the LAST-added parent, c) shadows ba";

    // setParents with the SAME listed parents, in the SAME order [c, b]:
    // appends ALL listed parents first, THEN all missing ancestors, in
    // listed-parent order: [c, b, ca, ba] — ca now appears AFTER b.
    const ProtoObject* dSetBase = context->newObject(false);
    const ProtoList* plist = context->newList()->appendLast(context, c)->appendLast(context, b);
    const ProtoObject* dSetFinal = dSetBase->setParents(context, plist);
    expectChainOrder(dSetFinal, {c, b, ca, ba});
    EXPECT_EQ(dSetFinal->getAttribute(context, sharedAttr), context->fromInteger(2))
        << "setParents: c (listed first) still wins directly, but via a "
           "different chain shape than addParent's";

    // A case where the difference actually flips the winning value: c's
    // own ancestor ca would, under addParent's interleaving, be checked
    // BEFORE b -- but under setParents it is checked AFTER b, so if b
    // ALSO defined `shared`, addParent and setParents would disagree on
    // the result. Demonstrate directly: b itself (not ba) defines shared.
    const ProtoObject* bWithAttr = ba->newChild(context);
    const_cast<ProtoObject*>(bWithAttr); // still immutable; rebuild below
    const ProtoObject* bSelf = ba->newChild(context);
    bSelf = bSelf->setAttribute(context, sharedAttr, context->fromInteger(3));

    const ProtoObject* dAdd2 = context->newObject(false);
    dAdd2 = dAdd2->addParent(context, bSelf);
    dAdd2 = dAdd2->addParent(context, c);
    // addParent order: [c, ca, bSelf, ba] -- c itself has no OWN
    // "shared" attribute, so lookup falls through to ca (value 2) before
    // ever reaching bSelf's own "shared" (value 3).
    EXPECT_EQ(dAdd2->getAttribute(context, sharedAttr), context->fromInteger(2));

    const ProtoObject* dSet2Base = context->newObject(false);
    const ProtoList* plist2 = context->newList()->appendLast(context, c)->appendLast(context, bSelf);
    const ProtoObject* dSet2 = dSet2Base->setParents(context, plist2);
    // setParents order: [c, bSelf, ca, ba] -- bSelf (listed, position 2)
    // is checked BEFORE ca (an ancestor, appended after all listed
    // parents), so bSelf's own "shared" (value 3) wins instead.
    EXPECT_EQ(dSet2->getAttribute(context, sharedAttr), context->fromInteger(3))
        << "setParents and addParent disagree on which value wins here -- "
           "exactly the documented ordering difference";
}

// --- I6: concurrency -----------------------------------------------------
//
// Several threads call setParents on ONE shared mutable object, each with
// its own, mutually-unrelated single parent candidate (so none of these
// calls is ever a genuine cycle), while another thread concurrently reads
// getParents/isInstanceOf/hasParent. Nothing must crash, no read may ever
// observe a torn/partial chain (more than one entry, or an entry that is
// not one of the candidates), and no writer may see a spurious
// std::invalid_argument (a "cycle" false positive) — these candidates
// share no ancestry with each other or with the receiver.
TEST_F(SetParentsFlattenTest, ConcurrentSetParentsWithConcurrentReadsIsSafe) {
    constexpr int kThreads = 4;
    constexpr int kIterPerThread = 1500;

    auto* m = const_cast<ProtoObject*>(context->newObject(true));

    std::vector<const ProtoObject*> candidates;
    for (int t = 0; t < kThreads; ++t) {
        candidates.push_back(context->newObject(false));
    }

    std::atomic<bool> stop{false};
    std::atomic<int> unexpectedThrows{0};
    std::atomic<int> tornReads{0};

    std::thread reader([&]() {
        // Each check below is a SINGLE call: it observes m's state at one
        // instant. Two SEPARATE calls (e.g. getParents() then hasParent())
        // can legitimately observe DIFFERENT states under a concurrent
        // writer -- that is not tearing, just an ordinary lock-free race,
        // and comparing them would be a false positive in the TEST, not a
        // bug in the code. What must never happen, from any SINGLE call,
        // is a corrupted/partial result: getParents() returning more than
        // one entry (only ever one candidate is ever set at a time) or an
        // entry that is not one of the known candidates; isInstanceOf
        // returning anything other than PROTO_TRUE/PROTO_NONE; or a crash.
        ProtoContext readerCtx{space};
        int rotate = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            const ProtoList* parents = m->getParents(&readerCtx);
            long size = parents->getSize(&readerCtx);
            if (size > 1) {
                tornReads.fetch_add(1, std::memory_order_relaxed);
            } else if (size == 1) {
                const ProtoObject* p = parents->getAt(&readerCtx, 0);
                bool matches = false;
                for (const ProtoObject* c : candidates) {
                    if (c == p) { matches = true; break; }
                }
                if (!matches) tornReads.fetch_add(1, std::memory_order_relaxed);
            }

            const ProtoObject* target = candidates[rotate % candidates.size()];
            ++rotate;
            const ProtoObject* result = m->isInstanceOf(&readerCtx, target);
            if (result != PROTO_TRUE && result != PROTO_NONE) {
                tornReads.fetch_add(1, std::memory_order_relaxed);
            }
            int has = m->hasParent(&readerCtx, target);
            if (has != 0 && has != 1) {
                tornReads.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t]() {
            ProtoContext threadCtx{space};
            const ProtoList* singleParent = threadCtx.newList()->appendLast(&threadCtx, candidates[t]);
            for (int i = 0; i < kIterPerThread; ++i) {
                try {
                    m->setParents(&threadCtx, singleParent);
                } catch (const std::invalid_argument&) {
                    unexpectedThrows.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& th : workers) th.join();
    stop.store(true, std::memory_order_relaxed);
    reader.join();

    EXPECT_EQ(unexpectedThrows.load(), 0)
        << "none of these candidates share any ancestry; no call here is a real cycle";
    EXPECT_EQ(tornReads.load(), 0);

    // Final state: exactly one of the candidates, whichever write landed last.
    const ProtoList* finalParents = m->getParents(context);
    ASSERT_EQ(finalParents->getSize(context), 1);
    const ProtoObject* finalParent = finalParents->getAt(context, 0);
    bool finalMatches = false;
    for (const ProtoObject* c : candidates) {
        if (c == finalParent) { finalMatches = true; break; }
    }
    EXPECT_TRUE(finalMatches);
}
