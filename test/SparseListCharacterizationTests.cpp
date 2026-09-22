// SparseListCharacterizationTests.cpp — locks ProtoSparseList's observable
// behaviour (contents, form, order, iterator output, AVL balance) before its
// algorithms are shared with ProtoMap.  Must pass unchanged
// before and after the refactor.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <map>
#include <random>
#include <utility>
#include <vector>

using namespace proto;

namespace {
    using PairVec = std::vector<std::pair<unsigned long, long long>>;

    void collect(ProtoContext* c, void* self, unsigned long key, const ProtoObject* value) {
        static_cast<PairVec*>(self)->emplace_back(key, value->asLong(c));
    }

    bool isSmallForm(const ProtoSparseList* sl) {
        ProtoObjectPointer pa{};
        pa.oid = reinterpret_cast<const ProtoObject*>(sl);
        return pa.op.pointer_tag == POINTER_TAG_SPARSE_LIST_SMALL;
    }

    PairVec viaIterator(ProtoContext* c, const ProtoSparseList* sl) {
        PairVec out;
        const ProtoSparseListIterator* it = sl->getIterator(c);
        while (it && it->hasNext(c)) {
            out.emplace_back(it->nextKey(c), it->nextValue(c)->asLong(c));
            it = const_cast<ProtoSparseListIterator*>(it)->advance(c);
        }
        return out;
    }
}

TEST(SparseListCharacterization, RandomOperationsMatchAnOrderedModel) {
    ProtoSpace space;
    ProtoContext* c = space.rootContext;
    std::mt19937 gen(20260922);
    std::uniform_int_distribution<unsigned long> keyDist(0, 40);
    std::uniform_int_distribution<int> opDist(0, 9);

    std::map<unsigned long, long long> model;
    const ProtoSparseList* sl = c->newSparseList();
    bool promoted = false;   // Small never demotes once it became AVL

    for (int step = 0; step < 4000; ++step) {
        const unsigned long k = keyDist(gen);
        if (opDist(gen) < 7) {
            sl = sl->setAt(c, k, c->fromInteger(step));
            model[k] = step;
            if (model.size() > ProtoSparseListSmallImplementation::MAX_INLINE || k == 0) promoted = true;
        } else {
            sl = sl->removeAt(c, k);
            model.erase(k);
        }
        ASSERT_EQ(isSmallForm(sl), !promoted) << "step " << step;
        ASSERT_EQ(sl->getSize(c), model.size()) << "step " << step;

        if (step % 50 == 0) {
            for (unsigned long q = 0; q <= 40; ++q) {
                auto it = model.find(q);
                ASSERT_EQ(sl->has(c, q), it != model.end()) << "key " << q;
                const ProtoObject* v = sl->getAt(c, q);
                if (it == model.end()) ASSERT_EQ(v, PROTO_NONE);
                else ASSERT_EQ(v->asLong(c), it->second);
            }
            PairVec expected(model.begin(), model.end());
            PairVec visited;
            sl->processElements(c, &visited, collect);
            ASSERT_EQ(visited, expected);
            ASSERT_EQ(viaIterator(c, sl), expected);
        }
    }
}

TEST(SparseListCharacterization, SequentialInsertionStaysBalanced) {
    ProtoSpace space;
    ProtoContext* c = space.rootContext;
    const ProtoSparseList* sl = c->newSparseList();
    for (unsigned long k = 1; k <= 10000; ++k) sl = sl->setAt(c, k, c->fromInteger(k));
    ASSERT_FALSE(isSmallForm(sl));
    const auto* root = toImpl<const ProtoSparseListImplementation>(sl);
    EXPECT_EQ(root->size, 10000u);
    EXPECT_LE(root->height, 20u);   // AVL bound: 1.44 * log2(10002) ≈ 19.1
    for (unsigned long k = 1; k <= 10000; k += 2) sl = sl->removeAt(c, k);
    EXPECT_EQ(sl->getSize(c), 5000u);
    EXPECT_LE(toImpl<const ProtoSparseListImplementation>(sl)->height, 20u);
}

TEST(SparseListCharacterization, SetAtNullptrRemovesAndSameRootValueKeepsAvlHandle) {
    ProtoSpace space;
    ProtoContext* c = space.rootContext;
    const ProtoSparseList* sl = c->newSparseList();
    for (unsigned long k = 1; k <= 8; ++k) sl = sl->setAt(c, k, c->fromInteger(k));
    // AVL: re-storing the root's own value returns the very same tree.  For a
    // non-root key the unchanged leaf is returned, but every ancestor is
    // rebuilt, so only the contents (not the handle) are preserved there.
    ASSERT_FALSE(isSmallForm(sl));
    const unsigned long rootKey = toImpl<const ProtoSparseListImplementation>(sl)->key;
    EXPECT_EQ(sl->setAt(c, rootKey, sl->getAt(c, rootKey)), sl);
    const ProtoObject* five = sl->getAt(c, 5);
    const ProtoSparseList* same = sl->setAt(c, 5, five);
    EXPECT_EQ(same->getSize(c), 8u);
    EXPECT_EQ(same->getAt(c, 5), five);
    const ProtoSparseList* removed = sl->setAt(c, 5, nullptr);
    EXPECT_FALSE(removed->has(c, 5));
    EXPECT_EQ(removed->getSize(c), 7u);
    EXPECT_TRUE(sl->has(c, 5));                      // old version intact
}
