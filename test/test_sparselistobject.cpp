// test_sparselistobject.cpp — API parity of ProtoSparseListObject with
// ProtoSparseList: Small and AVL forms, the promotion boundary, ordering by
// key word, persistence, isEqual/getHash, processElements/processValues.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <map>
#include <random>
#include <utility>
#include <vector>

using namespace proto;

namespace {
    CellType formOf(const ProtoSparseListObject* m) {
        return reinterpret_cast<const Cell*>(reinterpret_cast<uintptr_t>(m) & ~0x3FUL)->getType();
    }

    using PairVec = std::vector<std::pair<const ProtoObject*, const ProtoObject*>>;
    void collect(ProtoContext*, void* self, const ProtoObject* k, const ProtoObject* v) {
        static_cast<PairVec*>(self)->emplace_back(k, v);
    }
    void collectValue(ProtoContext*, void* self, const ProtoObject* v) {
        static_cast<std::vector<const ProtoObject*>*>(self)->push_back(v);
    }
}

class SparseListObjectTest : public ::testing::Test {
protected:
    ProtoSpace* space;
    ProtoContext* c;
    void SetUp() override { space = new ProtoSpace(); c = space->rootContext; }
    void TearDown() override { delete space; }
    const ProtoObject* I(long v) { return c->fromInteger(v); }
    const ProtoObject* obj() { return c->newObject(false); }
};

TEST_F(SparseListObjectTest, NewIsAnEmptySmallWithTheNewTag) {
    const ProtoSparseListObject* m = c->newSparseListObject();
    ProtoObjectPointer pa{};
    pa.oid = m->asObject(c);
    EXPECT_EQ(pa.op.pointer_tag, static_cast<unsigned long>(POINTER_TAG_SPARSE_LIST_OBJECT));
    EXPECT_EQ(formOf(m), CellType::SparseListObjectSmall);
    EXPECT_EQ(m->getSize(c), 0u);
    EXPECT_FALSE(m->has(c, obj()));
}

TEST_F(SparseListObjectTest, SmallHoldsThreeThenPromotesAndNeverDemotes) {
    const ProtoObject* k[5] = {obj(), obj(), obj(), obj(), obj()};
    const ProtoSparseListObject* m = c->newSparseListObject();
    for (int i = 0; i < 3; ++i) {
        m = m->setAt(c, k[i], I(i));
        EXPECT_EQ(formOf(m), CellType::SparseListObjectSmall) << i;
    }
    m = m->setAt(c, k[3], I(3));
    EXPECT_EQ(formOf(m), CellType::SparseListObject);
    EXPECT_EQ(m->getSize(c), 4u);
    for (int i = 0; i < 4; ++i) EXPECT_EQ(m->getAt(c, k[i]), I(i));
    m = m->removeAt(c, k[0]);
    EXPECT_EQ(formOf(m), CellType::SparseListObject);    // mirrors ProtoSparseList
    EXPECT_EQ(m->getSize(c), 3u);
    EXPECT_FALSE(m->has(c, k[0]));
}

TEST_F(SparseListObjectTest, AbsentIsNullptrAndStoredNoneIsDistinguishable) {
    const ProtoObject* a = obj();
    const ProtoObject* b = obj();
    const ProtoSparseListObject* m = c->newSparseListObject()->setAt(c, a, PROTO_NONE);
    EXPECT_EQ(m->getAt(c, a), PROTO_NONE);
    EXPECT_TRUE(m->has(c, a));
    EXPECT_EQ(m->getAt(c, b), nullptr);
    EXPECT_FALSE(m->has(c, b));
}

TEST_F(SparseListObjectTest, SetAtNullptrValueRemoves) {
    const ProtoObject* a = obj();
    const ProtoSparseListObject* m = c->newSparseListObject()->setAt(c, a, I(1));
    m = m->setAt(c, a, nullptr);
    EXPECT_FALSE(m->has(c, a));
    EXPECT_EQ(m->getSize(c), 0u);
}

// D3 = silent (recommended).  If the maintainer chose "throw", replace the
// body with EXPECT_THROW(... , std::invalid_argument) for setAt/removeAt/
// getAt/has on a nullptr key.
TEST_F(SparseListObjectTest, NullKeyIsRejected) {
    const ProtoSparseListObject* m = c->newSparseListObject()->setAt(c, obj(), I(1));
    EXPECT_FALSE(m->has(c, nullptr));
    EXPECT_EQ(m->getAt(c, nullptr), nullptr);
    EXPECT_EQ(m->setAt(c, nullptr, I(2)), m);
    EXPECT_EQ(m->removeAt(c, nullptr), m);
    EXPECT_EQ(m->getSize(c), 1u);
}

TEST_F(SparseListObjectTest, EmbeddedKeysAreStored) {
    const ProtoObject* keys[] = {I(0), I(7), I(-3), PROTO_TRUE, PROTO_FALSE, PROTO_NONE,
                                 c->fromUnicodeChar(0x263A), c->fromUTF8String("ab")};
    const ProtoSparseListObject* m = c->newSparseListObject();
    long i = 0;
    for (const ProtoObject* k : keys) m = m->setAt(c, k, I(100 + i++));
    EXPECT_EQ(m->getSize(c), 8u);
    i = 0;
    for (const ProtoObject* k : keys) EXPECT_EQ(m->getAt(c, k), I(100 + i++));
}

TEST_F(SparseListObjectTest, ElementsAreVisitedInAscendingKeyWordOrder) {
    const ProtoSparseListObject* m = c->newSparseListObject();
    for (int i = 0; i < 50; ++i) m = m->setAt(c, (i % 2) ? obj() : I(i * 13 - 200), I(i));
    PairVec visited;
    m->processElements(c, &visited, collect);
    ASSERT_EQ(visited.size(), 50u);
    for (size_t i = 1; i < visited.size(); ++i)
        EXPECT_LT(reinterpret_cast<uintptr_t>(visited[i - 1].first), reinterpret_cast<uintptr_t>(visited[i].first));
    std::vector<const ProtoObject*> values;
    m->processValues(c, &values, collectValue);
    ASSERT_EQ(values.size(), visited.size());
    for (size_t i = 0; i < values.size(); ++i) EXPECT_EQ(values[i], visited[i].second);
}

TEST_F(SparseListObjectTest, RandomOperationsMatchAnOrderedModel) {
    std::vector<const ProtoObject*> pool;
    for (int i = 0; i < 40; ++i) pool.push_back(obj());
    for (int i = 0; i < 8; ++i) pool.push_back(I(i * 1000 - 3000));
    pool.push_back(PROTO_TRUE);
    pool.push_back(PROTO_NONE);

    std::mt19937 gen(4242);
    std::uniform_int_distribution<size_t> pick(0, pool.size() - 1);
    std::uniform_int_distribution<int> op(0, 9);
    std::map<uintptr_t, std::pair<const ProtoObject*, const ProtoObject*>> model;
    const ProtoSparseListObject* m = c->newSparseListObject();
    bool promoted = false;

    for (int step = 0; step < 3000; ++step) {
        const ProtoObject* k = pool[pick(gen)];
        if (op(gen) < 7) {
            const ProtoObject* v = I(step);
            m = m->setAt(c, k, v);
            model[reinterpret_cast<uintptr_t>(k)] = {k, v};
            if (model.size() > ProtoSparseListObjectSmallImplementation::MAX_INLINE) promoted = true;
        } else {
            m = m->removeAt(c, k);
            model.erase(reinterpret_cast<uintptr_t>(k));
        }
        ASSERT_EQ(formOf(m), promoted ? CellType::SparseListObject : CellType::SparseListObjectSmall) << step;
        ASSERT_EQ(m->getSize(c), model.size()) << step;
        if (step % 25 == 0) {
            for (const ProtoObject* q : pool) {
                auto it = model.find(reinterpret_cast<uintptr_t>(q));
                ASSERT_EQ(m->has(c, q), it != model.end());
                ASSERT_EQ(m->getAt(c, q), it == model.end() ? nullptr : it->second.second);
            }
            PairVec visited;
            m->processElements(c, &visited, collect);
            PairVec expected;
            for (const auto& [w, kv] : model) expected.push_back(kv);
            ASSERT_EQ(visited, expected);
        }
    }
}

TEST_F(SparseListObjectTest, OldVersionsRemainValidAndUnchanged) {
    const ProtoObject* k[6] = {obj(), obj(), obj(), obj(), obj(), obj()};
    const ProtoSparseListObject* v0 = c->newSparseListObject();
    const ProtoSparseListObject* v1 = v0->setAt(c, k[0], I(0))->setAt(c, k[1], I(1));
    const ProtoSparseListObject* v2 = v1;
    for (int i = 2; i < 6; ++i) v2 = v2->setAt(c, k[i], I(i));   // crosses into AVL
    const ProtoSparseListObject* v3 = v2->setAt(c, k[0], I(99))->removeAt(c, k[5]);

    EXPECT_EQ(v0->getSize(c), 0u);
    EXPECT_EQ(v1->getSize(c), 2u);
    EXPECT_EQ(v1->getAt(c, k[0]), I(0));
    EXPECT_FALSE(v1->has(c, k[2]));
    EXPECT_EQ(v2->getSize(c), 6u);
    EXPECT_EQ(v2->getAt(c, k[0]), I(0));
    EXPECT_EQ(v2->getAt(c, k[5]), I(5));
    EXPECT_EQ(v3->getSize(c), 5u);
    EXPECT_EQ(v3->getAt(c, k[0]), I(99));
    EXPECT_FALSE(v3->has(c, k[5]));
}

// D4 = identity (recommended).  If the maintainer chose compare()==0, add a
// case with two distinct LargeInteger values of equal magnitude.
TEST_F(SparseListObjectTest, IsEqualAndHashIgnoreInsertionOrderAndForm) {
    const ProtoObject* k[5] = {obj(), obj(), obj(), obj(), obj()};
    const ProtoSparseListObject* a = c->newSparseListObject();
    const ProtoSparseListObject* b = c->newSparseListObject();
    for (int i = 0; i < 5; ++i) a = a->setAt(c, k[i], I(i));
    for (int i = 4; i >= 0; --i) b = b->setAt(c, k[i], I(i));
    EXPECT_TRUE(a->isEqual(c, b));
    EXPECT_EQ(a->getHash(c), b->getHash(c));
    EXPECT_FALSE(a->isEqual(c, b->setAt(c, k[2], I(42))));
    EXPECT_FALSE(a->isEqual(c, b->removeAt(c, k[2])));

    const ProtoSparseListObject* avl3 = a->removeAt(c, k[3])->removeAt(c, k[4]);   // AVL, size 3
    const ProtoSparseListObject* small3 = c->newSparseListObject()
        ->setAt(c, k[0], I(0))->setAt(c, k[1], I(1))->setAt(c, k[2], I(2));      // Small, size 3
    ASSERT_EQ(formOf(avl3), CellType::SparseListObject);
    ASSERT_EQ(formOf(small3), CellType::SparseListObjectSmall);
    EXPECT_TRUE(avl3->isEqual(c, small3));
    EXPECT_EQ(avl3->getHash(c), small3->getHash(c));
}
