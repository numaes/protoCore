// MapParentChainTests.cpp — ProtoMap under the rewritten parent-chain rules.
//
// ProtoMap (tag POINTER_TAG_MAP) and the parent-chain rewrite
// (isInstanceOf / hasParent / hasAttribute / getAttributes / getAttribute /
// newChild / setParents) landed on two separate branches that never saw each
// other.  They meet in exactly one place: a ProtoMap handle is a NON-OBJECT
// cell pointer, so every rule the rewrite states for a non-object receiver or
// a non-object chain entry must hold for it — through
// `ProtoSpace::mapPrototype`, which `getPrototype` now answers for tag 27.
//
// The other files test each half on its own (test_protomap.cpp,
// NonObjectParentTests.cpp, SetParentsFlattenTests.cpp, ...); this one pins
// the interaction, so a later change to either half cannot quietly drop the
// map case.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <chrono>
#include <thread>

using namespace proto;

namespace {

constexpr int kHeadroomCells = 40000;
constexpr int kGarbagePerBatch = 5000;

// Same forced-collection pattern as ProtoMapGCTests.cpp / GCRootScopeTests.cpp.
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

class MapParentChainTest : public ::testing::Test {
protected:
    proto::ProtoSpace* space = nullptr;
    proto::ProtoContext* context = nullptr;

    void SetUp() override {
        space = new proto::ProtoSpace();
        context = space->rootContext;
    }
    void TearDown() override { delete space; }

    const ProtoString* sym(const char* s) { return ProtoString::createSymbol(context, s); }

    // A non-empty map, as the public object word the object model sees.
    const ProtoObject* mapWord() {
        const ProtoMap* m = context->newMap();
        m = m->setAt(context, context->fromInteger(1), context->fromInteger(10));
        return m->asObject(context);
    }
};

// --- The receiver side: a map answers through mapPrototype ---------------

TEST_F(MapParentChainTest, GetPrototypeOfAMapIsTheMapPrototype) {
    EXPECT_EQ(mapWord()->getPrototype(context), space->mapPrototype)
        << "getPrototype's POINTER_TAG_MAP case must survive the chain rewrite";
}

TEST_F(MapParentChainTest, IsInstanceOfAnswersForAMapThroughItsPrototype) {
    const ProtoObject* m = mapWord();
    EXPECT_EQ(m->isInstanceOf(context, space->mapPrototype), PROTO_TRUE);
    EXPECT_EQ(m->isInstanceOf(context, space->listPrototype), PROTO_NONE);
    EXPECT_EQ(m->isInstanceOf(context, space->sparseListPrototype), PROTO_NONE)
        << "a ProtoMap is not a ProtoSparseList: the two have separate prototypes";
}

// A map's own prototype is reachable as a whole chain, not just one hop:
// mapPrototype given a parent must be found from a map receiver.
TEST_F(MapParentChainTest, IsInstanceOfWalksTheMapPrototypesOwnChain) {
    const ProtoObject* grand = context->newObject(false);
    space->mapPrototype = const_cast<ProtoObject*>(
        space->mapPrototype->addParent(context, grand));

    EXPECT_EQ(mapWord()->isInstanceOf(context, grand), PROTO_TRUE)
        << "the non-object branch of isInstanceOf walks the prototype's flattened chain";
}

TEST_F(MapParentChainTest, AttributeLookupOnAMapReachesTheMapPrototype) {
    const ProtoString* onMapProto = sym("onMapProto");
    // mapPrototype is IMMUTABLE: re-publish the handle setAttribute returns.
    space->mapPrototype = const_cast<ProtoObject*>(
        space->mapPrototype->setAttribute(context, onMapProto, context->fromInteger(7)));

    const ProtoObject* m = mapWord();
    EXPECT_EQ(m->getAttribute(context, onMapProto), context->fromInteger(7));
    EXPECT_EQ(m->hasAttribute(context, onMapProto), PROTO_TRUE);
    EXPECT_EQ(m->hasAttribute(context, sym("definitelyMissingOnMap")), PROTO_FALSE);

    const ProtoSparseList* attrs = m->getAttributes(context);
    ASSERT_NE(attrs, nullptr);
    EXPECT_EQ(attrs->getAt(context, reinterpret_cast<uintptr_t>(onMapProto)),
              context->fromInteger(7));
}

TEST_F(MapParentChainTest, NewChildOfAMapDelegatesToTheMapPrototype) {
    const ProtoObject* child = mapWord()->newChild(context, false);
    ASSERT_NE(child, nullptr);
    ASSERT_NE(child, PROTO_NONE);
    EXPECT_EQ(child->isInstanceOf(context, space->mapPrototype), PROTO_TRUE)
        << "newChild on a non-object receiver childs its prototype";
    EXPECT_FALSE(child->isMap(context)) << "the child is a plain object, not a map";
}

// --- The chain-entry side: a map stored AS a parent ----------------------

TEST_F(MapParentChainTest, AddParentAcceptsAMapAndFindsItsPrototypesAttributes) {
    const ProtoString* onMapProto = sym("onMapProto2");
    space->mapPrototype = const_cast<ProtoObject*>(
        space->mapPrototype->setAttribute(context, onMapProto, context->fromInteger(8)));

    const ProtoObject* m = mapWord();
    const ProtoObject* d = context->newObject(false)->addParent(context, m);

    EXPECT_EQ(d->hasParent(context, m), 1);
    EXPECT_EQ(d->isInstanceOf(context, m), PROTO_TRUE);
    // One hop through the non-object entry's own prototype, exactly as for a
    // heap string (NonObjectParentTests.cpp).
    EXPECT_EQ(d->getAttribute(context, onMapProto), context->fromInteger(8));
    EXPECT_EQ(d->hasAttribute(context, onMapProto), PROTO_TRUE);
    EXPECT_EQ(d->getAttributes(context)->getAt(context, reinterpret_cast<uintptr_t>(onMapProto)),
              context->fromInteger(8));
    EXPECT_EQ(d->getAttribute(context, sym("missingPastAMapParent")), PROTO_NONE)
        << "a not-found lookup past a map entry must terminate";
}

// flattenParentsOrder's non-object policy: a map entry is KEPT in the
// flattened chain but contributes no ancestors of its own (it has no object
// cell to walk), and the listed order around it is preserved.
TEST_F(MapParentChainTest, SetParentsKeepsAMapEntryAndTakesNoAncestorsFromIt) {
    const ProtoObject* grand = context->newObject(false);
    const ProtoObject* a = context->newObject(false)->addParent(context, grand);
    const ProtoObject* m = mapWord();

    const ProtoList* parents = context->newList()
        ->appendLast(context, m)
        ->appendLast(context, a);
    const ProtoObject* d = context->newObject(false)->setParents(context, parents);

    EXPECT_EQ(d->hasParent(context, m), 1) << "the map entry is kept, not filtered out";
    EXPECT_EQ(d->hasParent(context, a), 1);
    EXPECT_EQ(d->hasParent(context, grand), 1)
        << "flattening still pulls in the OBJECT parent's ancestors";

    const ProtoList* flat = d->getParents(context);
    ASSERT_EQ(flat->getSize(context), 3u)
        << "map, a, grand — the map adds nothing of its own to the chain";
    EXPECT_EQ(flat->getAt(context, 0), m);
    EXPECT_EQ(flat->getAt(context, 1), a);
    EXPECT_EQ(flat->getAt(context, 2), grand);
}

TEST_F(MapParentChainTest, MutableReceiverSeesAMapParentAddedAfterCreation) {
    const ProtoObject* m = mapWord();
    const ProtoObject* mutableObj = context->newObject(true);
    mutableObj->addParent(context, m);   // in-place for a mutable receiver

    EXPECT_EQ(mutableObj->isInstanceOf(context, m), PROTO_TRUE)
        << "isInstanceOf resolves the mutable snapshot before walking the chain";
    EXPECT_EQ(mutableObj->hasParent(context, m), 1);
}

// --- GC: a map reachable only as a chain entry stays alive ---------------

TEST(MapParentChainGC, AMapReachableOnlyAsAParentSurvivesForcedCollections) {
    ProtoSpace space;
    ProtoContext live(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    ProtoRootSet* rs = space.createRootSet("map-as-parent");
    ASSERT_NE(rs, nullptr);

    // Single-root pinning: only the child object is pinned; the map must be
    // kept alive through the ParentLink chain alone.
    ProtoRootSet::Handle pinned = ProtoRootSet::kNullHandle;
    const ProtoObject* mapWordCopy = nullptr;
    {
        ProtoContext sub(&space, &live, nullptr, nullptr, nullptr, nullptr);
        const ProtoMap* m = sub.newMap();
        for (int i = 0; i < 64; ++i) {
            const ProtoObject* key = sub.newList()->appendLast(&sub, sub.fromInteger(i))->asObject(&sub);
            m = m->setAt(&sub, key, sub.fromInteger(i * 10));
        }
        mapWordCopy = m->asObject(&sub);
        pinned = rs->add(sub.newObject(false)->addParent(&sub, mapWordCopy));
    }

    const uint64_t cycles = forceCycles(space, &live, 5);
    ASSERT_TRUE(waitForIdleCollector(space, &live));
    EXPECT_GE(cycles, 5u);

    const ProtoObject* d = rs->resolve(pinned);
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->hasParent(&live, mapWordCopy), 1);
    const ProtoMap* survived = mapWordCopy->asMap(&live);
    ASSERT_NE(survived, nullptr);
    EXPECT_EQ(survived->getSize(&live), 64u)
        << "the map was reachable only through the parent chain and must have survived";

    rs->remove(pinned);
    space.destroyRootSet(rs);
}
