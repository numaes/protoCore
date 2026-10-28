// AttributeListTests.cpp -- the list form of object attributes
// (ProtoAttributeListImplementation) and the form rules of AttributeStorage.
//
// An object holds its own attributes in the AVL form or, up to
// attributeListMax() attributes, in a singly linked list of 64-byte cells with
// two (key, value) pairs each.  These tests pin down:
//
//   * the form each mutator produces (promotion past the ceiling, demotion at
//     half of it, every other mutation keeps the form);
//   * the list layout (count per cell, only the head may hold one pair, new
//     and overwritten pairs at the head, the tail after an overwrite shared);
//   * overwrite and removal at every position, against a std::map model;
//   * iteration, getOwnAttributes, getAttributes and the prototype chain;
//   * mutable objects under concurrent writers (the CAS on the mutable table);
//   * values reachable only through list cells surviving collection under a
//     low heap limit.
//
// Enumeration order is unspecified; every comparison here is order-free.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace proto;

namespace {

using Model = std::map<proto_ulong, const ProtoObject*>;

class AttributeListTest : public ::testing::Test {
protected:
    ProtoSpace* space = nullptr;
    ProtoContext* ctx = nullptr;
    unsigned savedMax = 0;

    void SetUp() override {
        savedMax = attributeListMax();
        setAttributeListMax(8);
        space = new ProtoSpace();
        ctx = space->rootContext;
    }
    void TearDown() override {
        delete space;
        setAttributeListMax(savedMax);
    }

    const ProtoString* sym(const std::string& s) { return ProtoString::createSymbol(ctx, s.c_str()); }
    std::vector<const ProtoString*> names(int n, const char* prefix = "a") {
        std::vector<const ProtoString*> out;
        for (int i = 0; i < n; ++i) out.push_back(sym(std::string(prefix) + std::to_string(i)));
        return out;
    }
};

// The storage of `o`, resolving a mutable handle to its current state.
AttributeStorage storageOf(ProtoContext* c, const ProtoObject* o) {
    const ProtoObjectCell* oc = toImpl<const ProtoObjectCell>(o);
    if (oc->mutable_ref > 0) {
        // getOwnAttributes would convert; read the state through a clone,
        // which copies the current state's storage word as it is.
        oc = toImpl<const ProtoObjectCell>(o->clone(c, false));
    }
    return oc->attributes;
}

// Checks the list invariants; returns the number of pairs.
proto_ulong checkList(const AttributeStorage& s, bool& ok) {
    proto_ulong n = 0;
    for (const ProtoAttributeListImplementation* c = s.list(); c; c = c->next) {
        n += c->pairsHere();
        if (c != s.list() && c->pairsHere() != 2) ok = false;   // only the head may hold one pair
        if (c->count != c->pairsHere() + (c->next ? c->next->count : 0)) ok = false;
        if (c->value0 == nullptr) ok = false;
        if (c->value1 == nullptr && c->key1 != 0) ok = false;
    }
    return n;
}

Model ownModel(ProtoContext* c, const ProtoObject* o) {
    Model m;
    o->processOwnAttributes(c, &m, [](ProtoContext*, void* self, const ProtoString* k, const ProtoObject* v) {
        auto* mm = static_cast<Model*>(self);
        EXPECT_EQ(mm->count(reinterpret_cast<proto_ulong>(k)), 0u) << "a key was visited twice";
        (*mm)[reinterpret_cast<proto_ulong>(k)] = v;
    });
    return m;
}

Model sparseModel(ProtoContext* c, const ProtoSparseList* l) {
    Model m;
    l->processElements(c, &m, [](ProtoContext*, void* self, proto_ulong k, const ProtoObject* v) {
        (*static_cast<Model*>(self))[k] = v;
    });
    return m;
}

// Every own attribute of `o` read back through getAttribute, the walk, the
// public sparse list and hasOwnAttribute agrees with `m`.
void expectMatches(ProtoContext* c, const ProtoObject* o, const Model& m,
                   const std::vector<const ProtoString*>& universe) {
    EXPECT_EQ(ownModel(c, o), m);
    EXPECT_EQ(sparseModel(c, o->getOwnAttributes(c)), m);
    for (const ProtoString* k : universe) {
        auto it = m.find(reinterpret_cast<proto_ulong>(k));
        const ProtoObject* v = o->getOwnAttributeDirect(c, k);
        if (it == m.end()) {
            EXPECT_EQ(v, nullptr);
            EXPECT_EQ(o->hasOwnAttribute(c, k), PROTO_FALSE);
        } else {
            EXPECT_EQ(v, it->second);
            EXPECT_EQ(o->getAttribute(c, k), it->second);
            EXPECT_EQ(o->hasOwnAttribute(c, k), PROTO_TRUE);
        }
    }
}

}  // namespace

// Up to the ceiling the storage is a list of ceil(n / 2) cells; one more
// attribute promotes it to the AVL form.
TEST_F(AttributeListTest, FormFollowsTheSizeOnGrowth) {
    auto ks = names(10);
    const ProtoObject* o = ctx->newObject(false);
    EXPECT_TRUE(storageOf(ctx, o).isEmptyTree());
    Model m;
    for (int i = 0; i < 10; ++i) {
        o = o->setAttribute(ctx, ks[i], ctx->fromInteger(i));
        m[reinterpret_cast<proto_ulong>(ks[i])] = ctx->fromInteger(i);
        const AttributeStorage s = storageOf(ctx, o);
        EXPECT_EQ(s.size(), static_cast<proto_ulong>(i + 1));
        if (i + 1 <= 8) {
            ASSERT_TRUE(s.isList()) << "n=" << i + 1;
            bool ok = true;
            EXPECT_EQ(checkList(s, ok), static_cast<proto_ulong>(i + 1));
            EXPECT_TRUE(ok);
            unsigned cells = 0;
            for (auto* c = s.list(); c; c = c->next) ++cells;
            EXPECT_EQ(cells, static_cast<unsigned>((i + 2) / 2));
            EXPECT_EQ(s.list()->key0, reinterpret_cast<proto_ulong>(ks[i])) << "a new pair goes to the head";
        } else {
            EXPECT_FALSE(s.isList()) << "n=" << i + 1;
        }
        expectMatches(ctx, o, m, ks);
    }
}

// Removal keeps the AVL form down to half the ceiling (hysteresis), then
// demotes; a list stays a list as it shrinks.
TEST_F(AttributeListTest, DemotionAtHalfTheCeiling) {
    auto ks = names(12);
    const ProtoObject* o = ctx->newObject(false);
    Model m;
    for (int i = 0; i < 12; ++i) {
        o = o->setAttribute(ctx, ks[i], ctx->fromInteger(i));
        m[reinterpret_cast<proto_ulong>(ks[i])] = ctx->fromInteger(i);
    }
    ASSERT_FALSE(storageOf(ctx, o).isList());
    for (int i = 0; i < 12; ++i) {
        o = o->removeAttribute(ctx, ks[i]);
        m.erase(reinterpret_cast<proto_ulong>(ks[i]));
        const AttributeStorage s = storageOf(ctx, o);
        const int left = 11 - i;
        EXPECT_EQ(s.size(), static_cast<proto_ulong>(left));
        if (left > 4) EXPECT_FALSE(s.isList()) << "left=" << left;
        else if (left > 0) {
            EXPECT_TRUE(s.isList()) << "left=" << left;
            bool ok = true;
            checkList(s, ok);
            EXPECT_TRUE(ok);
        } else {
            EXPECT_TRUE(s.isEmptyTree());
        }
        expectMatches(ctx, o, m, ks);
    }
    // Growing again from the demoted list: list up to 8, AVL past it.
    for (int i = 0; i < 9; ++i) o = o->setAttribute(ctx, ks[i], ctx->fromInteger(100 + i));
    EXPECT_FALSE(storageOf(ctx, o).isList());
}

// An overwrite at every position of every list size: the new pair goes to the
// head, the cells after the old pair's cell are shared, and the result agrees
// with the model.  The cells copied are those up to the old pair's cell.
TEST_F(AttributeListTest, OverwriteAtEveryPosition) {
    setAttributeListMax(16);
    auto ks = names(16);
    for (int n = 1; n <= 16; ++n) {
        const ProtoObject* base = ctx->newObject(false);
        Model m;
        for (int i = 0; i < n; ++i) {
            base = base->setAttribute(ctx, ks[i], ctx->fromInteger(i));
            m[reinterpret_cast<proto_ulong>(ks[i])] = ctx->fromInteger(i);
        }
        const AttributeStorage bs = storageOf(ctx, base);
        ASSERT_TRUE(bs.isList());
        for (int k = 0; k < n; ++k) {
            const proto_ulong key = reinterpret_cast<proto_ulong>(ks[k]);
            // Depth of the cell holding ks[k] and the cell after it.
            unsigned depth = 0;
            const ProtoAttributeListImplementation* hit = bs.list();
            while (hit->key0 != key && hit->key1 != key) { hit = hit->next; ++depth; }
            const proto_ulong before = ctx->allocatedCellsCount;
            const ProtoObject* o = base->setAttribute(ctx, ks[k], ctx->fromInteger(1000 + k));
            const proto_ulong cells = ctx->allocatedCellsCount - before;
            Model mm = m;
            mm[key] = ctx->fromInteger(1000 + k);
            const AttributeStorage s = storageOf(ctx, o);
            ASSERT_TRUE(s.isList());
            bool ok = true;
            EXPECT_EQ(checkList(s, ok), static_cast<proto_ulong>(n));
            EXPECT_TRUE(ok) << "n=" << n << " k=" << k;
            EXPECT_EQ(s.list()->key0, key) << "the overwritten pair goes to the head";
            // The tail after the old pair's cell is shared, not copied.
            if (hit->next) {
                bool shared = false;
                for (auto* c = s.list(); c; c = c->next) if (c == hit->next) shared = true;
                EXPECT_TRUE(shared) << "n=" << n << " k=" << k;
            }
            // Cells: the copied prefix (depth + 1 cells' pairs, repacked) plus
            // the object cell; the allocation counter may reset at a
            // submission, so only a plausible delta is checked.
            if (cells < 1000) EXPECT_LE(cells, depth + 2u) << "n=" << n << " k=" << k;
            expectMatches(ctx, o, mm, ks);
            expectMatches(ctx, base, m, ks);   // the receiver is unchanged
        }
    }
}

// Writing the value a key already holds changes nothing.
TEST_F(AttributeListTest, SameValueWriteKeepsTheStorage) {
    auto ks = names(5);
    const ProtoObject* o = ctx->newObject(false);
    for (int i = 0; i < 5; ++i) o = o->setAttribute(ctx, ks[i], ctx->fromInteger(i));
    const AttributeStorage before = storageOf(ctx, o);
    const ProtoObject* o2 = o->setAttribute(ctx, ks[2], ctx->fromInteger(2));
    EXPECT_EQ(storageOf(ctx, o2), before);
}

// Removal at every position of every list size.
TEST_F(AttributeListTest, RemovalAtEveryPosition) {
    setAttributeListMax(16);
    auto ks = names(16);
    for (int n = 1; n <= 16; ++n) {
        const ProtoObject* base = ctx->newObject(false);
        Model m;
        for (int i = 0; i < n; ++i) {
            base = base->setAttribute(ctx, ks[i], ctx->fromInteger(i));
            m[reinterpret_cast<proto_ulong>(ks[i])] = ctx->fromInteger(i);
        }
        for (int k = 0; k < n; ++k) {
            const ProtoObject* o = base->removeAttribute(ctx, ks[k]);
            Model mm = m;
            mm.erase(reinterpret_cast<proto_ulong>(ks[k]));
            const AttributeStorage s = storageOf(ctx, o);
            bool ok = true;
            if (n == 1) EXPECT_TRUE(s.isEmptyTree());
            else {
                ASSERT_TRUE(s.isList());
                EXPECT_EQ(checkList(s, ok), static_cast<proto_ulong>(n - 1));
                EXPECT_TRUE(ok);
            }
            expectMatches(ctx, o, mm, ks);
            // Removing an absent name is a no-op answering the receiver.
            EXPECT_EQ(o->removeAttribute(ctx, ks[k]), o);
        }
    }
}

// Inherited lookup, shadowing, hasAttribute and the merged view work across
// list-form and AVL-form ancestors.
TEST_F(AttributeListTest, PrototypeChainAcrossForms) {
    auto ks = names(20);
    const ProtoObject* grand = ctx->newObject(false);
    for (int i = 0; i < 12; ++i) grand = grand->setAttribute(ctx, ks[i], ctx->fromInteger(i));   // AVL
    const ProtoObject* parent = grand->newChild(ctx, false);
    for (int i = 10; i < 14; ++i) parent = parent->setAttribute(ctx, ks[i], ctx->fromInteger(100 + i));   // list
    const ProtoObject* child = parent->newChild(ctx, false);
    child = child->setAttribute(ctx, ks[0], ctx->fromInteger(-1));
    ASSERT_FALSE(storageOf(ctx, grand).isList());
    ASSERT_TRUE(storageOf(ctx, parent).isList());
    ASSERT_TRUE(storageOf(ctx, child).isList());

    EXPECT_EQ(child->getAttribute(ctx, ks[0])->asLong(ctx), -1);          // own shadows grand
    EXPECT_EQ(child->getAttribute(ctx, ks[5])->asLong(ctx), 5);           // from grand
    EXPECT_EQ(child->getAttribute(ctx, ks[11])->asLong(ctx), 111);        // parent shadows grand
    EXPECT_EQ(child->getAttribute(ctx, ks[13])->asLong(ctx), 113);        // from parent
    EXPECT_EQ(child->getAttribute(ctx, ks[19]), PROTO_NONE);              // miss over the chain
    EXPECT_EQ(child->hasAttribute(ctx, ks[13]), PROTO_TRUE);
    EXPECT_EQ(child->hasAttribute(ctx, ks[19]), PROTO_FALSE);

    const Model merged = sparseModel(ctx, child->getAttributes(ctx));
    EXPECT_EQ(merged.size(), 14u);
    EXPECT_EQ(merged.at(reinterpret_cast<proto_ulong>(ks[0])), ctx->fromInteger(-1));
    EXPECT_EQ(merged.at(reinterpret_cast<proto_ulong>(ks[11])), ctx->fromInteger(111));
    EXPECT_EQ(merged.at(reinterpret_cast<proto_ulong>(ks[3])), ctx->fromInteger(3));
    // The read-only calls left the forms as they were.
    EXPECT_TRUE(storageOf(ctx, child).isList());
    EXPECT_TRUE(storageOf(ctx, parent).isList());
}

// Random operations (setAttribute, removeAttribute, setAttributes) on mutable
// and immutable objects, for several ceilings including 0 (AVL only),
// against a std::map model.
TEST_F(AttributeListTest, RandomOperationsMatchTheModel) {
    std::mt19937 rng(20261028);
    auto ks = names(40, "r");
    for (unsigned max : {0u, 1u, 2u, 3u, 4u, 8u, 13u, 32u, 64u}) {
        setAttributeListMax(max);
        for (bool isMutable : {false, true}) {
            const ProtoObject* o = ctx->newObject(isMutable);
            Model m;
            for (int step = 0; step < 600; ++step) {
                const int op = static_cast<int>(rng() % 10);
                if (op < 6) {
                    const ProtoString* k = ks[rng() % ks.size()];
                    const ProtoObject* v = ctx->fromInteger(static_cast<int>(rng() % 5));
                    const ProtoObject* r = o->setAttribute(ctx, k, v);
                    if (!isMutable) o = r;
                    m[reinterpret_cast<proto_ulong>(k)] = v;
                } else if (op < 8) {
                    const ProtoString* k = ks[rng() % ks.size()];
                    const ProtoObject* r = o->removeAttribute(ctx, k);
                    if (!isMutable) o = r;
                    m.erase(reinterpret_cast<proto_ulong>(k));
                } else {
                    const unsigned g = 1 + rng() % 12;
                    std::vector<const ProtoString*> gn;
                    std::vector<const ProtoObject*> gv;
                    for (unsigned i = 0; i < g; ++i) {
                        gn.push_back(ks[rng() % ks.size()]);
                        gv.push_back(rng() % 4 == 0 ? nullptr : ctx->fromInteger(static_cast<int>(rng() % 5)));
                    }
                    const ProtoObject* r = o->setAttributes(ctx, g, gn.data(), gv.data());
                    if (!isMutable) o = r;
                    for (unsigned i = 0; i < g; ++i) {
                        if (gv[i]) m[reinterpret_cast<proto_ulong>(gn[i])] = gv[i];
                        else m.erase(reinterpret_cast<proto_ulong>(gn[i]));
                    }
                }
                const AttributeStorage s = storageOf(ctx, o);
                ASSERT_EQ(s.size(), m.size()) << "max=" << max << " step=" << step;
                if (s.isList()) {
                    bool ok = true;
                    ASSERT_EQ(checkList(s, ok), m.size());
                    ASSERT_TRUE(ok) << "max=" << max << " step=" << step;
                    ASSERT_LE(m.size(), max);
                }
                if (max == 0) ASSERT_FALSE(s.isList());
                if (!s.isList() && !s.isEmptyTree() && max > 0) ASSERT_GT(m.size(), max / 2) << "an AVL at or below half the ceiling";
                if (step % 50 == 0) expectMatches(ctx, o, m, ks);
            }
            expectMatches(ctx, o, m, ks);
        }
    }
}

namespace {

constexpr int kWriters = 4;
constexpr int kRounds = 2000;
const ProtoObject* gObject = nullptr;
const ProtoString* gOwn[kWriters];
const ProtoString* gCounter = nullptr;

// Each writer overwrites its own name and increments a shared counter with
// setAttributeIfEqual (compare-and-set on the attribute).
const ProtoObject* listWriter(ProtoContext* c, const ProtoObject*, const ParentLink*,
                              const ProtoArguments* args, const ProtoKeywordArguments*) {
    const int w = static_cast<int>(args->getAt(c, 0)->asLong(c));
    for (int k = 1; k <= kRounds; ++k) {
        gObject->setAttribute(c, gOwn[w], c->fromInteger(k));
        while (true) {
            const ProtoObject* cur = gObject->getAttribute(c, gCounter);
            if (gObject->setAttributeIfEqual(c, gCounter, cur, c->fromInteger(cur->asLong(c) + 1))) break;
        }
    }
    return PROTO_NONE;
}

}  // namespace

// Concurrent writers on one mutable list-form object lose no update.
TEST_F(AttributeListTest, ConcurrentWritersOnAMutableList) {
    const ProtoObject* obj = ctx->newObject(true);
    gCounter = sym("counter");
    obj->setAttribute(ctx, gCounter, ctx->fromInteger(0));
    for (int w = 0; w < kWriters; ++w) {
        gOwn[w] = sym("own" + std::to_string(w));
        obj->setAttribute(ctx, gOwn[w], ctx->fromInteger(0));
    }
    ASSERT_TRUE(storageOf(ctx, obj).isList());
    gObject = obj;
    std::vector<const ProtoThread*> writers;
    for (int w = 0; w < kWriters; ++w) {
        const ProtoList* args = ctx->newList()->appendLast(ctx, ctx->fromInteger(w));
        writers.push_back(space->newThread(ctx, sym("list-writer"), listWriter, args, nullptr));
    }
    for (const ProtoThread* t : writers) const_cast<ProtoThread*>(t)->join(ctx);
    gObject = nullptr;
    EXPECT_EQ(obj->getAttribute(ctx, gCounter)->asLong(ctx), kWriters * kRounds);
    for (int w = 0; w < kWriters; ++w) EXPECT_EQ(obj->getAttribute(ctx, gOwn[w])->asLong(ctx), kRounds);
    const AttributeStorage s = storageOf(ctx, obj);
    ASSERT_TRUE(s.isList());
    bool ok = true;
    EXPECT_EQ(checkList(s, ok), static_cast<proto_ulong>(kWriters + 1));
    EXPECT_TRUE(ok);
}

// Under a low heap limit the collector runs many cycles while list-form
// objects are rewritten; values reachable only through list cells (fresh heap
// strings) must survive, in mutable and immutable objects.
TEST_F(AttributeListTest, ValuesSurviveCollectionUnderPressure) {
    auto ks = names(8, "g");
    const ProtoObject* mut = ctx->newObject(true);
    ProtoRootSet* roots = space->createRootSet("attribute-list-gc");
    const ProtoObject* imm = ctx->newObject(false);
    ProtoRootSet::Handle h = roots->add(imm);
    const uint64_t startCycles = space->getGCCycleCount();
    space->setHeapLimits(0, space->heapSize + 40000);
    // Run until at least ten cycles have completed (and 3,000 rounds), so
    // the result does not depend on how fast the collector is (ASan).
    int rounds = 0;
    for (int round = 0; round < 200000; ++round, rounds = round) {
        if (round >= 3000 && space->getGCCycleCount() >= startCycles + 10) break;
        ProtoContext c(space, ctx, nullptr, nullptr, nullptr, nullptr);
        const int k = round % 8;
        const std::string text = "v" + std::to_string(round);
        mut->setAttribute(&c, ks[k], c.fromUTF8String(text.c_str()));
        const ProtoObject* next = roots->resolve(h)->setAttribute(&c, ks[(k * 3) % 8], c.fromUTF8String(text.c_str()));
        roots->remove(h);
        h = roots->add(next);
        for (int g = 0; g < 100; ++g) (void) c.newObject(false);   // garbage
        c.safepoint();
    }
    space->setHeapLimits(0, 0);
    EXPECT_GE(space->getGCCycleCount(), startCycles + 10) << "too few cycles; the test proved little";
    ASSERT_TRUE(storageOf(ctx, mut).isList());
    for (int k = 0; k < 8; ++k) {
        // The last round that wrote ks[k] into `mut`: the largest r < rounds with r % 8 == k.
        int last = rounds - 1;
        while (last % 8 != k) --last;
        const ProtoObject* v = mut->getAttribute(ctx, ks[k]);
        ASSERT_TRUE(v->isString(ctx));
        EXPECT_EQ(v->compare(ctx, ctx->fromUTF8String(("v" + std::to_string(last)).c_str())), 0);
    }
    const ProtoObject* fin = roots->resolve(h);
    for (int k = 0; k < 8; ++k) {
        const ProtoObject* v = fin->getAttribute(ctx, ks[k]);
        ASSERT_TRUE(v->isString(ctx)) << "k=" << k;
    }
    roots->remove(h);
}
