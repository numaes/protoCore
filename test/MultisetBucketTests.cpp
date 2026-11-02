// ProtoMultiset keeps a separate count for every distinct element (by
// pointer), even when hashes collide: each hash slot holds a bucket of
// (element, count) pairs, searched by pointer.
//
// The tests before the PROTOCORE_HAS_MULTISET_BUCKETS block use only API
// that protoCore 3.4.0 already had, so the file also builds against that
// library, where they fail: 3.4.0 stored one count per hash, never the
// element, and its iterator returned counts.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <chrono>
#include <map>
#include <set>
#include <thread>
#include <vector>

using namespace proto;

namespace {

// A cell whose getHash is a constant: distinct instances always collide.
class CollidingCell final : public Cell {
public:
    explicit CollidingCell(ProtoContext* context) : Cell(context) {}
    proto_ulong getHash(ProtoContext*) const override { return 42; }
    void processReferences(ProtoContext*, void*, void (*)(ProtoContext*, void*, const Cell*)) const override {}
    const ProtoObject* implAsObject(ProtoContext*) const override {
        ProtoObjectPointer p{};
        p.voidPointer = const_cast<CollidingCell*>(this);
        p.op.pointer_tag = POINTER_TAG_EXTERNAL_POINTER;
        return p.oid;
    }
};

const ProtoObject* colliding(ProtoContext* context) {
    return (new (context) CollidingCell(context))->implAsObject(context);
}

// A distinct heap object per call (a one-element list holding `i`).
const ProtoObject* distinctObject(ProtoContext* context, int i) {
    return context->newList()->appendLast(context, context->fromInteger(i))->asObject(context);
}

long long countOf(ProtoContext* context, const ProtoMultiset* bag, const ProtoObject* value) {
    return bag->count(context, value)->asLong(context);
}

// The elements next() returns, in iteration order.
std::vector<const ProtoObject*> elementsOf(ProtoContext* context, const ProtoMultiset* bag) {
    std::vector<const ProtoObject*> out;
    for (const ProtoMultisetIterator* it = bag->getIterator(context); it && it->hasNext(context); it = it->advance(context))
        out.push_back(it->next(context));
    return out;
}

class MultisetBuckets : public ::testing::Test {
protected:
    ProtoSpace space;
    ProtoContext* context = nullptr;
    void SetUp() override { context = new ProtoContext(&space); }
    void TearDown() override { delete context; }
};

}  // namespace

TEST_F(MultisetBuckets, ElementsWhoseGetHashCollidesKeepSeparateCounts) {
    const ProtoObject* a = colliding(context);
    const ProtoObject* b = colliding(context);
    const ProtoObject* absent = colliding(context);
    ASSERT_EQ(a->getHash(context), b->getHash(context));

    const ProtoMultiset* bag = context->newMultiset()->add(context, a)->add(context, a)->add(context, b);
    EXPECT_EQ(bag->getSize(context), 3u);
    EXPECT_EQ(countOf(context, bag, a), 2);
    EXPECT_EQ(countOf(context, bag, b), 1);
    EXPECT_EQ(countOf(context, bag, absent), 0) << "count() matched an element by hash only";
}

// Strings hash by content, but the multiset compares pointers: two distinct
// heap strings with the same content are counted apart.  Interned strings
// are one pointer, so one element.
TEST_F(MultisetBuckets, CountsArePerPointer) {
    const char* text = "a string longer than six bytes";
    const ProtoObject* s1 = context->fromUTF8String(text);
    const ProtoObject* s2 = context->fromUTF8String(text);
    ASSERT_NE(s1, s2);
    ASSERT_EQ(s1->getHash(context), s2->getHash(context));

    const ProtoMultiset* bag = context->newMultiset()->add(context, s1)->add(context, s1)->add(context, s2);
    EXPECT_EQ(countOf(context, bag, s1), 2);
    EXPECT_EQ(countOf(context, bag, s2), 1);
    EXPECT_EQ(countOf(context, bag, context->fromUTF8String(text)), 0);
    EXPECT_EQ(bag->getSize(context), 3u);

    const ProtoObject* i1 = s1->asString(context)->intern(context)->asObject(context);
    const ProtoObject* i2 = s2->asString(context)->intern(context)->asObject(context);
    ASSERT_EQ(i1, i2);
    const ProtoMultiset* interned = context->newMultiset()->add(context, i1)->add(context, i2);
    EXPECT_EQ(countOf(context, interned, i1), 2);

    // Tagged immediates are values: equal small integers are one pointer.
    const ProtoMultiset* ints = context->newMultiset()
        ->add(context, context->fromInteger(7))->add(context, context->fromInteger(7));
    EXPECT_EQ(countOf(context, ints, context->fromInteger(7)), 2);
}

TEST_F(MultisetBuckets, RemoveToZeroDropsOnlyThatElement) {
    const ProtoObject* a = colliding(context);
    const ProtoObject* b = colliding(context);
    const ProtoObject* c = colliding(context);
    const ProtoMultiset* bag = context->newMultiset()
        ->add(context, a)->add(context, a)->add(context, b)->add(context, c);

    bag = bag->remove(context, a);
    EXPECT_EQ(countOf(context, bag, a), 1);
    EXPECT_EQ(countOf(context, bag, b), 1);
    EXPECT_EQ(countOf(context, bag, c), 1);
    EXPECT_EQ(bag->getSize(context), 3u);

    bag = bag->remove(context, a);
    EXPECT_EQ(countOf(context, bag, a), 0);
    EXPECT_EQ(countOf(context, bag, b), 1) << "removing one element's last occurrence touched another";
    EXPECT_EQ(countOf(context, bag, c), 1);
    EXPECT_EQ(bag->getSize(context), 2u);

    // A colliding pointer that does not occur removes nothing.
    const ProtoMultiset* same = bag->remove(context, colliding(context));
    EXPECT_EQ(same->getSize(context), 2u);
    EXPECT_EQ(countOf(context, same, b), 1);
    EXPECT_EQ(countOf(context, same, c), 1);

    bag = bag->remove(context, b)->remove(context, c);
    EXPECT_EQ(bag->getSize(context), 0u);
    EXPECT_EQ(countOf(context, bag, b), 0);
    EXPECT_EQ(bag->getIterator(context), nullptr);
}

TEST_F(MultisetBuckets, IterationRecoversTheElements) {
    const ProtoObject* a = distinctObject(context, 1);
    const ProtoObject* b = distinctObject(context, 2);
    const ProtoObject* c = colliding(context);
    const ProtoObject* d = colliding(context);
    const ProtoMultiset* bag = context->newMultiset()
        ->add(context, a)->add(context, a)->add(context, a)
        ->add(context, b)->add(context, c)->add(context, d)->add(context, d)
        ->add(context, context->fromInteger(5));

    const std::vector<const ProtoObject*> seen = elementsOf(context, bag);
    EXPECT_EQ(seen.size(), 5u) << "iteration must visit each distinct element once";
    EXPECT_EQ(std::set<const ProtoObject*>(seen.begin(), seen.end()),
              (std::set<const ProtoObject*>{a, b, c, d, context->fromInteger(5)}));
    long long total = 0;
    for (const ProtoObject* e : seen) total += countOf(context, bag, e);
    EXPECT_EQ(total, 8);
}

// An element that is itself a ProtoList shaped like a stored pair is still
// one element: pairing inside a bucket is positional.
TEST_F(MultisetBuckets, ListElementsAreNotMistakenForPairs) {
    const ProtoObject* x = colliding(context);
    const ProtoObject* pairLike = context->newList()->appendLast(context, x)
        ->appendLast(context, context->fromInteger(1))->asObject(context);
    const ProtoMultiset* bag = context->newMultiset()->add(context, pairLike)->add(context, x)->add(context, pairLike);
    EXPECT_EQ(countOf(context, bag, pairLike), 2);
    EXPECT_EQ(countOf(context, bag, x), 1);
    const std::vector<const ProtoObject*> seen = elementsOf(context, bag);
    EXPECT_EQ(std::set<const ProtoObject*>(seen.begin(), seen.end()), (std::set<const ProtoObject*>{pairLike, x}));
}

TEST_F(MultisetBuckets, SizeIsTheTotalOfTheCounts) {
    const ProtoMultiset* bag = context->newMultiset();
    std::vector<const ProtoObject*> objs;
    for (int i = 0; i < 4; ++i) objs.push_back(colliding(context));
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k <= i; ++k) bag = bag->add(context, objs[i]);
    EXPECT_EQ(bag->getSize(context), 10u);
    for (int i = 0; i < 4; ++i) EXPECT_EQ(countOf(context, bag, objs[i]), i + 1);
    bag = bag->remove(context, objs[3]);
    EXPECT_EQ(bag->getSize(context), 9u);
    EXPECT_EQ(countOf(context, bag, objs[3]), 3);
    EXPECT_EQ(countOf(context, bag, objs[0]), 1);
}

TEST_F(MultisetBuckets, OldVersionsAreUnchangedByAddAndRemove) {
    const ProtoObject* a = colliding(context);
    const ProtoObject* b = colliding(context);
    const ProtoMultiset* a1 = context->newMultiset()->add(context, a);
    const ProtoMultiset* a2 = a1->add(context, a);
    const ProtoMultiset* a2b1 = a2->add(context, b);
    const ProtoMultiset* a1b1 = a2b1->remove(context, a);
    const ProtoMultiset* b1 = a1b1->remove(context, a);

    EXPECT_EQ(a1->getSize(context), 1u);
    EXPECT_EQ(countOf(context, a1, a), 1);
    EXPECT_EQ(countOf(context, a1, b), 0);

    EXPECT_EQ(a2->getSize(context), 2u);
    EXPECT_EQ(countOf(context, a2, a), 2);
    EXPECT_EQ(countOf(context, a2, b), 0);

    EXPECT_EQ(a2b1->getSize(context), 3u);
    EXPECT_EQ(countOf(context, a2b1, a), 2);
    EXPECT_EQ(countOf(context, a2b1, b), 1);

    EXPECT_EQ(a1b1->getSize(context), 2u);
    EXPECT_EQ(countOf(context, a1b1, a), 1);
    EXPECT_EQ(countOf(context, a1b1, b), 1);

    EXPECT_EQ(b1->getSize(context), 1u);
    EXPECT_EQ(countOf(context, b1, a), 0);
    EXPECT_EQ(countOf(context, b1, b), 1);
}

namespace {

constexpr int kHeadroomCells = 40000;
constexpr int kGarbagePerBatch = 5000;

uint64_t forceCycles(ProtoSpace& space, ProtoContext* parent, uint64_t minCycles) {
    const uint64_t start = space.getGCCycleCount();
    space.setHeapLimits(/*soft=*/0, /*hard=*/space.heapSize + kHeadroomCells);
    for (int batch = 0; batch < 400 && space.getGCCycleCount() - start < minCycles; ++batch) {
        ProtoContext garbage(&space, parent, nullptr, nullptr, nullptr, nullptr);
        for (int i = 0; i < kGarbagePerBatch; ++i) (void) garbage.newObject(false);
    }
    space.setHeapLimits(0, 0);
    return space.getGCCycleCount() - start;
}

bool waitForIdleCollector(ProtoSpace& space, ProtoContext* ctx) {
    ProtoContext::UnmanagedScope parked(ctx);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (space.gcStarted.load()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

}  // namespace

// The elements are reachable only through the buckets of a multiset pinned
// in a root set (their allocating context has exited).  After forced
// collections under a small hard heap limit every element is still there,
// intact, with its count.  Element i occurs (i % 3) + 1 times; all share
// one colliding hash in groups (CollidingCell elements are mixed in).
TEST(MultisetBucketsGC, BucketsElementsAndCountsSurviveCollections) {
    constexpr int kElements = 600;
    ProtoSpace space;
    ProtoContext live(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    ProtoRootSet* rs = space.createRootSet("multiset-buckets");
    ASSERT_NE(rs, nullptr);
    ProtoRootSet::Handle pinned = ProtoRootSet::kNullHandle;
    constexpr int kColliding = 40;
    {
        ProtoContext sub(&space, &live, nullptr, nullptr, nullptr, nullptr);
        const ProtoMultiset* bag = sub.newMultiset();
        for (int i = 0; i < kElements; ++i) {
            const ProtoObject* e = distinctObject(&sub, i);
            for (int k = 0; k <= i % 3; ++k) bag = bag->add(&sub, e);
        }
        for (int i = 0; i < kColliding; ++i) bag = bag->add(&sub, colliding(&sub));
        pinned = rs->add(bag->asObject(&sub));
    }

    const uint64_t cycles = forceCycles(space, &live, 5);
    ASSERT_TRUE(waitForIdleCollector(space, &live));
    EXPECT_GE(cycles, 5u);

    const ProtoMultiset* bag = rs->resolve(pinned)->asMultiset(&live);
    ASSERT_NE(bag, nullptr);
    proto_ulong expectedSize = kColliding;
    for (int i = 0; i < kElements; ++i) expectedSize += (i % 3) + 1;
    EXPECT_EQ(bag->getSize(&live), expectedSize);

    std::vector<bool> seen(kElements, false);
    int bad = 0, collidingSeen = 0;
    for (const ProtoMultisetIterator* it = bag->getIterator(&live); it && it->hasNext(&live); it = it->advance(&live)) {
        const ProtoObject* e = it->next(&live);
        if (e->getHash(&live) == 42 && !e->asList(&live)) {
            ++collidingSeen;
            if (countOf(&live, bag, e) != 1) ++bad;
            continue;
        }
        const ProtoList* l = e->asList(&live);
        const proto_long i = (l && l->getSize(&live) == 1) ? l->getAt(&live, 0)->asLong(&live) : -1;
        if (i < 0 || i >= kElements || seen[i] || countOf(&live, bag, e) != (i % 3) + 1) {
            ++bad;
            continue;
        }
        seen[i] = true;
    }
    EXPECT_EQ(bad, 0);
    EXPECT_EQ(collidingSeen, kColliding);
    for (int i = 0; i < kElements; ++i) EXPECT_TRUE(seen[i]) << "element " << i << " lost";
    rs->remove(pinned);
    space.destroyRootSet(rs);
}

#ifdef PROTOCORE_HAS_MULTISET_BUCKETS

TEST_F(MultisetBuckets, DistinctSizeHasAndIteratorCounts) {
    const ProtoObject* a = colliding(context);
    const ProtoObject* b = colliding(context);
    const ProtoMultiset* bag = context->newMultiset()->add(context, a)->add(context, a)->add(context, b);
    EXPECT_EQ(bag->getSize(context), 3u);
    EXPECT_EQ(bag->getDistinctSize(context), 2u);
    EXPECT_EQ(bag->has(context, a), PROTO_TRUE);
    EXPECT_EQ(bag->has(context, colliding(context)), PROTO_FALSE);

    std::map<const ProtoObject*, proto_ulong> counts;
    for (const ProtoMultisetIterator* it = bag->getIterator(context); it && it->hasNext(context); it = it->advance(context)) {
        EXPECT_EQ(it->nextHash(context), 42u);
        counts[it->next(context)] = it->nextCount(context);
    }
    EXPECT_EQ(counts, (std::map<const ProtoObject*, proto_ulong>{{a, 2}, {b, 1}}));

    const ProtoMultiset* onlyB = bag->remove(context, a)->remove(context, a);
    EXPECT_EQ(onlyB->getDistinctSize(context), 1u);
    EXPECT_EQ(onlyB->has(context, a), PROTO_FALSE);
    EXPECT_EQ(bag->getDistinctSize(context), 2u);
}

TEST_F(MultisetBuckets, RemoveOfAnAbsentPointerReturnsTheSameMultiset) {
    const ProtoMultiset* bag = context->newMultiset()->add(context, distinctObject(context, 1));
    EXPECT_EQ(bag->remove(context, distinctObject(context, 1)), bag);
    EXPECT_EQ(bag->removeWithHash(context, 3, distinctObject(context, 1)), bag);
}

TEST_F(MultisetBuckets, PointerLevelHashVariants) {
    const ProtoObject* a = distinctObject(context, 1);
    const ProtoObject* b = distinctObject(context, 2);
    const ProtoMultiset* bag = context->newMultiset()
        ->addWithHash(context, 9, a)->addWithHash(context, 9, a)->addWithHash(context, 9, b);

    EXPECT_EQ(bag->countWithHash(context, 9, a), 2u);
    EXPECT_EQ(bag->countWithHash(context, 9, b), 1u);
    EXPECT_EQ(bag->countWithHash(context, 10, a), 0u);
    EXPECT_TRUE(bag->hasWithHash(context, 9, b));
    EXPECT_FALSE(bag->hasWithHash(context, 9, distinctObject(context, 1)));
    EXPECT_EQ(bag->getSize(context), 3u);
    EXPECT_EQ(bag->getDistinctSize(context), 2u);

    EXPECT_EQ(bag->removeWithHash(context, 10, a), bag);
    const ProtoMultiset* r = bag->removeWithHash(context, 9, a)->removeWithHash(context, 9, a);
    EXPECT_EQ(r->countWithHash(context, 9, a), 0u);
    EXPECT_EQ(r->countWithHash(context, 9, b), 1u);
    const ProtoMultiset* empty = r->removeWithHash(context, 9, b);
    EXPECT_EQ(empty->getSize(context), 0u);
    EXPECT_EQ(empty->getDistinctSize(context), 0u);
    EXPECT_EQ(empty->getIterator(context), nullptr);
    EXPECT_EQ(bag->countWithHash(context, 9, a), 2u);
}

// Buckets of six elements (twelve list entries, past the inline list form),
// each element visited once with its count and its bucket's hash; an
// exhausted iterator stays safe to query.
TEST_F(MultisetBuckets, IterationVisitsEveryElementOnceWithItsCountAndHash) {
    constexpr int kElements = 300;
    constexpr int kBuckets = 50;
    const ProtoMultiset* bag = context->newMultiset();
    for (int i = 0; i < kElements; ++i) {
        const ProtoObject* e = distinctObject(context, i);
        const proto_ulong h = static_cast<proto_ulong>(i % kBuckets) * 1000003u;
        for (int k = 0; k <= i % 4; ++k) bag = bag->addWithHash(context, h, e);
    }
    EXPECT_EQ(bag->getDistinctSize(context), static_cast<proto_ulong>(kElements));

    std::set<proto_long> seen;
    const ProtoMultisetIterator* it = bag->getIterator(context);
    int visits = 0;
    for (; it && it->hasNext(context); it = it->advance(context)) {
        const proto_long i = it->next(context)->asList(context)->getAt(context, 0)->asLong(context);
        ++visits;
        seen.insert(i);
        EXPECT_EQ(it->nextCount(context), static_cast<proto_ulong>(i % 4 + 1));
        EXPECT_EQ(it->nextHash(context), static_cast<proto_ulong>(i % kBuckets) * 1000003u);
    }
    EXPECT_EQ(visits, kElements);
    EXPECT_EQ(seen.size(), static_cast<size_t>(kElements));
    ASSERT_NE(it, nullptr);
    EXPECT_FALSE(it->hasNext(context));
    EXPECT_EQ(it->nextCount(context), 0u);
    EXPECT_FALSE(it->advance(context)->hasNext(context));
}

#endif  // PROTOCORE_HAS_MULTISET_BUCKETS
