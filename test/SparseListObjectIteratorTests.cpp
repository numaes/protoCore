// SparseListObjectIteratorTests.cpp — ascending key-word iteration over a
// ProtoSparseListObject version, both in Small form (promoted internally)
// and AVL form.  D1 = (a) (PSLO-SPEC §7): the iterator handle is an
// unboxed C++ pointer, never a ProtoObject word.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <utility>
#include <vector>

using namespace proto;

namespace {
    using PairVec = std::vector<std::pair<const ProtoObject*, const ProtoObject*>>;
    void collect(ProtoContext*, void* self, const ProtoObject* k, const ProtoObject* v) {
        static_cast<PairVec*>(self)->emplace_back(k, v);
    }
    PairVec drain(ProtoContext* c, const ProtoSparseListObject* m) {
        PairVec out;
        for (const ProtoSparseListObjectIterator* it = m->getIterator(c); it && it->hasNext(c); it = it->advance(c))
            out.emplace_back(it->nextKey(c), it->nextValue(c));
        return out;
    }
}

TEST(SparseListObjectIterator, EmptyYieldsNothing) {
    ProtoSpace space;
    ProtoContext* c = space.rootContext;
    EXPECT_TRUE(drain(c, c->newSparseListObject()).empty());
}

TEST(SparseListObjectIterator, MatchesProcessElementsInBothForms) {
    ProtoSpace space;
    ProtoContext* c = space.rootContext;
    const ProtoSparseListObject* m = c->newSparseListObject();
    for (int n = 1; n <= 40; ++n) {
        m = m->setAt(c, (n % 3) ? c->newObject(false) : c->fromInteger(n), c->fromInteger(n));
        PairVec expected;
        m->processElements(c, &expected, collect);
        ASSERT_EQ(drain(c, m), expected) << "size " << n;   // n <= 3: Small, n >= 4: AVL
    }
}
