// SetAttributesTests.cpp -- ProtoObject::setAttributes, a group of attribute
// writes published as one version.
//
// `obj.f1 = a; obj.f2 = b; obj.f3 = c` on a mutable object costs three
// publications into the mutable table: three snapshots, three shard-root path
// copies, three CASes.  setAttributes is the same program written in immutable
// style: take the current snapshot once, derive the new version from it, and
// publish that version once.  The tests below pin down the contract:
//
//   * the result is exactly what the sequential setAttribute calls produce;
//   * a mutable receiver is published once, not once per name;
//   * the group is atomic: a concurrent reader sees all of it or none of it,
//     and concurrent writers to other names of the same object lose nothing;
//   * values that live only in the caller's arrays survive collection cycles
//     that run during the call.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <atomic>
#include <random>
#include <string>
#include <vector>

using namespace proto;

namespace {

class SetAttributesTest : public ::testing::Test {
protected:
    ProtoSpace* space = nullptr;
    ProtoContext* ctx = nullptr;

    void SetUp() override {
        space = new ProtoSpace();
        ctx = space->rootContext;
    }
    void TearDown() override { delete space; }

    const ProtoString* sym(const char* s) { return ProtoString::createSymbol(ctx, s); }
    const ProtoString* sym(const std::string& s) { return sym(s.c_str()); }
};

// Every own attribute of `o`, as (key word, value) pairs in key order.
std::vector<std::pair<unsigned long, const ProtoObject*>>
ownEntries(ProtoContext* c, const ProtoObject* o) {
    std::vector<std::pair<unsigned long, const ProtoObject*>> out;
    o->getOwnAttributes(c)->processElements(c, &out,
        [](ProtoContext*, void* self, proto_ulong key, const ProtoObject* value) {
            static_cast<std::vector<std::pair<unsigned long, const ProtoObject*>>*>(self)
                ->emplace_back(key, value);
        });
    return out;
}

}  // namespace

// An immutable receiver answers a new version equal to the chained
// setAttribute result, and is itself left untouched.
TEST_F(SetAttributesTest, ImmutableReceiverMatchesChainedSetAttribute) {
    const ProtoObject* base = ctx->newObject(false);
    for (int i = 0; i < 7; ++i)
        base = base->setAttribute(ctx, sym("k" + std::to_string(i)), ctx->fromInteger(i));
    const auto before = ownEntries(ctx, base);

    const ProtoString* names[] = {sym("k2"), sym("new1"), sym("k5"), sym("new2")};
    const ProtoObject* values[] = {ctx->fromInteger(20), ctx->fromInteger(100),
                                   ctx->fromInteger(50), ctx->fromInteger(200)};

    const ProtoObject* chained = base;
    for (int i = 0; i < 4; ++i) chained = chained->setAttribute(ctx, names[i], values[i]);
    const ProtoObject* grouped = base->setAttributes(ctx, 4, names, values);

    ASSERT_NE(grouped, base) << "an immutable receiver must answer a new version";
    EXPECT_EQ(ownEntries(ctx, grouped), ownEntries(ctx, chained));
    EXPECT_EQ(ownEntries(ctx, base), before) << "the receiver must not change";
}

// A mutable receiver keeps its identity and ends in the state the sequential
// writes produce, including overwrites of names it already had.
TEST_F(SetAttributesTest, MutableReceiverMatchesSequentialSetAttribute) {
    for (int initial : {0, 1, 3, 8, 40}) {
        const ProtoObject* seq = ctx->newObject(true);
        const ProtoObject* grp = ctx->newObject(true);
        for (int i = 0; i < initial; ++i) {
            seq->setAttribute(ctx, sym("f" + std::to_string(i)), ctx->fromInteger(i));
            grp->setAttribute(ctx, sym("f" + std::to_string(i)), ctx->fromInteger(i));
        }
        std::vector<const ProtoString*> names;
        std::vector<const ProtoObject*> values;
        for (int i = 0; i < 6; ++i) {
            names.push_back(sym("f" + std::to_string(i * 3)));   // some exist, some do not
            values.push_back(ctx->fromInteger(1000 + i));
        }
        for (size_t i = 0; i < names.size(); ++i) seq->setAttribute(ctx, names[i], values[i]);
        const ProtoObject* r = grp->setAttributes(ctx, static_cast<unsigned>(names.size()),
                                                  names.data(), values.data());

        EXPECT_EQ(r, grp) << "a mutable receiver keeps its identity (initial=" << initial << ")";
        EXPECT_EQ(ownEntries(ctx, grp), ownEntries(ctx, seq)) << "initial=" << initial;
        for (size_t i = 0; i < names.size(); ++i)
            EXPECT_EQ(grp->getAttribute(ctx, names[i]), values[i]) << "initial=" << initial;
    }
}

// A name written twice in one group takes the later value, as two
// sequential writes would.
TEST_F(SetAttributesTest, RepeatedNameTakesTheLaterValue) {
    const ProtoObject* obj = ctx->newObject(true);
    const ProtoString* names[] = {sym("x"), sym("y"), sym("x")};
    const ProtoObject* values[] = {ctx->fromInteger(1), ctx->fromInteger(2), ctx->fromInteger(3)};
    obj->setAttributes(ctx, 3, names, values);
    EXPECT_EQ(obj->getAttribute(ctx, sym("x"))->asLong(ctx), 3);
    EXPECT_EQ(obj->getAttribute(ctx, sym("y"))->asLong(ctx), 2);

    const ProtoObject* imm = ctx->newObject(false)->setAttributes(ctx, 3, names, values);
    EXPECT_EQ(imm->getAttribute(ctx, sym("x"))->asLong(ctx), 3);
}

// A nullptr value removes the name, exactly as setAttribute(name, nullptr)
// does, and a later write in the same group can install it again.
TEST_F(SetAttributesTest, NullValueRemovesLikeSetAttribute) {
    const ProtoObject* seq = ctx->newObject(true);
    const ProtoObject* grp = ctx->newObject(true);
    for (const ProtoObject* o : {seq, grp}) {
        o->setAttribute(ctx, sym("a"), ctx->fromInteger(1));
        o->setAttribute(ctx, sym("b"), ctx->fromInteger(2));
    }
    const ProtoString* names[] = {sym("a"), sym("c"), sym("b"), sym("b")};
    const ProtoObject* values[] = {nullptr, ctx->fromInteger(3), nullptr, ctx->fromInteger(4)};
    for (int i = 0; i < 4; ++i) seq->setAttribute(ctx, names[i], values[i]);
    grp->setAttributes(ctx, 4, names, values);
    EXPECT_EQ(ownEntries(ctx, grp), ownEntries(ctx, seq));
    EXPECT_EQ(grp->hasOwnAttribute(ctx, sym("a")), PROTO_FALSE);
}

// Heap-string names are interned, as setAttribute interns them: the stored
// key is the symbol, so a lookup by symbol finds the value.
TEST_F(SetAttributesTest, HeapStringNamesAreInterned) {
    const ProtoObject* obj = ctx->newObject(true);
    const ProtoString* heapName = ctx->fromUTF8String("heapKey")->asString(ctx);
    ASSERT_NE(reinterpret_cast<const ProtoObject*>(heapName),
              reinterpret_cast<const ProtoObject*>(sym("heapKey"))) << "the name must not be a symbol yet";
    const ProtoString* names[] = {heapName};
    const ProtoObject* values[] = {ctx->fromInteger(7)};
    obj->setAttributes(ctx, 1, names, values);
    EXPECT_EQ(obj->getAttribute(ctx, sym("heapKey"))->asLong(ctx), 7);
}

// No names: nothing to publish, nothing allocated, the receiver answered.
TEST_F(SetAttributesTest, EmptyGroupIsANoOp) {
    const ProtoObject* obj = ctx->newObject(true);
    obj->setAttribute(ctx, sym("a"), ctx->fromInteger(1));
    ProtoContext c(space, ctx, nullptr, nullptr, nullptr, nullptr);
    const proto_ulong before = c.allocatedCellsCount;
    EXPECT_EQ(obj->setAttributes(&c, 0, nullptr, nullptr), obj);
    EXPECT_EQ(c.allocatedCellsCount, before);
}

// The thread's attribute cache must not keep answering a value the group
// replaced.
TEST_F(SetAttributesTest, AttributeCacheSeesTheNewValues) {
    const ProtoObject* obj = ctx->newObject(true);
    obj->setAttribute(ctx, sym("a"), ctx->fromInteger(1));
    obj->setAttribute(ctx, sym("b"), ctx->fromInteger(2));
    EXPECT_EQ(obj->getAttribute(ctx, sym("a"))->asLong(ctx), 1);   // warm the cache
    EXPECT_EQ(obj->getAttribute(ctx, sym("b"))->asLong(ctx), 2);
    const ProtoString* names[] = {sym("a"), sym("b")};
    const ProtoObject* values[] = {ctx->fromInteger(10), ctx->fromInteger(20)};
    obj->setAttributes(ctx, 2, names, values);
    EXPECT_EQ(obj->getAttribute(ctx, sym("a"))->asLong(ctx), 10);
    EXPECT_EQ(obj->getAttribute(ctx, sym("b"))->asLong(ctx), 20);
}

// One publication: a group on a mutable object costs what the same group
// costs on an immutable copy (the new version) plus ONE mutable-table
// publication (one shard-root path copy), however many names it carries.
// The sequential form pays a snapshot and a publication per name.
TEST_F(SetAttributesTest, MutableGroupIsPublishedOnce) {
    // Populate the mutable table so a publication has a real path to copy.
    std::vector<const ProtoObject*> others;
    for (int i = 0; i < 256 * 16; ++i) {
        const ProtoObject* o = ctx->newObject(true);
        o->setAttribute(ctx, sym("v"), ctx->fromInteger(i));
        others.push_back(o);
    }
    const int N = 8;
    std::vector<const ProtoString*> names;
    for (int i = 0; i < N; ++i) names.push_back(sym("field" + std::to_string(i)));

    auto makeObject = [&]() {
        const ProtoObject* o = ctx->newObject(true);
        for (int i = 0; i < N; ++i) o->setAttribute(ctx, names[i], ctx->fromInteger(i));
        return o;
    };
    std::vector<const ProtoObject*> values;
    for (int i = 0; i < N; ++i) values.push_back(ctx->fromInteger(500 + i));

    auto cellsOf = [&](auto&& fn) {
        ProtoContext c(space, ctx, nullptr, nullptr, nullptr, nullptr);
        const proto_ulong before = c.allocatedCellsCount;
        fn(&c);
        return c.allocatedCellsCount - before;
    };

    const ProtoObject* mut = makeObject();
    const ProtoObject* imm = mut->clone(ctx, false);   // same attribute tree

    // Cost of one publication of this object = one mutable write minus the
    // same write on the immutable copy.
    const proto_ulong onePublication =
        cellsOf([&](ProtoContext* c) { mut->setAttribute(c, names[0], ctx->fromInteger(-1)); }) -
        cellsOf([&](ProtoContext* c) { imm->setAttribute(c, names[0], ctx->fromInteger(-1)); });
    ASSERT_GT(onePublication, 1u);

    const proto_ulong newVersion =
        cellsOf([&](ProtoContext* c) { imm->setAttributes(c, N, names.data(), values.data()); });
    const proto_ulong grouped =
        cellsOf([&](ProtoContext* c) { mut->setAttributes(c, N, names.data(), values.data()); });
    EXPECT_EQ(grouped, newVersion + onePublication)
        << "the group must be one version plus one publication";

    const ProtoObject* mut2 = makeObject();
    const proto_ulong sequential = cellsOf([&](ProtoContext* c) {
        for (int i = 0; i < N; ++i) mut2->setAttribute(c, names[i], values[i]);
    });
    EXPECT_GE(sequential, grouped + (N - 1) * onePublication)
        << "sequential=" << sequential << " grouped=" << grouped;
    EXPECT_EQ(ownEntries(ctx, mut), ownEntries(ctx, mut2));
}

// Building a fresh object (a constructor's run of writes): the group builds
// the attribute tree in one pass.
TEST_F(SetAttributesTest, FreshObjectGroupAllocatesLessThanSequential) {
    const int N = 5;
    std::vector<const ProtoString*> names;
    std::vector<const ProtoObject*> values;
    for (int i = 0; i < N; ++i) {
        names.push_back(sym("p" + std::to_string(i)));
        values.push_back(ctx->fromInteger(i));
    }
    auto cellsOf = [&](auto&& fn) {
        ProtoContext c(space, ctx, nullptr, nullptr, nullptr, nullptr);
        const proto_ulong before = c.allocatedCellsCount;
        fn(&c);
        return c.allocatedCellsCount - before;
    };
    const proto_ulong sequential = cellsOf([&](ProtoContext* c) {
        const ProtoObject* o = c->newObject(true);
        for (int i = 0; i < N; ++i) o->setAttribute(c, names[i], values[i]);
    });
    const proto_ulong grouped = cellsOf([&](ProtoContext* c) {
        const ProtoObject* o = c->newObject(true);
        o->setAttributes(c, N, names.data(), values.data());
    });
    std::printf("[ cells    ] 5-field mutable object: sequential=%lu grouped=%lu\n",
                static_cast<unsigned long>(sequential), static_cast<unsigned long>(grouped));
    EXPECT_LT(grouped, sequential);
}

namespace {

// Checks the AVL invariants of an attribute tree and answers its height:
// keys strictly ascending, every node's height and size consistent with its
// children, and the two subtrees of every node within one level.
int checkAvl(const ProtoSparseListImplementation* n, uintptr_t lo, uintptr_t hi, bool& ok,
             proto_ulong* size) {
    if (!n || n->isEmpty) { *size = 0; return 0; }
    if (n->key <= lo || n->key >= hi) ok = false;
    proto_ulong ls = 0, rs = 0;
    const int lh = checkAvl(n->previous, lo, n->key, ok, &ls);
    const int rh = checkAvl(n->next, n->key, hi, ok, &rs);
    if (lh - rh > 1 || rh - lh > 1) ok = false;
    const int h = 1 + (lh > rh ? lh : rh);
    if (static_cast<int>(n->height) != h) ok = false;
    *size = ls + rs + 1;
    if (n->size != *size) ok = false;
    if (n->value == nullptr) ok = false;
    return h;
}

}  // namespace

// Random groups (inserts, overwrites, removals, repeated names) on random
// trees: the one-pass build must equal the chained result and stay a valid
// AVL tree.
TEST_F(SetAttributesTest, RandomGroupsMatchChainedWritesAndStayBalanced) {
    std::mt19937 rng(20261003);
    std::vector<const ProtoString*> pool;
    for (int i = 0; i < 64; ++i) pool.push_back(sym("r" + std::to_string(i)));
    for (int round = 0; round < 1500; ++round) {
        const ProtoObject* base = ctx->newObject(false);
        const int initial = static_cast<int>(rng() % 48);
        for (int i = 0; i < initial; ++i)
            base = base->setAttribute(ctx, pool[rng() % pool.size()], ctx->fromInteger(i));
        const unsigned k = 1 + rng() % 24;
        std::vector<const ProtoString*> names;
        std::vector<const ProtoObject*> values;
        for (unsigned i = 0; i < k; ++i) {
            names.push_back(pool[rng() % pool.size()]);
            values.push_back(rng() % 5 == 0 ? nullptr : ctx->fromInteger(1000 + i));
        }
        const ProtoObject* chained = base;
        for (unsigned i = 0; i < k; ++i) chained = chained->setAttribute(ctx, names[i], values[i]);
        const ProtoObject* grouped = base->setAttributes(ctx, k, names.data(), values.data());
        ASSERT_EQ(ownEntries(ctx, grouped), ownEntries(ctx, chained)) << "round " << round;

        bool ok = true;
        proto_ulong size = 0;
        checkAvl(toImpl<const ProtoObjectCell>(grouped)->attributes, 0, UINTPTR_MAX, ok, &size);
        ASSERT_TRUE(ok) << "round " << round << ": the built tree is not a valid AVL tree";
        ASSERT_EQ(size, ownEntries(ctx, chained).size()) << "round " << round;
    }
}

namespace {

// Shared state of the concurrency test.  A ProtoMethod is a plain function
// pointer, so the workers reach the object through these globals.
constexpr int kWriters = 4;
constexpr int kReaders = 2;
constexpr int kGroupSize = 3;
constexpr int kRounds = 3000;
const ProtoObject* gObject = nullptr;
const ProtoString* gNames[kWriters][kGroupSize];
std::atomic<bool> gWritersDone{false};
std::atomic<long> gTornReads{0};
std::atomic<long> gReads{0};

const ProtoObject* groupWriter(ProtoContext* c, const ProtoObject*, const ParentLink*,
                               const ProtoList* args, const ProtoSparseList*) {
    const int w = static_cast<int>(args->getAt(c, 0)->asLong(c));
    for (int k = 1; k <= kRounds; ++k) {
        const ProtoObject* v = c->fromInteger(k);
        const ProtoObject* values[kGroupSize] = {v, v, v};
        gObject->setAttributes(c, kGroupSize, gNames[w], values);
    }
    return PROTO_NONE;
}

// Reads one snapshot at a time and checks every writer's group in it: the
// three names of a group must always carry the same round number.
const ProtoObject* groupReader(ProtoContext* c, const ProtoObject*, const ParentLink*,
                               const ProtoList*, const ProtoSparseList*) {
    while (!gWritersDone.load(std::memory_order_acquire)) {
        const ProtoSparseList* snap = gObject->getOwnAttributes(c);
        for (int w = 0; w < kWriters; ++w) {
            const ProtoObject* first = snap->getAt(c, reinterpret_cast<uintptr_t>(gNames[w][0]));
            for (int i = 1; i < kGroupSize; ++i) {
                if (snap->getAt(c, reinterpret_cast<uintptr_t>(gNames[w][i])) != first) {
                    gTornReads.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
        }
        gReads.fetch_add(1, std::memory_order_relaxed);
    }
    return PROTO_NONE;
}

}  // namespace

// Several protoCore threads write their own groups of names on ONE object
// while readers take snapshots.  No writer may lose another's names (the CAS
// retry reapplies the whole group onto the newer snapshot), and no reader
// may see half a group.
TEST_F(SetAttributesTest, ConcurrentGroupsAreAtomicAndLoseNoUpdate) {
    const ProtoObject* obj = ctx->newObject(true);
    for (int w = 0; w < kWriters; ++w)
        for (int i = 0; i < kGroupSize; ++i) {
            gNames[w][i] = sym("w" + std::to_string(w) + "_" + std::to_string(i));
            obj->setAttribute(ctx, gNames[w][i], ctx->fromInteger(0));
        }
    gObject = obj;
    gWritersDone = false;
    gTornReads = 0;
    gReads = 0;

    std::vector<const ProtoThread*> readers, writers;
    for (int r = 0; r < kReaders; ++r)
        readers.push_back(space->newThread(ctx, sym("group-reader"), groupReader, nullptr, nullptr));
    for (int w = 0; w < kWriters; ++w) {
        const ProtoList* args = ctx->newList()->appendLast(ctx, ctx->fromInteger(w));
        writers.push_back(space->newThread(ctx, sym("group-writer"), groupWriter, args, nullptr));
    }
    for (const ProtoThread* t : writers) const_cast<ProtoThread*>(t)->join(ctx);
    gWritersDone = true;
    for (const ProtoThread* t : readers) const_cast<ProtoThread*>(t)->join(ctx);
    gObject = nullptr;

    EXPECT_EQ(gTornReads.load(), 0) << "a reader saw part of a group (" << gReads.load() << " reads)";
    EXPECT_GT(gReads.load(), 0);
    for (int w = 0; w < kWriters; ++w)
        for (int i = 0; i < kGroupSize; ++i)
            EXPECT_EQ(obj->getAttribute(ctx, gNames[w][i])->asLong(ctx), kRounds)
                << "writer " << w << " name " << i << " lost its last update";
}

// The values of a group are often fresh cells referenced only by the
// caller's array.  Collection cycles forced while groups are built must not
// reclaim them, nor the half-built version.
TEST_F(SetAttributesTest, FreshValuesSurviveCyclesDuringTheCall) {
    const ProtoObject* obj = ctx->newObject(true);
    const int N = 6;
    std::vector<const ProtoString*> names;
    for (int i = 0; i < N; ++i) names.push_back(sym("s" + std::to_string(i)));

    const uint64_t startCycles = space->getGCCycleCount();
    space->setHeapLimits(0, space->heapSize + 40000);
    for (int round = 0; round < 400; ++round) {
        ProtoContext c(space, ctx, nullptr, nullptr, nullptr, nullptr);
        std::vector<const ProtoObject*> values;
        for (int i = 0; i < N; ++i) {
            const std::string text = "r" + std::to_string(round) + "v" + std::to_string(i);
            const ProtoList* list = c.newList()->appendLast(&c, c.fromUTF8String(text.c_str()));
            values.push_back(list->asObject(&c));
        }
        // Garbage so the space keeps cycling.
        for (int g = 0; g < 400; ++g) (void) c.newObject(false);
        obj->setAttributes(&c, N, names.data(), values.data());
        c.safepoint();
    }
    space->setHeapLimits(0, 0);
    EXPECT_GT(space->getGCCycleCount(), startCycles) << "no cycle ran; the test proved nothing";

    for (int i = 0; i < N; ++i) {
        const ProtoObject* v = obj->getAttribute(ctx, names[i]);
        ASSERT_TRUE(v && v != PROTO_NONE && v->asList(ctx));
        const ProtoObject* s = v->asList(ctx)->getAt(ctx, 0);
        const std::string expected = "r399v" + std::to_string(i);
        ASSERT_TRUE(s->isString(ctx));
        EXPECT_EQ(s->compare(ctx, ctx->fromUTF8String(expected.c_str())), 0) << "i=" << i;
    }
}
