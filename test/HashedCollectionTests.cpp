#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <map>
#include <set>
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
    const ProtoSparseListObject* m = c->newSparseListObject();
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
    const ProtoSparseListObject* m = c->newSparseListObject();
    for (long i = 1; i <= 10; ++i) m = hashedPut(c, m, kColliding, c->fromInteger(i), c->fromInteger(i * 2));
    EXPECT_EQ(m->getSize(c), 1u);                                    // one bucket
    for (long i = 1; i <= 10; ++i) EXPECT_EQ(hashedGet(c, m, kColliding, c->fromInteger(i)), c->fromInteger(i * 2));
    EXPECT_EQ(hashedGet(c, m, kColliding, c->fromInteger(11)), nullptr);
}

TEST(HashedCollection, PutWithAnEqualKeyReplacesTheValue) {
    ProtoSpace space;
    ProtoContext* c = space.rootContext;
    const ProtoSparseListObject* m = c->newSparseListObject();
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
    const ProtoSparseListObject* m = c->newSparseListObject();
    for (long i = 1; i <= 5; ++i) m = hashedPut(c, m, kColliding, c->fromInteger(i), c->fromInteger(i));
    const ProtoSparseListObject* before = m;
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
    const ProtoSparseListObject* m = c->newSparseListObject();
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
    const ProtoSparseListObject* m = c->newSparseListObject();
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
    const ProtoSparseListObject* m = c->newSparseListObject();
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
