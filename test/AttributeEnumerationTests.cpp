// AttributeEnumerationTests.cpp — ProtoObject::processOwnAttributes.
//
// Attribute keys are stored as the interned symbol pointer reinterpreted as
// an integer, so `getOwnAttributes()` hands an embedder a sparse list whose
// keys are opaque numbers: the values are reachable but the NAMES are not.
// `processOwnAttributes` is the supported way to walk own attributes as
// (name, value) pairs.  These tests pin down the contract:
//
//   * every own attribute is reported exactly once, removed ones are not,
//     and a PROTO_NONE value is a value like any other;
//   * the reported name is the very symbol the attribute was set with, so
//     it compares equal by identity and feeding it back to `getAttribute`
//     returns the value the callback received;
//   * the walk allocates nothing, and runs the callback OUTSIDE any GC
//     critical section — so the callback may allocate and safepoint;
//   * a concurrent writer on another thread cannot make the walk crash or
//     make it report a torn view.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace proto;

namespace {

// Callback sink.  Records the (name, value) pairs in visit order; the walk
// documents its order as unspecified, so every assertion below is written
// against the SET of pairs, never their sequence.
struct Collector {
    std::vector<const ProtoString*> names;
    std::vector<const ProtoObject*> values;
    int calls = 0;
};

void collect(ProtoContext*, void* self, const ProtoString* name, const ProtoObject* value) {
    auto* c = static_cast<Collector*>(self);
    c->names.push_back(name);
    c->values.push_back(value);
    ++c->calls;
}

bool hasName(const Collector& c, const ProtoString* name) {
    return std::find(c.names.begin(), c.names.end(), name) != c.names.end();
}

const ProtoObject* valueOf(const Collector& c, const ProtoString* name) {
    for (size_t i = 0; i < c.names.size(); ++i) {
        if (c.names[i] == name) return c.values[i];
    }
    return nullptr;
}

}  // namespace

class AttributeEnumerationTest : public ::testing::Test {
protected:
    ProtoSpace* space = nullptr;
    ProtoContext* ctx = nullptr;

    void SetUp() override {
        space = new ProtoSpace();
        ctx = space->rootContext;
    }
    void TearDown() override { delete space; }

    const ProtoString* sym(const char* s) { return ProtoString::createSymbol(ctx, s); }
};

// An object with no own attributes never invokes the callback — neither in
// the immutable nor in the mutable form.
TEST_F(AttributeEnumerationTest, NoOwnAttributesNeverInvokesTheCallback) {
    Collector immutableSink;
    ctx->newObject(false)->processOwnAttributes(ctx, &immutableSink, collect);
    EXPECT_EQ(immutableSink.calls, 0);

    Collector mutableSink;
    ctx->newObject(true)->processOwnAttributes(ctx, &mutableSink, collect);
    EXPECT_EQ(mutableSink.calls, 0);
}

// A handful of own attributes on an IMMUTABLE object: each is reported once,
// with the symbol it was set with and the value it was set to.
TEST_F(AttributeEnumerationTest, ImmutableObjectReportsEachOwnAttributeOnce) {
    const ProtoString* a = sym("alpha");
    const ProtoString* b = sym("beta");
    const ProtoString* c = sym("gamma");
    const ProtoObject* va = ctx->fromInteger(1);
    const ProtoObject* vb = ctx->fromInteger(2);
    const ProtoObject* vc = ctx->fromInteger(3);

    // Each setAttribute on an immutable receiver returns a NEW object.
    const ProtoObject* obj = ctx->newObject(false);
    obj = obj->setAttribute(ctx, a, va);
    obj = obj->setAttribute(ctx, b, vb);
    obj = obj->setAttribute(ctx, c, vc);

    Collector sink;
    obj->processOwnAttributes(ctx, &sink, collect);

    EXPECT_EQ(sink.calls, 3);
    EXPECT_EQ(valueOf(sink, a), va);
    EXPECT_EQ(valueOf(sink, b), vb);
    EXPECT_EQ(valueOf(sink, c), vc);
}

// A MUTABLE object after several setAttribute calls, one of which was later
// removed: the survivors are reported, the removed name is not.
TEST_F(AttributeEnumerationTest, MutableObjectReportsLiveAttributesNotRemovedOnes) {
    auto* obj = const_cast<ProtoObject*>(ctx->newObject(true));
    const ProtoString* keep1 = sym("keep_one");
    const ProtoString* keep2 = sym("keep_two");
    const ProtoString* keep3 = sym("keep_three");
    const ProtoString* gone  = sym("removed_name");

    obj->setAttribute(ctx, keep1, ctx->fromInteger(10));
    obj->setAttribute(ctx, gone,  ctx->fromInteger(99));
    obj->setAttribute(ctx, keep2, ctx->fromInteger(20));
    obj->setAttribute(ctx, keep3, ctx->fromInteger(30));
    // Overwrite one of them — it must still be reported exactly once.
    obj->setAttribute(ctx, keep2, ctx->fromInteger(21));
    obj->removeAttribute(ctx, gone);

    Collector sink;
    obj->processOwnAttributes(ctx, &sink, collect);

    EXPECT_EQ(sink.calls, 3) << "a removed attribute must not be reported";
    EXPECT_FALSE(hasName(sink, gone));
    EXPECT_EQ(valueOf(sink, keep1)->asLong(ctx), 10);
    EXPECT_EQ(valueOf(sink, keep2)->asLong(ctx), 21) << "must report the current value";
    EXPECT_EQ(valueOf(sink, keep3)->asLong(ctx), 30);
}

// More attributes than any inline form could hold, so the attribute store is
// a deep AVL tree: every entry is still reported exactly once.
TEST_F(AttributeEnumerationTest, DeepTreeReportsEveryAttributeExactlyOnce) {
    constexpr int kCount = 200;
    auto* obj = const_cast<ProtoObject*>(ctx->newObject(true));
    std::vector<const ProtoString*> keys;
    keys.reserve(kCount);

    for (int i = 0; i < kCount; ++i) {
        const ProtoString* k = sym(("attribute_number_" + std::to_string(i)).c_str());
        keys.push_back(k);
        obj->setAttribute(ctx, k, ctx->fromInteger(i));
    }

    Collector sink;
    obj->processOwnAttributes(ctx, &sink, collect);

    ASSERT_EQ(sink.calls, kCount);
    for (int i = 0; i < kCount; ++i) {
        EXPECT_EQ(valueOf(sink, keys[i])->asLong(ctx), i) << "at index " << i;
    }
    // Exactly once each: no duplicate names in the visit list.
    std::vector<const ProtoString*> sorted = sink.names;
    std::sort(sorted.begin(), sorted.end());
    EXPECT_EQ(std::adjacent_find(sorted.begin(), sorted.end()), sorted.end())
        << "an attribute was reported more than once";
}

// Names that are short pure-ASCII strings are inline-string handles, while
// longer names are heap symbols.  Both forms must come back as the very
// pointer the attribute was set with.
TEST_F(AttributeEnumerationTest, ReportsInlineAndHeapSymbolNamesByIdentity) {
    // <= 6 ASCII bytes: createSymbol returns an inline-string handle.
    const ProtoString* shortName = sym("ab");
    const ProtoString* sixChars  = sym("abcdef");
    // Well past the inline limit: a heap symbol.
    const ProtoString* longName =
        sym("a_considerably_longer_attribute_name_than_six_bytes");

    auto* obj = const_cast<ProtoObject*>(ctx->newObject(true));
    obj->setAttribute(ctx, shortName, ctx->fromInteger(1));
    obj->setAttribute(ctx, sixChars,  ctx->fromInteger(2));
    obj->setAttribute(ctx, longName,  ctx->fromInteger(3));

    Collector sink;
    obj->processOwnAttributes(ctx, &sink, collect);

    ASSERT_EQ(sink.calls, 3);
    // Identity, not just equality: the callback gets the canonical symbol.
    EXPECT_TRUE(hasName(sink, shortName));
    EXPECT_TRUE(hasName(sink, sixChars));
    EXPECT_TRUE(hasName(sink, longName));
    EXPECT_EQ(valueOf(sink, longName)->asLong(ctx), 3);

    // The names are usable as strings, and re-interning by content yields
    // the same pointer the walk reported.
    const ProtoString* reInterned = sym("a_considerably_longer_attribute_name_than_six_bytes");
    EXPECT_EQ(reInterned, longName);
    EXPECT_TRUE(hasName(sink, reInterned));
}

// PROTO_NONE is a value, not an absence: an attribute holding it is reported.
TEST_F(AttributeEnumerationTest, AttributeValuedProtoNoneIsReported) {
    auto* obj = const_cast<ProtoObject*>(ctx->newObject(true));
    const ProtoString* none = sym("none_valued");
    const ProtoString* real = sym("int_valued");
    obj->setAttribute(ctx, none, PROTO_NONE);
    obj->setAttribute(ctx, real, ctx->fromInteger(7));

    Collector sink;
    obj->processOwnAttributes(ctx, &sink, collect);

    EXPECT_EQ(sink.calls, 2);
    EXPECT_TRUE(hasName(sink, none));
    EXPECT_EQ(valueOf(sink, none), PROTO_NONE);
}

// Every reported name, fed back to getAttribute, returns the value that was
// handed to the callback.  This is the property that lets an embedder copy
// or stamp an object's own attributes.
TEST_F(AttributeEnumerationTest, ReportedNamesRoundTripThroughGetAttribute) {
    auto* obj = const_cast<ProtoObject*>(ctx->newObject(true));
    obj->setAttribute(ctx, sym("one"), ctx->fromInteger(1));
    obj->setAttribute(ctx, sym("a_long_attribute_name_for_the_heap"), ctx->fromInteger(2));
    obj->setAttribute(ctx, sym("none"), PROTO_NONE);

    Collector sink;
    obj->processOwnAttributes(ctx, &sink, collect);

    ASSERT_EQ(sink.calls, 3);
    for (size_t i = 0; i < sink.names.size(); ++i) {
        EXPECT_EQ(obj->getAttribute(ctx, sink.names[i]), sink.values[i])
            << "getAttribute disagreed with the walk for entry " << i;
        EXPECT_EQ(obj->getOwnAttributeDirect(ctx, sink.names[i]), sink.values[i]);
    }
}

// The walk itself creates no Cell: allocatedCellsCount is unchanged by it.
TEST_F(AttributeEnumerationTest, WalkAllocatesNothing) {
    auto* mutableObj = const_cast<ProtoObject*>(ctx->newObject(true));
    const ProtoObject* immutableObj = ctx->newObject(false);
    for (int i = 0; i < 40; ++i) {
        const ProtoString* k = sym(("alloc_probe_" + std::to_string(i)).c_str());
        mutableObj->setAttribute(ctx, k, ctx->fromInteger(i));
        immutableObj = immutableObj->setAttribute(ctx, k, ctx->fromInteger(i));
    }

    Collector sink;
    const unsigned long beforeMutable = ctx->allocatedCellsCount;
    mutableObj->processOwnAttributes(ctx, &sink, collect);
    EXPECT_EQ(ctx->allocatedCellsCount, beforeMutable)
        << "walking a mutable object must not allocate";
    EXPECT_EQ(sink.calls, 40);

    Collector sink2;
    const unsigned long beforeImmutable = ctx->allocatedCellsCount;
    immutableObj->processOwnAttributes(ctx, &sink2, collect);
    EXPECT_EQ(ctx->allocatedCellsCount, beforeImmutable)
        << "walking an immutable object must not allocate";
    EXPECT_EQ(sink2.calls, 40);
}

namespace {
// A callback that does everything the contract permits: allocates cells and
// reaches a safepoint.  If the walk held a critical section across the
// callback this would deadlock the collector or trip its assertions; if the
// walk left its nodes un-anchored, the collection could sweep them.
struct AllocatingSink {
    ProtoSpace* space = nullptr;
    int calls = 0;
    std::vector<const ProtoString*> names;
};

void allocateAndSafepoint(ProtoContext* context, void* self,
                          const ProtoString* name, const ProtoObject* value) {
    auto* s = static_cast<AllocatingSink*>(self);
    ++s->calls;
    s->names.push_back(name);
    // Allocate a few cells from inside the callback.
    const ProtoObject* scratch = context->newObject(true);
    const_cast<ProtoObject*>(scratch)->setAttribute(
        context, ProtoString::createSymbol(context, "scratch_slot"), value);
    context->safepoint();
}
}  // namespace

// The callback runs outside any critical section: it may allocate, trigger
// collections and park at a safepoint, and the walk still completes and
// reports every attribute.
TEST_F(AttributeEnumerationTest, CallbackMayAllocateAndSafepoint) {
    constexpr int kCount = 60;
    auto* obj = const_cast<ProtoObject*>(ctx->newObject(true));
    std::vector<const ProtoString*> keys;
    for (int i = 0; i < kCount; ++i) {
        const ProtoString* k = sym(("safepoint_probe_" + std::to_string(i)).c_str());
        keys.push_back(k);
        obj->setAttribute(ctx, k, ctx->fromInteger(i));
    }

    AllocatingSink sink;
    sink.space = space;
    obj->processOwnAttributes(ctx, &sink, allocateAndSafepoint);

    EXPECT_EQ(sink.calls, kCount);
    for (const ProtoString* k : keys) {
        EXPECT_NE(std::find(sink.names.begin(), sink.names.end(), k), sink.names.end());
    }
}

namespace {
// Concurrent sink: verifies on the fly that nothing torn is ever handed to
// the callback, and keeps the walk honest by touching every value.
struct ConcurrentSink {
    ProtoContext* context = nullptr;
    int calls = 0;
    unsigned long nullValues = 0;
    unsigned long badValues = 0;
};

void checkEachPair(ProtoContext* context, void* self,
                   const ProtoString* name, const ProtoObject* value) {
    auto* s = static_cast<ConcurrentSink*>(self);
    ++s->calls;
    if (name == nullptr || value == nullptr) { ++s->nullValues; return; }
    // Every value this test ever stores is an integer; anything else means
    // the walk handed back a torn or recycled cell.
    if (!value->isInteger(context)) ++s->badValues;
}
}  // namespace

// Another thread mutates the same MUTABLE object — and the collector runs —
// while the walk is in progress.  The walk must never crash and must never
// report a torn pair: it reports the snapshot it resolved when it started.
TEST_F(AttributeEnumerationTest, ConcurrentMutationDuringWalkIsSafe) {
    constexpr int kAttributes = 48;
    auto* obj = const_cast<ProtoObject*>(ctx->newObject(true));
    std::vector<const ProtoString*> keys;
    for (int i = 0; i < kAttributes; ++i) {
        const ProtoString* k = sym(("shared_slot_" + std::to_string(i)).c_str());
        keys.push_back(k);
        obj->setAttribute(ctx, k, ctx->fromInteger(i));
    }

    std::atomic<bool> stop{false};
    std::atomic<unsigned long> writes{0};

    // Writer: keeps replacing values, so the snapshot a walk started from is
    // orphaned again and again while that walk is still running.  `writes`
    // counts INDIVIDUAL setAttribute calls, so the main thread can tell
    // quickly — and deterministically — that the writer is really running.
    std::thread writer([&]() {
        ProtoContext writerCtx{space};
        unsigned long n = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            for (size_t i = 0; i < keys.size(); ++i) {
                obj->setAttribute(&writerCtx, keys[i],
                                  writerCtx.fromInteger(static_cast<long long>(1000 + n)));
                writes.fetch_add(1, std::memory_order_relaxed);
            }
            ++n;
        }
    });

    // Collector: forces cycles that would sweep an orphaned snapshot.
    std::thread gcKicker([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            space->triggerGC();
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    // Do not start walking until the writer is demonstrably running: on a
    // loaded machine the main thread can otherwise finish every round before
    // the writer is ever scheduled, and the test would prove nothing.
    const auto startDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (writes.load(std::memory_order_relaxed) == 0 &&
           std::chrono::steady_clock::now() < startDeadline) {
        std::this_thread::yield();
    }

    // Walk at least kMinRounds times AND keep going until the writer has
    // landed kMinWrites mutations while we were walking, so the overlap the
    // test claims to exercise actually happened.  The deadline keeps a
    // starved writer from hanging the suite.
    constexpr int kMinRounds = 400;
    constexpr unsigned long kMinWrites = 500;
    const unsigned long writesBefore = writes.load(std::memory_order_relaxed);
    const auto roundsDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);

    unsigned long totalWalks = 0;
    unsigned long shortWalks = 0;
    for (int round = 0;
         round < kMinRounds ||
         (writes.load(std::memory_order_relaxed) - writesBefore < kMinWrites &&
          std::chrono::steady_clock::now() < roundsDeadline);
         ++round) {
        ConcurrentSink sink;
        sink.context = ctx;
        obj->processOwnAttributes(ctx, &sink, checkEachPair);
        ++totalWalks;
        EXPECT_EQ(sink.nullValues, 0u) << "the walk reported a null name or value";
        EXPECT_EQ(sink.badValues, 0u) << "the walk reported a torn or recycled value";
        // The attribute SET never changes (only the values do), so every
        // walk must see the full set.
        if (sink.calls != kAttributes) ++shortWalks;
        ctx->safepoint();
    }

    const unsigned long writesDuringWalks =
        writes.load(std::memory_order_relaxed) - writesBefore;

    stop.store(true, std::memory_order_relaxed);
    writer.join();
    gcKicker.join();

    EXPECT_EQ(shortWalks, 0u)
        << "a walk saw an inconsistent number of attributes out of " << totalWalks;
    EXPECT_GT(writesDuringWalks, 0u)
        << "the writer never ran while we walked; the test proved nothing";
}

// Non-object receivers have no own attributes: the callback is never called
// and nothing crashes.
TEST_F(AttributeEnumerationTest, NonObjectReceiversReportNothing) {
    Collector sink;
    ctx->fromInteger(42)->processOwnAttributes(ctx, &sink, collect);
    ctx->fromUTF8String("a string receiver")->processOwnAttributes(ctx, &sink, collect);
    PROTO_NONE->processOwnAttributes(ctx, &sink, collect);
    PROTO_TRUE->processOwnAttributes(ctx, &sink, collect);
    EXPECT_EQ(sink.calls, 0);
}

// A null callback (or a null context) is ignored rather than crashing.
TEST_F(AttributeEnumerationTest, NullArgumentsAreIgnored) {
    auto* obj = const_cast<ProtoObject*>(ctx->newObject(true));
    obj->setAttribute(ctx, sym("slot"), ctx->fromInteger(1));
    obj->processOwnAttributes(ctx, nullptr, nullptr);
    Collector sink;
    obj->processOwnAttributes(nullptr, &sink, collect);
    EXPECT_EQ(sink.calls, 0);
}

// ---------------------------------------------------------------------------
// ProtoObject::clone — carries the CURRENT own attributes, not the birth ones.
//
// A mutable object never writes back into its handle cell: setAttribute
// publishes a new state cell into the mutable shard table and returns the same
// handle.  clone used to copy the handle's own `attributes` pointer, so a
// clone of a mutable object came back with whatever it was born with — for a
// freshly created object, nothing at all.
// ---------------------------------------------------------------------------

class CloneOwnAttributesTest : public ::testing::Test {
protected:
    ProtoSpace* space = nullptr;
    ProtoContext* ctx = nullptr;

    void SetUp() override {
        space = new ProtoSpace();
        ctx = space->rootContext;
    }
    void TearDown() override { delete space; }

    const ProtoString* sym(const char* s) { return ProtoString::createSymbol(ctx, s); }
};

// The reported defect: protoCore.ImmutableObject({a:1}) came back as {}.
// Cloning a MUTABLE object must carry the attributes it holds now.
TEST_F(CloneOwnAttributesTest, CloneOfMutableCarriesCurrentOwnAttributes) {
    auto* source = const_cast<ProtoObject*>(ctx->newObject(true));
    const ProtoString* a = sym("a");
    const ProtoString* longName = sym("a_longer_attribute_name");
    source->setAttribute(ctx, a, ctx->fromInteger(1));
    source->setAttribute(ctx, longName, ctx->fromInteger(2));

    const ProtoObject* copy = source->clone(ctx, false);

    ASSERT_NE(copy, PROTO_NONE);
    EXPECT_EQ(copy->getAttribute(ctx, a)->asLong(ctx), 1)
        << "the clone lost an attribute written after the object was created";
    EXPECT_EQ(copy->getAttribute(ctx, longName)->asLong(ctx), 2);

    // And it answers the same own attributes as its source.
    Collector fromSource, fromCopy;
    source->processOwnAttributes(ctx, &fromSource, collect);
    copy->processOwnAttributes(ctx, &fromCopy, collect);
    EXPECT_EQ(fromCopy.calls, fromSource.calls);
    for (size_t i = 0; i < fromSource.names.size(); ++i) {
        EXPECT_EQ(valueOf(fromCopy, fromSource.names[i]), fromSource.values[i]);
    }
}

// The immutable path must keep working: there the handle cell IS the state.
TEST_F(CloneOwnAttributesTest, CloneOfImmutableCarriesOwnAttributes) {
    const ProtoString* k = sym("frozen_slot");
    const ProtoObject* source = ctx->newObject(false)->setAttribute(ctx, k, ctx->fromInteger(42));

    const ProtoObject* copy = source->clone(ctx, false);

    EXPECT_EQ(copy->getAttribute(ctx, k)->asLong(ctx), 42);
    Collector sink;
    copy->processOwnAttributes(ctx, &sink, collect);
    EXPECT_EQ(sink.calls, 1);
    EXPECT_TRUE(hasName(sink, k));
}

// Thawing: clone(ctx, true) carries the attributes AND is independent — a
// write to the copy must not be visible through the source, or vice versa.
TEST_F(CloneOwnAttributesTest, MutableCloneIsIndependentOfItsSource) {
    auto* source = const_cast<ProtoObject*>(ctx->newObject(true));
    const ProtoString* k = sym("slot");
    source->setAttribute(ctx, k, ctx->fromInteger(1));

    auto* copy = const_cast<ProtoObject*>(source->clone(ctx, true));
    ASSERT_NE(copy, PROTO_NONE);
    EXPECT_EQ(copy->getAttribute(ctx, k)->asLong(ctx), 1);

    copy->setAttribute(ctx, k, ctx->fromInteger(99));
    EXPECT_EQ(copy->getAttribute(ctx, k)->asLong(ctx), 99);
    EXPECT_EQ(source->getAttribute(ctx, k)->asLong(ctx), 1)
        << "writing to the clone must not be visible through its source";

    source->setAttribute(ctx, k, ctx->fromInteger(7));
    EXPECT_EQ(copy->getAttribute(ctx, k)->asLong(ctx), 99)
        << "writing to the source must not be visible through the clone";
}

// The clone is a sibling: it keeps the parents of the CURRENT snapshot, and
// the receiver does not become its parent.
TEST_F(CloneOwnAttributesTest, CloneKeepsTheParentsOfTheCurrentSnapshot) {
    const ProtoObject* prototype = ctx->newObject(false);
    auto* child = const_cast<ProtoObject*>(prototype->newChild(ctx, true));
    const ProtoString* k = sym("own_slot");
    child->setAttribute(ctx, k, ctx->fromInteger(5));

    const ProtoObject* copy = child->clone(ctx, false);

    ASSERT_NE(copy, PROTO_NONE);
    EXPECT_EQ(copy->getAttribute(ctx, k)->asLong(ctx), 5);
    EXPECT_EQ(copy->isInstanceOf(ctx, prototype), PROTO_TRUE)
        << "the clone lost the parent chain of its source";
    EXPECT_EQ(copy->getFirstParent(ctx), child->getFirstParent(ctx));
    // isInstanceOf answers PROTO_TRUE when the prototype is found in the
    // chain and PROTO_NONE — not PROTO_FALSE — when it is not, so assert
    // against the positive answer rather than one particular negative one.
    EXPECT_NE(copy->isInstanceOf(ctx, child), PROTO_TRUE)
        << "clone must be a sibling, not a child, of its source";
}
