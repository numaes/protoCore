#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

using namespace proto;

namespace {
    constexpr unsigned MAX = ProtoListSmallImplementation::MAX_INLINE;

    bool isSmallList(const ProtoList* l) {
        ProtoObjectPointer pa{}; pa.oid = reinterpret_cast<const ProtoObject*>(l);
        return pa.op.pointer_tag == POINTER_TAG_LIST_SMALL;
    }

    bool isAvlList(const ProtoList* l) {
        ProtoObjectPointer pa{}; pa.oid = reinterpret_cast<const ProtoObject*>(l);
        return pa.op.pointer_tag == POINTER_TAG_LIST;
    }
}

class SmallListTest : public ::testing::Test {
protected:
    ProtoSpace* space;
    ProtoContext* context;

    void SetUp() override {
        space = new ProtoSpace();
        context = space->rootContext;
    }
    void TearDown() override { delete space; }

    const ProtoList* small(std::initializer_list<int> ints) {
        std::vector<const ProtoObject*> items;
        for (int v : ints) items.push_back(context->fromInteger(v));
        return context->newList(items.size(), items.data());
    }

    const ProtoList* avl(std::initializer_list<int> ints) {
        const ProtoList* l = context->newList();
        for (int v : ints) l = l->appendLast(context, context->fromInteger(v));
        return l;
    }
};

TEST_F(SmallListTest, EmptySmall) {
    const ProtoList* l = context->newList(0, nullptr);
    ASSERT_TRUE(isSmallList(l));
    ASSERT_EQ(l->getSize(context), 0u);
}

TEST_F(SmallListTest, SizesZeroToFive) {
    for (unsigned n = 0; n <= MAX; ++n) {
        std::vector<const ProtoObject*> items;
        for (unsigned i = 0; i < n; ++i) items.push_back(context->fromInteger(i + 100));
        const ProtoList* l = context->newList(n, items.data());
        ASSERT_TRUE(isSmallList(l));
        ASSERT_EQ(l->getSize(context), n);
        for (unsigned i = 0; i < n; ++i) {
            ASSERT_EQ(l->getAt(context, i)->asLong(context), 100 + (long)i);
        }
    }
}

TEST_F(SmallListTest, OverflowFallsBackToAvl) {
    std::vector<const ProtoObject*> items;
    for (unsigned i = 0; i < MAX + 1; ++i) items.push_back(context->fromInteger(i));
    const ProtoList* l = context->newList(MAX + 1, items.data());
    ASSERT_TRUE(isAvlList(l)) << "newSmallListN with N>MAX must fall back to AVL";
    ASSERT_EQ(l->getSize(context), MAX + 1);
    for (unsigned i = 0; i < MAX + 1; ++i) {
        ASSERT_EQ(l->getAt(context, i)->asLong(context), (long)i);
    }
}

TEST_F(SmallListTest, GetFirstLast) {
    const ProtoList* l = small({10, 20, 30});
    ASSERT_EQ(l->getFirst(context)->asLong(context), 10);
    ASSERT_EQ(l->getLast(context)->asLong(context), 30);
}

TEST_F(SmallListTest, Has) {
    const ProtoList* l = small({1, 2, 3});
    ASSERT_TRUE(l->has(context, context->fromInteger(2)));
    ASSERT_FALSE(l->has(context, context->fromInteger(99)));
}

TEST_F(SmallListTest, AppendLastStaysSmallUntilFull) {
    const ProtoList* l = small({1, 2});
    ASSERT_TRUE(isSmallList(l));
    l = l->appendLast(context, context->fromInteger(3));
    ASSERT_TRUE(isSmallList(l));
    ASSERT_EQ(l->getSize(context), 3u);
    l = l->appendLast(context, context->fromInteger(4));
    ASSERT_TRUE(isSmallList(l));
    l = l->appendLast(context, context->fromInteger(5));
    ASSERT_TRUE(isSmallList(l)) << "size=5 should still be SmallList";
    ASSERT_EQ(l->getSize(context), MAX);
    // Now overflow.
    l = l->appendLast(context, context->fromInteger(6));
    ASSERT_TRUE(isAvlList(l)) << "size=6 must promote to AVL";
    ASSERT_EQ(l->getSize(context), MAX + 1);
    for (unsigned i = 0; i < MAX + 1; ++i) {
        ASSERT_EQ(l->getAt(context, i)->asLong(context), (long)(i + 1));
    }
}

TEST_F(SmallListTest, SetAtKeepsSmall) {
    const ProtoList* l = small({1, 2, 3});
    const ProtoList* r = l->setAt(context, 1, context->fromInteger(99));
    ASSERT_TRUE(isSmallList(r));
    ASSERT_EQ(r->getSize(context), 3u);
    ASSERT_EQ(r->getAt(context, 0)->asLong(context), 1);
    ASSERT_EQ(r->getAt(context, 1)->asLong(context), 99);
    ASSERT_EQ(r->getAt(context, 2)->asLong(context), 3);
    // Original unchanged
    ASSERT_EQ(l->getAt(context, 1)->asLong(context), 2);
}

TEST_F(SmallListTest, InsertAtKeepsSmallUntilOverflow) {
    const ProtoList* l = small({1, 2});
    const ProtoList* r = l->insertAt(context, 1, context->fromInteger(99));
    ASSERT_TRUE(isSmallList(r));
    ASSERT_EQ(r->getSize(context), 3u);
    ASSERT_EQ(r->getAt(context, 0)->asLong(context), 1);
    ASSERT_EQ(r->getAt(context, 1)->asLong(context), 99);
    ASSERT_EQ(r->getAt(context, 2)->asLong(context), 2);

    // Fill to MAX, then insert one more → AVL.
    const ProtoList* full = small({1, 2, 3, 4, 5});
    const ProtoList* over = full->insertAt(context, 2, context->fromInteger(99));
    ASSERT_TRUE(isAvlList(over));
    ASSERT_EQ(over->getSize(context), 6u);
    ASSERT_EQ(over->getAt(context, 0)->asLong(context), 1);
    ASSERT_EQ(over->getAt(context, 1)->asLong(context), 2);
    ASSERT_EQ(over->getAt(context, 2)->asLong(context), 99);
    ASSERT_EQ(over->getAt(context, 3)->asLong(context), 3);
    ASSERT_EQ(over->getAt(context, 5)->asLong(context), 5);
}

TEST_F(SmallListTest, RemoveAtKeepsSmall) {
    const ProtoList* l = small({1, 2, 3, 4, 5});
    const ProtoList* r = l->removeAt(context, 2);
    ASSERT_TRUE(isSmallList(r));
    ASSERT_EQ(r->getSize(context), 4u);
    ASSERT_EQ(r->getAt(context, 0)->asLong(context), 1);
    ASSERT_EQ(r->getAt(context, 1)->asLong(context), 2);
    ASSERT_EQ(r->getAt(context, 2)->asLong(context), 4);
    ASSERT_EQ(r->getAt(context, 3)->asLong(context), 5);
}

TEST_F(SmallListTest, IteratorRoundTrip) {
    const ProtoList* l = small({10, 20, 30});
    const ProtoListIterator* it = l->getIterator(context);
    ASSERT_TRUE(it->hasNext(context));
    ASSERT_EQ(it->next(context)->asLong(context), 10);
    it = it->advance(context);
    ASSERT_EQ(it->next(context)->asLong(context), 20);
    it = it->advance(context);
    ASSERT_EQ(it->next(context)->asLong(context), 30);
    it = it->advance(context);
    ASSERT_FALSE(it->hasNext(context));
}

TEST_F(SmallListTest, IteratorOverEmpty) {
    const ProtoList* l = context->newList(0, nullptr);
    const ProtoListIterator* it = l->getIterator(context);
    ASSERT_FALSE(it->hasNext(context));
}

TEST_F(SmallListTest, ExtendCrossingForms) {
    const ProtoList* a = small({1, 2});
    const ProtoList* b = small({3, 4});
    const ProtoList* c = a->extend(context, b);
    // 4 elements still inline.
    ASSERT_TRUE(isSmallList(c));
    ASSERT_EQ(c->getSize(context), 4u);
    ASSERT_EQ(c->getAt(context, 3)->asLong(context), 4);

    const ProtoList* big = small({1, 2, 3, 4});
    const ProtoList* over = big->extend(context, b);
    ASSERT_TRUE(isAvlList(over));
    ASSERT_EQ(over->getSize(context), 6u);
    ASSERT_EQ(over->getAt(context, 5)->asLong(context), 4);
}

TEST_F(SmallListTest, AsObjectAndAsListRoundTrip) {
    const ProtoList* l = small({1, 2, 3});
    const ProtoObject* obj = l->asObject(context);
    const ProtoList* back = obj->asList(context);
    ASSERT_EQ(back->getSize(context), 3u);
    ASSERT_TRUE(isSmallList(back));
}

TEST_F(SmallListTest, TupleFromSmallList) {
    const ProtoList* l = small({7, 8, 9});
    const ProtoTuple* t = context->newTupleFromList(l);
    ASSERT_NE(t, nullptr);
    ASSERT_EQ(t->getSize(context), 3u);
    ASSERT_EQ(t->getAt(context, 0)->asLong(context), 7);
    ASSERT_EQ(t->getAt(context, 1)->asLong(context), 8);
    ASSERT_EQ(t->getAt(context, 2)->asLong(context), 9);
}

TEST_F(SmallListTest, GcStressViaRootSet) {
    // Pin a SmallList via a ProtoRootSet (documented transient-pin
    // mechanism), trigger GC, verify slots survive.  This exercises
    // ProtoListSmallImplementation::processReferences end-to-end.
    auto* roots = space->createRootSet("smalllist-test");
    const ProtoList* l = small({100, 200, 300});
    auto handle = roots->add(l->asObject(context));
    space->triggerGC();
    const ProtoObject* recovered = roots->resolve(handle);
    ASSERT_NE(recovered, nullptr);
    const ProtoList* lr = recovered->asList(context);
    ASSERT_TRUE(isSmallList(lr));
    ASSERT_EQ(lr->getSize(context), 3u);
    ASSERT_EQ(lr->getAt(context, 0)->asLong(context), 100);
    ASSERT_EQ(lr->getAt(context, 1)->asLong(context), 200);
    ASSERT_EQ(lr->getAt(context, 2)->asLong(context), 300);
    roots->remove(handle);
    space->destroyRootSet(roots);
}
