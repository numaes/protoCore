// ProtoSet keeps every distinct element whose hash collides with another
// one's: each hash slot holds a bucket of elements, searched by pointer.
//
// The tests before the PROTOCORE_HAS_SET_BUCKETS block use only API that
// protoCore 3.4.0 already had, so the file also builds against that
// library, where they fail: 3.4.0 kept one element per hash and replaced
// it on a collision.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <chrono>
#include <set>
#include <string>
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

std::vector<const ProtoObject*> elementsOf(ProtoContext* context, const ProtoSet* set) {
    std::vector<const ProtoObject*> out;
    for (const ProtoSetIterator* it = set->getIterator(context); it && it->hasNext(context); it = it->advance(context))
        out.push_back(it->next(context));
    return out;
}

bool isIn(ProtoContext* context, const ProtoSet* set, const ProtoObject* value) {
    return set->has(context, value) == PROTO_TRUE;
}

class SetBuckets : public ::testing::Test {
protected:
    ProtoSpace space;
    ProtoContext* context = nullptr;
    void SetUp() override { context = new ProtoContext(&space); }
    void TearDown() override { delete context; }
};

}  // namespace

TEST_F(SetBuckets, ElementsAddedUnderOneHashAreAllKept) {
    const ProtoObject* a = distinctObject(context, 1);
    const ProtoObject* b = distinctObject(context, 2);
    const ProtoObject* c = distinctObject(context, 3);
    const ProtoSet* set = context->newSet()
        ->addWithHash(context, 7, a)->addWithHash(context, 7, b)->addWithHash(context, 7, c);

    EXPECT_EQ(set->getSize(context), 3u);
    EXPECT_TRUE(set->hasHash(context, 7));
    const std::vector<const ProtoObject*> seen = elementsOf(context, set);
    EXPECT_EQ(std::set<const ProtoObject*>(seen.begin(), seen.end()), (std::set<const ProtoObject*>{a, b, c}));
    EXPECT_EQ(seen.size(), 3u);
}

TEST_F(SetBuckets, ElementsWhoseGetHashCollidesAreAllKept) {
    const ProtoObject* a = colliding(context);
    const ProtoObject* b = colliding(context);
    const ProtoObject* c = colliding(context);
    const ProtoObject* absent = colliding(context);
    ASSERT_EQ(a->getHash(context), b->getHash(context));

    const ProtoSet* set = context->newSet()->add(context, a)->add(context, b)->add(context, c);
    EXPECT_EQ(set->getSize(context), 3u);
    EXPECT_TRUE(isIn(context, set, a));
    EXPECT_TRUE(isIn(context, set, b));
    EXPECT_TRUE(isIn(context, set, c));
    EXPECT_FALSE(isIn(context, set, absent)) << "has() matched an element by hash only";
}

TEST_F(SetBuckets, RemoveTakesOnlyThatPointerOutOfTheBucket) {
    const ProtoObject* a = colliding(context);
    const ProtoObject* b = colliding(context);
    const ProtoObject* c = colliding(context);
    const ProtoSet* set = context->newSet()->add(context, a)->add(context, b)->add(context, c);

    const ProtoSet* withoutB = set->remove(context, b);
    EXPECT_EQ(withoutB->getSize(context), 2u);
    EXPECT_TRUE(isIn(context, withoutB, a));
    EXPECT_FALSE(isIn(context, withoutB, b));
    EXPECT_TRUE(isIn(context, withoutB, c));

    // A colliding pointer that is not an element removes nothing.
    const ProtoSet* same = withoutB->remove(context, colliding(context));
    EXPECT_EQ(same->getSize(context), 2u);
    EXPECT_TRUE(isIn(context, same, a));
    EXPECT_TRUE(isIn(context, same, c));
}

TEST_F(SetBuckets, RemovingTheLastElementOfABucketRemovesTheHash) {
    const ProtoObject* a = colliding(context);
    const ProtoObject* b = colliding(context);
    const proto_ulong h = a->getHash(context);
    const ProtoSet* set = context->newSet()->add(context, a)->add(context, b);

    set = set->remove(context, a);
    EXPECT_EQ(set->getSize(context), 1u);
    EXPECT_TRUE(set->hasHash(context, h)) << "removing one element dropped its whole bucket";
    EXPECT_TRUE(isIn(context, set, b));

    set = set->remove(context, b);
    EXPECT_EQ(set->getSize(context), 0u);
    EXPECT_FALSE(set->hasHash(context, h));
    EXPECT_EQ(set->getIterator(context), nullptr);
}

TEST_F(SetBuckets, OldVersionsAreUnchangedByAddAndRemove) {
    const ProtoObject* a = colliding(context);
    const ProtoObject* b = colliding(context);
    const ProtoObject* c = colliding(context);
    const ProtoSet* ab = context->newSet()->add(context, a)->add(context, b);
    const ProtoSet* abc = ab->add(context, c);
    const ProtoSet* bc = abc->remove(context, a);

    EXPECT_EQ(ab->getSize(context), 2u);
    EXPECT_TRUE(isIn(context, ab, a));
    EXPECT_TRUE(isIn(context, ab, b));
    EXPECT_FALSE(isIn(context, ab, c));

    EXPECT_EQ(abc->getSize(context), 3u);
    EXPECT_TRUE(isIn(context, abc, a));
    EXPECT_EQ(elementsOf(context, abc).size(), 3u);

    EXPECT_EQ(bc->getSize(context), 2u);
    EXPECT_FALSE(isIn(context, bc, a));
    EXPECT_TRUE(isIn(context, bc, b));
    EXPECT_TRUE(isIn(context, bc, c));
}

// Buckets of one to six elements (a six-element bucket is past the inline
// list form), each visited once, each reporting its bucket's hash.
TEST_F(SetBuckets, IterationVisitsEveryElementOnceWithItsHash) {
    constexpr int kElements = 300;
    constexpr int kBuckets = 50;  // every bucket gets 6 elements
    std::vector<const ProtoObject*> objects;
    const ProtoSet* set = context->newSet();
    for (int i = 0; i < kElements; ++i) {
        objects.push_back(distinctObject(context, i));
        set = set->addWithHash(context, static_cast<proto_ulong>(i % kBuckets) * 1000003u, objects.back());
    }
    set = set->add(context, context->fromInteger(-1));  // and a singleton bucket
    EXPECT_EQ(set->getSize(context), static_cast<proto_ulong>(kElements + 1));

    std::set<const ProtoObject*> seen;
    int visits = 0;
    for (const ProtoSetIterator* it = set->getIterator(context); it && it->hasNext(context); it = it->advance(context)) {
        const ProtoObject* e = it->next(context);
        ++visits;
        seen.insert(e);
        if (e == context->fromInteger(-1)) {
            EXPECT_EQ(it->nextHash(context), e->getHash(context));
            continue;
        }
        const proto_long i = e->asList(context)->getAt(context, 0)->asLong(context);
        EXPECT_EQ(it->nextHash(context), static_cast<proto_ulong>(i % kBuckets) * 1000003u);
    }
    EXPECT_EQ(visits, kElements + 1);
    EXPECT_EQ(seen.size(), static_cast<size_t>(kElements + 1));
}

// Behaviour change: strings hash by content, but the set compares pointers.
// Two distinct heap strings with the same content are two elements (3.4.0
// kept only the last one); interned strings are one pointer, so one element.
TEST_F(SetBuckets, EqualStringsAreOneElementOnlyWhenTheyAreOnePointer) {
    const char* text = "a string longer than six bytes";
    const ProtoObject* s1 = context->fromUTF8String(text);
    const ProtoObject* s2 = context->fromUTF8String(text);
    ASSERT_NE(s1, s2);
    ASSERT_EQ(s1->getHash(context), s2->getHash(context));

    const ProtoSet* set = context->newSet()->add(context, s1)->add(context, s2);
    EXPECT_EQ(set->getSize(context), 2u);
    EXPECT_TRUE(isIn(context, set, s1));
    EXPECT_TRUE(isIn(context, set, s2));
    EXPECT_FALSE(isIn(context, set, context->fromUTF8String(text)));

    const ProtoObject* i1 = s1->asString(context)->intern(context)->asObject(context);
    const ProtoObject* i2 = s2->asString(context)->intern(context)->asObject(context);
    ASSERT_EQ(i1, i2);
    const ProtoSet* interned = context->newSet()->add(context, i1)->add(context, i2);
    EXPECT_EQ(interned->getSize(context), 1u);

    // Inline strings (up to 6 bytes) are immediates: equal content is one pointer.
    const ProtoSet* inlined = context->newSet()
        ->add(context, context->fromUTF8String("abc"))->add(context, context->fromUTF8String("abc"));
    EXPECT_EQ(inlined->getSize(context), 1u);
}

// Behaviour change: doubles are boxed, so two equal doubles are two elements.
TEST_F(SetBuckets, EqualBoxedDoublesAreTwoElements) {
    const ProtoObject* d1 = context->fromDouble(1.5);
    const ProtoObject* d2 = context->fromDouble(1.5);
    ASSERT_NE(d1, d2);
    const ProtoSet* set = context->newSet()->add(context, d1)->add(context, d2);
    EXPECT_EQ(set->getSize(context), 2u);
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

// The elements are reachable only through the buckets of a set pinned in a
// root set (their allocating context has exited).  After forced collections
// under a small hard heap limit every element is still there, intact.
TEST(SetBucketsGC, BucketsAndTheirElementsSurviveCollections) {
    constexpr int kElements = 600;
    constexpr int kBuckets = 37;
    ProtoSpace space;
    ProtoContext live(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    ProtoRootSet* rs = space.createRootSet("set-buckets");
    ASSERT_NE(rs, nullptr);
    ProtoRootSet::Handle pinned = ProtoRootSet::kNullHandle;
    {
        ProtoContext sub(&space, &live, nullptr, nullptr, nullptr, nullptr);
        const ProtoSet* set = sub.newSet();
        for (int i = 0; i < kElements; ++i)
            set = set->addWithHash(&sub, static_cast<proto_ulong>(i % kBuckets), distinctObject(&sub, i));
        pinned = rs->add(set->asObject(&sub));
    }

    const uint64_t cycles = forceCycles(space, &live, 5);
    ASSERT_TRUE(waitForIdleCollector(space, &live));
    EXPECT_GE(cycles, 5u);

    const ProtoSet* set = rs->resolve(pinned)->asSet(&live);
    ASSERT_NE(set, nullptr);
    EXPECT_EQ(set->getSize(&live), static_cast<proto_ulong>(kElements));
    std::vector<bool> seen(kElements, false);
    int bad = 0;
    for (const ProtoSetIterator* it = set->getIterator(&live); it && it->hasNext(&live); it = it->advance(&live)) {
        const ProtoList* l = it->next(&live)->asList(&live);
        const proto_long i = (l && l->getSize(&live) == 1) ? l->getAt(&live, 0)->asLong(&live) : -1;
        if (i < 0 || i >= kElements || seen[i] || it->nextHash(&live) != static_cast<proto_ulong>(i % kBuckets)) {
            ++bad;
            continue;
        }
        seen[i] = true;
    }
    EXPECT_EQ(bad, 0);
    for (int i = 0; i < kElements; ++i) EXPECT_TRUE(seen[i]) << "element " << i << " lost";
    rs->remove(pinned);
    space.destroyRootSet(rs);
}

#ifdef PROTOCORE_HAS_SET_BUCKETS

TEST_F(SetBuckets, AddingAPresentPointerReturnsTheSameSet) {
    const ProtoObject* a = distinctObject(context, 1);
    const ProtoSet* set = context->newSet()->add(context, a);
    EXPECT_EQ(set->add(context, a), set);
    EXPECT_EQ(set->addWithHash(context, a->getHash(context), a), set);
    EXPECT_EQ(set->remove(context, distinctObject(context, 1)), set);
}

TEST_F(SetBuckets, PointerLevelHashVariants) {
    const ProtoObject* a = distinctObject(context, 1);
    const ProtoObject* b = distinctObject(context, 2);
    const ProtoSet* set = context->newSet()->addWithHash(context, 9, a)->addWithHash(context, 9, b);

    EXPECT_TRUE(set->hasWithHash(context, 9, a));
    EXPECT_TRUE(set->hasWithHash(context, 9, b));
    EXPECT_FALSE(set->hasWithHash(context, 10, a));
    EXPECT_FALSE(set->hasWithHash(context, 9, distinctObject(context, 1)));

    EXPECT_EQ(set->removeWithHash(context, 10, a), set);
    const ProtoSet* onlyB = set->removeWithHash(context, 9, a);
    EXPECT_EQ(onlyB->getSize(context), 1u);
    EXPECT_FALSE(onlyB->hasWithHash(context, 9, a));
    EXPECT_TRUE(onlyB->hasWithHash(context, 9, b));
    EXPECT_TRUE(onlyB->hasHash(context, 9));

    const ProtoSet* empty = onlyB->removeWithHash(context, 9, b);
    EXPECT_EQ(empty->getSize(context), 0u);
    EXPECT_FALSE(empty->hasHash(context, 9));
    EXPECT_EQ(empty->getIterator(context), nullptr);
    EXPECT_EQ(set->getSize(context), 2u);
}

TEST_F(SetBuckets, RemoveHashDropsTheWholeBucketAndItsCount) {
    const ProtoSet* set = context->newSet();
    for (int i = 0; i < 4; ++i) set = set->addWithHash(context, 5, distinctObject(context, i));
    set = set->addWithHash(context, 6, distinctObject(context, 9));
    ASSERT_EQ(set->getSize(context), 5u);
    const ProtoSet* rest = set->removeHash(context, 5);
    EXPECT_EQ(rest->getSize(context), 1u);
    EXPECT_FALSE(rest->hasHash(context, 5));
    EXPECT_TRUE(rest->hasHash(context, 6));
    EXPECT_EQ(rest->removeHash(context, 5), rest);
}

#endif  // PROTOCORE_HAS_SET_BUCKETS
