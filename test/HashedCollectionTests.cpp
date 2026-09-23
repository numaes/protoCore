#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <atomic>
#include <set>
#include <string>
#include <thread>
#include <chrono>
#include <utility>

using namespace proto;

namespace {
    // Test language: integers and strings have value equality; everything
    // else is identity.
    bool testIsIdentity(ProtoContext* c, const ProtoObject* k) { return !k->isInteger(c) && !k->isString(c); }
    unsigned long testHash(ProtoContext* c, const ProtoObject* k) {
        return k->isInteger(c) ? static_cast<unsigned long>(k->asLong(c)) : k->getHash(c);
    }
    bool testEquals(ProtoContext* c, const ProtoObject* a, const ProtoObject* b) {
        if (a->isInteger(c) && b->isInteger(c)) return a->asLong(c) == b->asLong(c);
        if (a->isString(c) && b->isString(c)) return a->asString(c)->cmp_to_string(c, b->asString(c)) == 0;
        return a == b;
    }
    unsigned long constantHash(ProtoContext*, const ProtoObject*) { return 42; }

    const KeySemantics kTest{testIsIdentity, testHash, testEquals};
    const KeySemantics kColliding{testIsIdentity, constantHash, testEquals};

    struct Seen { std::multiset<std::pair<long, long>> pairs; };
    void record(ProtoContext* c, void* self, const ProtoObject* k, const ProtoObject* v) {
        static_cast<Seen*>(self)->pairs.insert({k->isInteger(c) ? k->asLong(c) : -1, v->asLong(c)});
    }
}

TEST(HashedCollection, IdentityAndHashedKeysCoexist) {
    ProtoSpace space;
    ProtoContext* c = space.rootContext;
    const ProtoObject* o = c->newObject(false);
    const ProtoObject* s1 = c->fromUTF8String("a longer string key");
    const ProtoObject* s2 = c->fromUTF8String("a longer string key");   // equal, distinct object
    const ProtoMap* m = c->newMap();
    m = hashedPut(c, m, kTest, o, c->fromInteger(1));
    m = hashedPut(c, m, kTest, c->fromInteger(5), c->fromInteger(2));
    m = hashedPut(c, m, kTest, s1, c->fromInteger(3));
    EXPECT_EQ(hashedGet(c, m, kTest, o), c->fromInteger(1));
    EXPECT_EQ(hashedGet(c, m, kTest, c->fromInteger(5)), c->fromInteger(2));
    EXPECT_EQ(hashedGet(c, m, kTest, s2), c->fromInteger(3));           // value equality
    EXPECT_EQ(hashedGet(c, m, kTest, c->newObject(false)), nullptr);
    EXPECT_EQ(m->getSize(c), 3u);
}

TEST(HashedCollection, ForcedCollisionsKeepEveryEntry) {
    ProtoSpace space;
    ProtoContext* c = space.rootContext;
    const ProtoMap* m = c->newMap();
    for (long i = 1; i <= 10; ++i) m = hashedPut(c, m, kColliding, c->fromInteger(i), c->fromInteger(i * 2));
    EXPECT_EQ(m->getSize(c), 1u);                                    // one bucket
    for (long i = 1; i <= 10; ++i) EXPECT_EQ(hashedGet(c, m, kColliding, c->fromInteger(i)), c->fromInteger(i * 2));
    EXPECT_EQ(hashedGet(c, m, kColliding, c->fromInteger(11)), nullptr);
}

TEST(HashedCollection, PutWithAnEqualKeyReplacesTheValue) {
    ProtoSpace space;
    ProtoContext* c = space.rootContext;
    const ProtoMap* m = c->newMap();
    m = hashedPut(c, m, kColliding, c->fromInteger(1), c->fromInteger(10));
    m = hashedPut(c, m, kColliding, c->fromInteger(2), c->fromInteger(20));
    m = hashedPut(c, m, kColliding, c->fromInteger(1), c->fromInteger(11));
    EXPECT_EQ(hashedGet(c, m, kColliding, c->fromInteger(1)), c->fromInteger(11));
    Seen seen;
    hashedForEach(c, m, &seen, record);
    EXPECT_EQ(seen.pairs, (std::multiset<std::pair<long, long>>{{1, 11}, {2, 20}}));
}

TEST(HashedCollection, RemoveFromACollisionBucket) {
    ProtoSpace space;
    ProtoContext* c = space.rootContext;
    const ProtoMap* m = c->newMap();
    for (long i = 1; i <= 5; ++i) m = hashedPut(c, m, kColliding, c->fromInteger(i), c->fromInteger(i));
    const ProtoMap* before = m;
    m = hashedRemove(c, m, kColliding, c->fromInteger(3));
    EXPECT_EQ(hashedGet(c, m, kColliding, c->fromInteger(3)), nullptr);
    for (long i : {1L, 2L, 4L, 5L}) EXPECT_EQ(hashedGet(c, m, kColliding, c->fromInteger(i)), c->fromInteger(i));
    EXPECT_EQ(hashedGet(c, before, kColliding, c->fromInteger(3)), c->fromInteger(3));   // persistence
    EXPECT_EQ(hashedRemove(c, m, kColliding, c->fromInteger(99)), m);                     // absent: unchanged
    for (long i : {1L, 2L, 4L, 5L}) m = hashedRemove(c, m, kColliding, c->fromInteger(i));
    EXPECT_EQ(m->getSize(c), 0u);
}

TEST(HashedCollection, ForEachYieldsEveryPairExactlyOnce) {
    ProtoSpace space;
    ProtoContext* c = space.rootContext;
    const ProtoMap* m = c->newMap();
    std::multiset<std::pair<long, long>> expected;
    for (long i = 0; i < 20; ++i) {
        m = hashedPut(c, m, kColliding, c->fromInteger(i), c->fromInteger(100 + i));
        expected.insert({i, 100 + i});
    }
    for (long i = 0; i < 5; ++i) {
        m = hashedPut(c, m, kColliding, c->newObject(false), c->fromInteger(200 + i));
        expected.insert({-1, 200 + i});
    }
    Seen seen;
    hashedForEach(c, m, &seen, record);
    EXPECT_EQ(seen.pairs, expected);
}

TEST(HashedCollection, IntegersAlwaysTakeTheHashedPath) {
    ProtoSpace space;
    ProtoContext* c = space.rootContext;
    const KeySemantics allIdentity{
        [](ProtoContext*, const ProtoObject*) { return true; }, testHash, testEquals};
    const ProtoMap* m = c->newMap();
    m = hashedPut(c, m, allIdentity, c->fromInteger(7), c->fromInteger(70));
    EXPECT_EQ(hashedGet(c, m, allIdentity, c->fromInteger(7)), c->fromInteger(70));
    const ProtoObject* slot = m->getAt(c, c->fromInteger(7));   // slot key = SmallInteger(7), value = [7, 70]
    ASSERT_NE(slot, nullptr);
    ASSERT_NE(slot->asList(c), nullptr);
    EXPECT_EQ(slot->asList(c)->getSize(c), 2u);
}

TEST(HashedCollection, HashesAbove53BitsStayEmbeddedSmallIntegerWords) {
    ProtoSpace space;
    ProtoContext* c = space.rootContext;
    const KeySemantics wideHash{
        testIsIdentity, [](ProtoContext*, const ProtoObject*) { return ~0UL; }, testEquals};
    const ProtoMap* m = c->newMap();
    m = hashedPut(c, m, wideHash, c->fromInteger(1), c->fromInteger(10));
    m = hashedPut(c, m, wideHash, c->fromInteger(2), c->fromInteger(20));
    EXPECT_EQ(m->getSize(c), 1u);
    EXPECT_EQ(hashedGet(c, m, wideHash, c->fromInteger(1)), c->fromInteger(10));
    EXPECT_EQ(hashedGet(c, m, wideHash, c->fromInteger(2)), c->fromInteger(20));
    struct Probe { const ProtoObject* slotKey = nullptr; } probe;
    m->processElements(c, &probe, [](ProtoContext*, void* self, const ProtoObject* k, const ProtoObject*) {
        static_cast<Probe*>(self)->slotKey = k;
    });
    ProtoObjectPointer p{};
    p.oid = probe.slotKey;
    EXPECT_EQ(p.op.pointer_tag, static_cast<unsigned long>(POINTER_TAG_EMBEDDED_VALUE));
    EXPECT_EQ(p.op.embedded_type, static_cast<unsigned long>(EMBEDDED_TYPE_SMALLINT));
    EXPECT_EQ(p.op.value, (1UL << 54) - 1);
}

namespace {
    std::atomic<int> gCallbackCalls{0};
    bool countingIsIdentity(ProtoContext*, const ProtoObject*) { ++gCallbackCalls; return false; }
    unsigned long countingHash(ProtoContext*, const ProtoObject*) { ++gCallbackCalls; return 1; }
    bool countingEquals(ProtoContext*, const ProtoObject*, const ProtoObject*) { ++gCallbackCalls; return false; }
    const KeySemantics kCounting{countingIsIdentity, countingHash, countingEquals};

    void countPairs(ProtoContext*, void* self, const ProtoObject*, const ProtoObject*) { ++*static_cast<int*>(self); }
}

// D3 (PROTOMAP-SPEC §7): a nullptr key is ignored silently by every entry point,
// before any language callback runs.
TEST(HashedCollection, NullKeyIsIgnoredWithoutCallingTheLanguage) {
    ProtoSpace space;
    ProtoContext* c = space.rootContext;
    const ProtoMap* m = hashedPut(c, c->newMap(), kTest, c->fromInteger(1), c->fromInteger(10));
    gCallbackCalls = 0;
    EXPECT_EQ(hashedPut(c, m, kCounting, nullptr, c->fromInteger(2)), m);
    EXPECT_EQ(hashedPut(c, m, kCounting, nullptr, nullptr), m);
    EXPECT_EQ(hashedGet(c, m, kCounting, nullptr), nullptr);
    EXPECT_EQ(hashedRemove(c, m, kCounting, nullptr), m);
    EXPECT_EQ(gCallbackCalls.load(), 0);
    int pairs = 0;
    hashedForEach(c, m, &pairs, nullptr);                      // no visitor: no-op
    hashedForEach(c, m, &pairs, countPairs);
    EXPECT_EQ(pairs, 1);
    EXPECT_EQ(m->getSize(c), 1u);
}

// A nullptr value removes the key, as ProtoMap::setAt does, on
// both the identity path and the hashed path (single pair and collision bucket).
TEST(HashedCollection, NullValueRemovesTheKey) {
    ProtoSpace space;
    ProtoContext* c = space.rootContext;
    const ProtoObject* o = c->newObject(false);
    const ProtoMap* m = c->newMap();
    m = hashedPut(c, m, kTest, o, c->fromInteger(1));
    m = hashedPut(c, m, kTest, c->fromInteger(5), c->fromInteger(2));
    m = hashedPut(c, m, kTest, o, nullptr);
    EXPECT_EQ(hashedGet(c, m, kTest, o), nullptr);
    m = hashedPut(c, m, kTest, c->fromInteger(5), nullptr);
    EXPECT_EQ(hashedGet(c, m, kTest, c->fromInteger(5)), nullptr);
    EXPECT_EQ(m->getSize(c), 0u);
    EXPECT_EQ(hashedPut(c, m, kTest, c->fromInteger(6), nullptr), m);   // absent: unchanged

    const ProtoMap* b = c->newMap();
    for (long i = 1; i <= 3; ++i) b = hashedPut(c, b, kColliding, c->fromInteger(i), c->fromInteger(i * 2));
    b = hashedPut(c, b, kColliding, c->fromInteger(2), nullptr);
    EXPECT_EQ(hashedGet(c, b, kColliding, c->fromInteger(2)), nullptr);
    EXPECT_EQ(hashedGet(c, b, kColliding, c->fromInteger(1)), c->fromInteger(2));
    EXPECT_EQ(hashedGet(c, b, kColliding, c->fromInteger(3)), c->fromInteger(6));
}

namespace {
    // String keys with value equality, whose callbacks allocate: each call
    // builds a fresh ProtoString and some garbage before answering.
    constexpr int kGarbagePerCallback = 64;
    std::atomic<int> gAllocatingCalls{0};

    std::string contentOf(ProtoContext* c, const ProtoObject* k) {
        // Allocates: a new string derived from the key, then read back.
        const ProtoString* copy = k->asString(c)->appendLast(c, c->fromUTF8String("#")->asString(c));
        for (int i = 0; i < kGarbagePerCallback; ++i) (void) c->newObject(false);
        std::string s = copy->toStdString(c);
        s.pop_back();   // drop the '#' suffix
        return s;
    }
    bool allocIsIdentity(ProtoContext* c, const ProtoObject* k) { return !k->isString(c); }
    unsigned long allocHash(ProtoContext* c, const ProtoObject* k) {
        ++gAllocatingCalls;
        const std::string s = contentOf(c, k);
        unsigned long h = 1469598103934665603UL;
        for (unsigned char ch : s) { h ^= ch; h *= 1099511628211UL; }
        return h % 31;  // few slots: most lookups run equals inside a bucket
    }
    bool allocEquals(ProtoContext* c, const ProtoObject* a, const ProtoObject* b) {
        ++gAllocatingCalls;
        return contentOf(c, a) == contentOf(c, b);
    }
    const KeySemantics kAllocating{allocIsIdentity, allocHash, allocEquals};

    const ProtoObject* keyString(ProtoContext* c, int i) {
        return c->fromUTF8String(("allocating-key-" + std::to_string(i)).c_str());
    }
}

// Language callbacks that allocate, under a small hard heap limit and with a
// thread requesting collections continuously, so that cycles run while the
// helper is between a callback and the construction of the new version.
// The map lives only in a root set between operations; every operation runs
// in a short-lived context whose own allocations become garbage.
TEST(HashedCollection, AllocatingCallbacksUnderGcPressure) {
    constexpr int kKeys = 400;
    ProtoSpace space;
    ProtoContext live(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    ProtoRootSet* rs = space.createRootSet("hashed-allocating-callbacks");
    ASSERT_NE(rs, nullptr);
    ProtoRootSet::Handle pinned = rs->add(live.newMap()->asObject(&live));

    auto step = [&](auto&& op) {
        ProtoContext sub(&space, &live, nullptr, nullptr, nullptr, nullptr);
        const ProtoMap* m = rs->resolve(pinned)->asMap(&sub);
        const ProtoMap* next = op(&sub, m);
        const ProtoRootSet::Handle h = rs->add(next->asObject(&sub));
        rs->remove(pinned);
        pinned = h;
    };

    gAllocatingCalls = 0;
    const uint64_t startCycles = space.getGCCycleCount();
    space.setHeapLimits(/*soft=*/0, /*hard=*/space.heapSize + 40000);
    std::atomic<bool> stopGc{false};
    std::thread gcKicker([&]() {
        while (!stopGc.load(std::memory_order_relaxed)) {
            space.triggerGC();
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    for (int i = 0; i < kKeys; ++i)
        step([&](ProtoContext* s, const ProtoMap* m) {
            return hashedPut(s, m, kAllocating, keyString(s, i), s->fromInteger(i));
        });
    for (int i = 0; i < kKeys; i += 2)          // replace through an equal, distinct key
        step([&](ProtoContext* s, const ProtoMap* m) {
            return hashedPut(s, m, kAllocating, keyString(s, i), s->fromInteger(i * 10));
        });
    for (int i = 0; i < kKeys; i += 3)
        step([&](ProtoContext* s, const ProtoMap* m) {
            return hashedRemove(s, m, kAllocating, keyString(s, i));
        });

    // Verify one key per short-lived context: every lookup allocates in the
    // callbacks, and a live context's young cells cannot be reclaimed.
    int bad = 0, present = 0;
    auto check = [&](auto&& probe) {
        ProtoContext sub(&space, &live, nullptr, nullptr, nullptr, nullptr);
        probe(&sub, rs->resolve(pinned)->asMap(&sub));
    };
    for (int i = 0; i <= kKeys; ++i)
        check([&](ProtoContext* s, const ProtoMap* m) {
            const ProtoObject* v = hashedGet(s, m, kAllocating, keyString(s, i));
            if (i == kKeys || i % 3 == 0) { bad += v != nullptr; return; }   // never put / removed
            ++present;
            const long expected = (i % 2 == 0) ? i * 10 : i;
            bad += (v == nullptr || !v->isInteger(s) || v->asLong(s) != expected);
        });
    int pairs = 0;
    check([&](ProtoContext* s, const ProtoMap* m) { hashedForEach(s, m, &pairs, countPairs); });
    EXPECT_EQ(pairs, present);
    stopGc.store(true, std::memory_order_relaxed);
    gcKicker.join();
    space.setHeapLimits(0, 0);

    EXPECT_EQ(bad, 0);
    EXPECT_GT(gAllocatingCalls.load(), kKeys);
    EXPECT_GE(space.getGCCycleCount() - startCycles, 3u) << "too few collections ran during the test";
    rs->remove(pinned);
    space.destroyRootSet(rs);
}
