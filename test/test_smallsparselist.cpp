#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

using namespace proto;

namespace {
    constexpr unsigned MAX_SMALL = ProtoSparseListSmallImplementation::MAX_INLINE;

    bool isSmall(const ProtoSparseList* sl) {
        ProtoObjectPointer pa{};
        pa.oid = reinterpret_cast<const ProtoObject*>(sl);
        return pa.op.pointer_tag == POINTER_TAG_SPARSE_LIST_SMALL;
    }

    bool isAvl(const ProtoSparseList* sl) {
        ProtoObjectPointer pa{};
        pa.oid = reinterpret_cast<const ProtoObject*>(sl);
        return pa.op.pointer_tag == POINTER_TAG_SPARSE_LIST;
    }
}

class SmallSparseListTest : public ::testing::Test {
protected:
    ProtoSpace* space;
    ProtoContext* context;

    void SetUp() override {
        space = new ProtoSpace();
        context = space->rootContext;
    }
    void TearDown() override { delete space; }

    const ProtoObject* I(long v) { return context->fromInteger(v); }
};

// -----------------------------------------------------------------------------
// Construction & form preservation
// -----------------------------------------------------------------------------

TEST_F(SmallSparseListTest, NewSparseListReturnsEmptySmall) {
    const ProtoSparseList* sl = context->newSparseList();
    EXPECT_TRUE(isSmall(sl));
    EXPECT_EQ(sl->getSize(context), 0u);
    EXPECT_FALSE(sl->has(context, 1));
    EXPECT_FALSE(sl->has(context, 100));
}

TEST_F(SmallSparseListTest, SetSingleEntryStaysSmall) {
    const ProtoSparseList* sl = context->newSparseList();
    sl = sl->setAt(context, 17, I(42));
    EXPECT_TRUE(isSmall(sl));
    EXPECT_EQ(sl->getSize(context), 1u);
    EXPECT_TRUE(sl->has(context, 17));
    EXPECT_EQ(sl->getAt(context, 17), I(42));
    EXPECT_FALSE(sl->has(context, 99));
}

TEST_F(SmallSparseListTest, FillToMaxStaysSmall) {
    const ProtoSparseList* sl = context->newSparseList();
    sl = sl->setAt(context, 1, I(10));
    sl = sl->setAt(context, 2, I(20));
    sl = sl->setAt(context, 3, I(30));
    EXPECT_TRUE(isSmall(sl));
    EXPECT_EQ(sl->getSize(context), MAX_SMALL);
    EXPECT_EQ(sl->getAt(context, 1), I(10));
    EXPECT_EQ(sl->getAt(context, 2), I(20));
    EXPECT_EQ(sl->getAt(context, 3), I(30));
}

TEST_F(SmallSparseListTest, OverflowPromotesToAvl) {
    const ProtoSparseList* sl = context->newSparseList();
    sl = sl->setAt(context, 1, I(10));
    sl = sl->setAt(context, 2, I(20));
    sl = sl->setAt(context, 3, I(30));
    EXPECT_TRUE(isSmall(sl));
    sl = sl->setAt(context, 4, I(40));   // overflow
    EXPECT_TRUE(isAvl(sl));
    EXPECT_EQ(sl->getSize(context), 4u);
    EXPECT_EQ(sl->getAt(context, 1), I(10));
    EXPECT_EQ(sl->getAt(context, 2), I(20));
    EXPECT_EQ(sl->getAt(context, 3), I(30));
    EXPECT_EQ(sl->getAt(context, 4), I(40));
}

TEST_F(SmallSparseListTest, KeyZeroPromotesToAvl) {
    // Small uses key 0 as the empty-slot sentinel; storing key 0 must
    // route through the AVL form which has no such reservation.
    const ProtoSparseList* sl = context->newSparseList();
    sl = sl->setAt(context, 0, I(99));
    EXPECT_TRUE(isAvl(sl));
    EXPECT_EQ(sl->getSize(context), 1u);
    EXPECT_TRUE(sl->has(context, 0));
    EXPECT_EQ(sl->getAt(context, 0), I(99));
}

TEST_F(SmallSparseListTest, KeyZeroPromotesEvenWithExistingSmall) {
    // Existing Small with 1-2 entries + setAt(0, ...) → AVL containing
    // both the old entries and the new key=0 binding.
    const ProtoSparseList* sl = context->newSparseList();
    sl = sl->setAt(context, 5, I(50));
    sl = sl->setAt(context, 7, I(70));
    EXPECT_TRUE(isSmall(sl));
    sl = sl->setAt(context, 0, I(0));
    EXPECT_TRUE(isAvl(sl));
    EXPECT_EQ(sl->getSize(context), 3u);
    EXPECT_EQ(sl->getAt(context, 0), I(0));
    EXPECT_EQ(sl->getAt(context, 5), I(50));
    EXPECT_EQ(sl->getAt(context, 7), I(70));
}

TEST_F(SmallSparseListTest, ReplaceExistingKeyStaysSmall) {
    const ProtoSparseList* sl = context->newSparseList();
    sl = sl->setAt(context, 5, I(50));
    sl = sl->setAt(context, 5, I(51));
    EXPECT_TRUE(isSmall(sl));
    EXPECT_EQ(sl->getSize(context), 1u);
    EXPECT_EQ(sl->getAt(context, 5), I(51));
}

// -----------------------------------------------------------------------------
// removeAt — Small never promotes; AVL never degrades
// -----------------------------------------------------------------------------

TEST_F(SmallSparseListTest, RemoveFromSmallStaysSmall) {
    const ProtoSparseList* sl = context->newSparseList();
    sl = sl->setAt(context, 1, I(10));
    sl = sl->setAt(context, 2, I(20));
    sl = sl->setAt(context, 3, I(30));
    sl = sl->removeAt(context, 2);
    EXPECT_TRUE(isSmall(sl));
    EXPECT_EQ(sl->getSize(context), 2u);
    EXPECT_TRUE(sl->has(context, 1));
    EXPECT_FALSE(sl->has(context, 2));
    EXPECT_TRUE(sl->has(context, 3));
}

TEST_F(SmallSparseListTest, RemoveAllFromSmallEndsEmptySmall) {
    const ProtoSparseList* sl = context->newSparseList();
    sl = sl->setAt(context, 7, I(70));
    sl = sl->removeAt(context, 7);
    EXPECT_TRUE(isSmall(sl));
    EXPECT_EQ(sl->getSize(context), 0u);
}

TEST_F(SmallSparseListTest, RemoveFromAvlStaysAvl) {
    // Build an AVL by overflowing the Small, then remove until size 1
    // — the result must STAY AVL (no auto-degradation per design).
    const ProtoSparseList* sl = context->newSparseList();
    for (int i = 1; i <= 5; ++i) sl = sl->setAt(context, i, I(i * 10));
    EXPECT_TRUE(isAvl(sl));
    sl = sl->removeAt(context, 1);
    sl = sl->removeAt(context, 2);
    sl = sl->removeAt(context, 3);
    sl = sl->removeAt(context, 4);
    EXPECT_TRUE(isAvl(sl));        // no degradation
    EXPECT_EQ(sl->getSize(context), 1u);
    EXPECT_TRUE(sl->has(context, 5));
}

TEST_F(SmallSparseListTest, RemoveMissingKeyIsNoOp) {
    const ProtoSparseList* sl = context->newSparseList();
    sl = sl->setAt(context, 1, I(10));
    sl = sl->removeAt(context, 999);
    EXPECT_TRUE(isSmall(sl));
    EXPECT_EQ(sl->getSize(context), 1u);
    EXPECT_TRUE(sl->has(context, 1));
}

// -----------------------------------------------------------------------------
// Equivalence with AVL form for reads
// -----------------------------------------------------------------------------

TEST_F(SmallSparseListTest, SmallReadsMatchAvlReads) {
    // Build the same content in both forms; reads must produce identical
    // results.  Force AVL by adding 4+ entries then removing 1 to leave
    // 3 entries in AVL form.
    const ProtoSparseList* small = context->newSparseList();
    small = small->setAt(context, 7,  I(70));
    small = small->setAt(context, 13, I(130));
    small = small->setAt(context, 21, I(210));
    EXPECT_TRUE(isSmall(small));

    const ProtoSparseList* avl = context->newSparseList();
    avl = avl->setAt(context, 7,  I(70));
    avl = avl->setAt(context, 13, I(130));
    avl = avl->setAt(context, 21, I(210));
    avl = avl->setAt(context, 99, I(990));    // overflow → AVL
    avl = avl->removeAt(context, 99);          // back to size 3, still AVL
    EXPECT_TRUE(isAvl(avl));
    EXPECT_EQ(avl->getSize(context), 3u);

    EXPECT_EQ(small->getSize(context), avl->getSize(context));
    for (unsigned long k : {7ul, 13ul, 21ul, 100ul}) {
        EXPECT_EQ(small->has(context, k),     avl->has(context, k))     << "key=" << k;
        EXPECT_EQ(small->getAt(context, k),  avl->getAt(context, k))   << "key=" << k;
    }
}

// -----------------------------------------------------------------------------
// Iterator parity (key-ascending order)
// -----------------------------------------------------------------------------

TEST_F(SmallSparseListTest, IteratorYieldsKeyAscOrder) {
    const ProtoSparseList* sl = context->newSparseList();
    sl = sl->setAt(context, 21, I(210));   // insert out of order
    sl = sl->setAt(context, 7,  I(70));
    sl = sl->setAt(context, 13, I(130));
    EXPECT_TRUE(isSmall(sl));

    const ProtoSparseListIterator* it = sl->getIterator(context);
    std::vector<unsigned long> keys;
    while (it && it->hasNext(context)) {
        keys.push_back(it->nextKey(context));
        it = const_cast<ProtoSparseListIterator*>(it)->advance(context);
    }
    ASSERT_EQ(keys.size(), 3u);
    EXPECT_EQ(keys[0], 7u);
    EXPECT_EQ(keys[1], 13u);
    EXPECT_EQ(keys[2], 21u);
}

TEST_F(SmallSparseListTest, ProcessElementsVisitsAllPairs) {
    const ProtoSparseList* sl = context->newSparseList();
    sl = sl->setAt(context, 5, I(50));
    sl = sl->setAt(context, 1, I(10));
    sl = sl->setAt(context, 9, I(90));

    struct Acc {
        std::vector<std::pair<unsigned long, long>> pairs;
        ProtoContext* ctx;
    } acc { {}, context };

    sl->processElements(context, &acc,
        [](ProtoContext* c, void* self, unsigned long k, const ProtoObject* v) {
            auto* a = static_cast<Acc*>(self);
            a->pairs.emplace_back(k, v->asLong(c));
        });

    ASSERT_EQ(acc.pairs.size(), 3u);
    // Key-ascending order regardless of insertion order.
    EXPECT_EQ(acc.pairs[0].first, 1u);   EXPECT_EQ(acc.pairs[0].second, 10);
    EXPECT_EQ(acc.pairs[1].first, 5u);   EXPECT_EQ(acc.pairs[1].second, 50);
    EXPECT_EQ(acc.pairs[2].first, 9u);   EXPECT_EQ(acc.pairs[2].second, 90);
}

// -----------------------------------------------------------------------------
// GC stress — Small must survive a forced sweep with values intact
// -----------------------------------------------------------------------------

TEST_F(SmallSparseListTest, SurvivesForcedCollection) {
    const ProtoSparseList* sl = context->newSparseList();
    sl = sl->setAt(context, 11, I(111));
    sl = sl->setAt(context, 22, I(222));
    sl = sl->setAt(context, 33, I(333));

    // Allocate and discard a large amount to provoke a sweep.  `sl` is
    // held in a C++ local; the GC walk reaches it via the test fixture's
    // root scan because ProtoContext keeps live frames alive — when the
    // test fixture's root is the rootContext, that should pin it.  Drop
    // through the standard cooperative safepoint to give the GC a chance.
    for (int i = 0; i < 4096; ++i) {
        const ProtoObject* dead = context->fromInteger(i);
        (void) dead;
    }
    context->safepoint();

    EXPECT_EQ(sl->getSize(context), 3u);
    EXPECT_EQ(sl->getAt(context, 11), I(111));
    EXPECT_EQ(sl->getAt(context, 22), I(222));
    EXPECT_EQ(sl->getAt(context, 33), I(333));
}

// -----------------------------------------------------------------------------
// Empty-small via setAt(k, nullptr) — same contract as removeAt
// -----------------------------------------------------------------------------

TEST_F(SmallSparseListTest, SetAtNullValueRemovesEntry) {
    const ProtoSparseList* sl = context->newSparseList();
    sl = sl->setAt(context, 5, I(50));
    sl = sl->setAt(context, 5, nullptr);
    EXPECT_TRUE(isSmall(sl));
    EXPECT_EQ(sl->getSize(context), 0u);
    EXPECT_FALSE(sl->has(context, 5));
}
