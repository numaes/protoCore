#include <gtest/gtest.h>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>
#include "../headers/protoCore.h"

// Test fixture for ProtoTuple tests
class TupleTest : public ::testing::Test {
protected:
    proto::ProtoSpace* space = nullptr;
    proto::ProtoContext* context;

    void SetUp() override {
        space = new proto::ProtoSpace();
        context = space->rootContext;
    }
};

TEST_F(TupleTest, CreationAndSize) {
    const proto::ProtoTuple* tuple = context->newTuple();
    ASSERT_TRUE(tuple != nullptr);
    ASSERT_EQ(tuple->getSize(context), 0);
}

TEST_F(TupleTest, CreationFromList) {
    const proto::ProtoList* list = context->newList();
    list = list->appendLast(context, context->fromInteger(10));
    list = list->appendLast(context, context->fromInteger(20));

    const proto::ProtoTuple* tuple = context->newTupleFromList(list);
    ASSERT_EQ(tuple->getSize(context), 2);
    ASSERT_EQ(tuple->getAt(context, 0)->asLong(context), 10);
    ASSERT_EQ(tuple->getAt(context, 1)->asLong(context), 20);
}

TEST_F(TupleTest, GetAt) {
    const proto::ProtoList* list = context->newList();
    list = list->appendLast(context, context->fromInteger(10));
    list = list->appendLast(context, context->fromInteger(20));
    const proto::ProtoTuple* tuple = context->newTupleFromList(list);

    ASSERT_EQ(tuple->getAt(context, 0)->asLong(context), 10);
    ASSERT_EQ(tuple->getAt(context, 1)->asLong(context), 20);
}

TEST_F(TupleTest, Interning) {
    // Create two separate lists with the same content
    const proto::ProtoList* list1 = context->newList()->appendLast(context, context->fromInteger(1));
    list1 = list1->appendLast(context, context->fromInteger(2));

    const proto::ProtoList* list2 = context->newList()->appendLast(context, context->fromInteger(1));
    list2 = list2->appendLast(context, context->fromInteger(2));

    // Create tuples from them; cast needed as newTupleFromList returns const
    const proto::ProtoTuple* tuple1 = context->newTupleFromList(list1);
    const proto::ProtoTuple* tuple2 = context->newTupleFromList(list2);

    // Because of interning, they should be the exact same object in memory
    ASSERT_EQ(tuple1, tuple2);

    // A different tuple should be a different object
    const proto::ProtoList* list3 = context->newList()->appendLast(context, context->fromInteger(99));
    const proto::ProtoTuple* tuple3 = context->newTupleFromList(list3);
    ASSERT_NE(tuple1, tuple3);
}

TEST_F(TupleTest, GetSlice) {
    const proto::ProtoList* list = context->newList();
    for (int i = 0; i < 10; ++i) {
        list = list->appendLast(context, context->fromInteger(i));
    }
    const proto::ProtoTuple* tuple = context->newTupleFromList(list);

    const proto::ProtoTuple* slice = (const proto::ProtoTuple*)tuple->getSlice(context, 2, 5);
    ASSERT_EQ(slice->getSize(context), 3);
    ASSERT_EQ(slice->getAt(context, 0)->asLong(context), 2);
    ASSERT_EQ(slice->getAt(context, 1)->asLong(context), 3);
    ASSERT_EQ(slice->getAt(context, 2)->asLong(context), 4);

    // Test that the slice is also interned correctly
    const proto::ProtoTuple* slice2 = (const proto::ProtoTuple*)tuple->getSlice(context, 2, 5);
    ASSERT_EQ(slice, slice2);
}

TEST_F(TupleTest, EmptyTupleGetFirstGetLast) {
    const proto::ProtoTuple* tuple = context->newTuple();
    ASSERT_EQ(tuple->getSize(context), 0);
    ASSERT_EQ(tuple->getFirst(context), PROTO_NONE);
    ASSERT_EQ(tuple->getLast(context), PROTO_NONE);
}

TEST_F(TupleTest, TupleHas) {
    const proto::ProtoList* list = context->newList();
    list = list->appendLast(context, context->fromInteger(10));
    list = list->appendLast(context, context->fromInteger(20));
    const proto::ProtoTuple* tuple = context->newTupleFromList(list);
    const proto::ProtoObject* ten = context->fromInteger(10);
    const proto::ProtoObject* twenty = context->fromInteger(20);
    const proto::ProtoObject* ninety = context->fromInteger(90);
    ASSERT_TRUE(tuple->has(context, ten));
    ASSERT_TRUE(tuple->has(context, twenty));
    ASSERT_FALSE(tuple->has(context, ninety));
}

namespace {

const proto::ProtoTuple* pair(proto::ProtoContext* ctx, const proto::ProtoObject* a, const proto::ProtoObject* b) {
    return ctx->newTupleFromList(ctx->newList()->appendLast(ctx, a)->appendLast(ctx, b));
}

// triggerGC() starts a cycle only under heap pressure, and the stop-the-world
// quorum needs this thread to park, so request each cycle directly and reach
// safepoints until it completes (as GCSurvivorRechainTests does).
void runGcCycles(proto::ProtoSpace* space, int cycles) {
    proto::ProtoContext* ctx = space->rootContext;
    for (int i = 0; i < cycles; ++i) {
        {
            std::lock_guard<std::recursive_mutex> lock(proto::ProtoSpace::globalMutex);
            space->gcStarted = true;
            space->gcCV.notify_all();
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (space->gcStarted.load() && std::chrono::steady_clock::now() < deadline) {
            ctx->safepoint();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // gcStarted is cleared before sweep finishes; give sweep room.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

}  // namespace

TEST_F(TupleTest, InterningManyDistinctTuplesIsNotQuadratic) {
    // The interner was a binary search tree that was never rebalanced, walked
    // twice per tuple under the global mutex. Tuples whose element pointers
    // grow (small integers in order) degenerated it into a linked list, so
    // interning N distinct tuples took O(N^2) time.
    constexpr int kTuples = 100000;
    const proto::ProtoObject* tag = context->fromUTF8String("k");
    const proto::ProtoTuple* first = pair(context, context->fromInteger(0), tag);
    const auto start = std::chrono::steady_clock::now();
    for (int i = 1; i < kTuples; ++i) {
        pair(context, context->fromInteger(i), tag);
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    EXPECT_LT(seconds, 30.0) << "interning " << kTuples << " distinct tuples took " << seconds << " s";
    EXPECT_EQ(pair(context, context->fromInteger(0), tag), first);
    EXPECT_EQ(pair(context, context->fromInteger(kTuples - 1), tag),
              pair(context, context->fromInteger(kTuples - 1), tag));
}

TEST_F(TupleTest, InternedTuplesArePerennialAndKeepTheirElementsAlive) {
    // Interned tuples are never collected: the interner is a GC root. A tuple
    // and the heap objects it references outlive the context that built them,
    // and building the tuple again returns the same object.
    const proto::ProtoObject* element = nullptr;
    const proto::ProtoTuple* tuple = nullptr;
    {
        proto::ProtoContext scratch{space};
        element = scratch.newList()->appendLast(&scratch, scratch.fromInteger(4242))->asObject(&scratch);
        tuple = pair(&scratch, element, scratch.fromInteger(7));
    }
    // The scratch context's cells are collection candidates now: only the
    // interner references the tuple, and only the tuple references `element`.
    runGcCycles(space, 3);
    // Reuse whatever the GC freed, so a collected element would be overwritten.
    for (int i = 0; i < 20000; ++i) {
        context->newList()->appendLast(context, context->fromInteger(-1));
    }
    ASSERT_EQ(element->asList(context)->getSize(context), 1u);
    EXPECT_EQ(element->asList(context)->getAt(context, 0)->asLong(context), 4242);
    EXPECT_EQ(pair(context, element, context->fromInteger(7)), tuple);
}

TEST_F(TupleTest, ConcurrentInterningYieldsOneObjectPerTuple) {
    // Threads interning the same tuples at the same time, in opposite orders,
    // must all get one object per distinct tuple.
    constexpr int kThreads = 4;
    constexpr int kKeys = 20000;
    const proto::ProtoObject* tag = context->fromUTF8String("t");
    std::vector<std::vector<const proto::ProtoTuple*>> results(
        kThreads, std::vector<const proto::ProtoTuple*>(kKeys, nullptr));
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t]() {
            proto::ProtoContext threadCtx{space};
            for (int n = 0; n < kKeys; ++n) {
                const int k = (t % 2 == 0) ? n : kKeys - 1 - n;
                results[t][k] = pair(&threadCtx, threadCtx.fromInteger(k), tag);
            }
        });
    }
    for (auto& worker : workers) worker.join();
    for (int k = 0; k < kKeys; ++k) {
        for (int t = 1; t < kThreads; ++t) {
            ASSERT_EQ(results[t][k], results[0][k]) << "tuple " << k << " was interned twice";
        }
        ASSERT_EQ(results[0][k]->getAt(context, 0)->asLong(context), k);
    }
}
