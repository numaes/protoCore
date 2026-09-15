// HeapLimitBatchTests.cpp — per-thread refill batches under a heap limit.
//
// A ProtoThread allocates from a private freelist that getFreeCells refills
// in batches.  Those cells count against the heap limit as soon as they are
// handed out, but no cycle can reclaim the cells a thread holds.  Without a
// limit the batch stays as it was (blocksPerAllocation, or 60,000-65,536 cells
// with several running threads; a recycled chunk hands out up to
// CELL_CHUNK_SIZE).  With a hard limit, each refill is capped at
// maxHeapSize / (8 x runningThreads) cells (never below a small floor), so the
// running threads' batches together use about one eighth of the limit.
//
// The cap is computed from runningThreads at each refill, and that count dips
// while a thread waits for heap headroom (the waiting thread holds almost
// nothing), so an individual thread may briefly hold more than one eighth
// divided by the final thread count.  What must stay bounded is the SUM of
// the cells all threads hold.
//
// These tests check that:
//   * under a 500,000-cell limit with 8 threads that start together, the sum
//     of the cells held in the threads' freelists stays within a quarter of
//     the limit (one eighth plus room for those dips) -- before the change it
//     was up to 8 x 65,536 cells, the whole limit -- and a small live set
//     never reaches the out-of-memory path;
//   * without a limit, a refill still hands out at least a full chunk.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace proto;

namespace {

constexpr int kMaxWorkers = 8;

unsigned long heldCells(ProtoContext* ctx) {
    unsigned long n = 0;
    for (Cell* c = toImpl<ProtoThreadImplementation>(ctx->thread)->extension->freeCells; c; c = c->getNext()) ++n;
    return n;
}

struct BatchShared {
    int rounds = 0;
    int garbagePerRound = 0;
    std::atomic<int> nextIndex{0};
    std::atomic<int> started{0};
    std::atomic<bool> go{false};
    std::atomic<int> done{0};
    std::atomic<bool> release{false};
    std::atomic<unsigned long> held[kMaxWorkers];
    std::atomic<unsigned long> maxHeld{0};
    std::atomic<unsigned long> errors{0};
    BatchShared() { for (auto& h : held) h.store(0); }
};
BatchShared* gBatch = nullptr;

void recordMax(std::atomic<unsigned long>& slot, unsigned long v) {
    unsigned long cur = slot.load();
    while (v > cur && !slot.compare_exchange_weak(cur, v)) {}
}

// Waits (unmanaged) for the common start, keeps a small live list in its own
// context, churns garbage in short-lived children, and publishes the length
// of its freelist after every round.
const ProtoObject* batchThreadMain(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                                   const ProtoList*, const ProtoSparseList*) {
    BatchShared& shared = *gBatch;
    const int index = shared.nextIndex.fetch_add(1);
    ProtoThread* self = const_cast<ProtoThread*>(ctx->thread);
    shared.started.fetch_add(1);
    {
        ProtoContext::UnmanagedScope parked(ctx);
        while (!shared.go.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const ProtoList* live = ctx->newList();
    for (int i = 0; i < 500; ++i) live = live->appendLast(ctx, ctx->fromInteger(i));
    for (int round = 0; round < shared.rounds; ++round) {
        {
            ProtoContext garbage(ctx->space, ctx, nullptr, nullptr, nullptr, nullptr);
            for (int i = 0; i < shared.garbagePerRound; ++i) (void) garbage.newObject(false);
        }
        const unsigned long held = heldCells(ctx);
        shared.held[index].store(held);
        recordMax(shared.maxHeld, held);
        self->synchToGC();
    }
    shared.held[index].store(0);
    if (live->getSize(ctx) != 500) shared.errors.fetch_add(1);
    for (int i = 0; i < 500; ++i) {
        if (live->getAt(ctx, i)->asLong(ctx) != i) { shared.errors.fetch_add(1); break; }
    }
    shared.done.fetch_add(1);
    ProtoContext::UnmanagedScope parked(ctx);
    while (!shared.release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return PROTO_NONE;
}

// Starts the workers, releases them together, and samples the sum of the
// cells they hold until all are done.  Returns the largest sum observed.
unsigned long runThreads(ProtoSpace& space, int threads, BatchShared& shared) {
    ProtoContext* root = space.rootContext;
    gBatch = &shared;
    std::vector<const ProtoThread*> handles;
    for (int t = 0; t < threads; ++t) {
        handles.push_back(space.newThread(root, ProtoString::createSymbol(root, "batch-worker"),
                                          batchThreadMain, nullptr, nullptr));
    }
    unsigned long maxSum = 0;
    {
        ProtoContext::UnmanagedScope parked(root);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
        while (shared.started.load() < threads && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        shared.go = true;
        while (shared.done.load() < threads && std::chrono::steady_clock::now() < deadline) {
            unsigned long sum = 0;
            for (int t = 0; t < threads; ++t) sum += shared.held[t].load();
            if (sum > maxSum) maxSum = sum;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    shared.release = true;
    {
        ProtoContext::UnmanagedScope parked(root);
        for (const ProtoThread* h : handles) const_cast<ProtoThread*>(h)->join(root);
    }
    gBatch = nullptr;
    return maxSum;
}

}  // namespace

TEST(HeapLimitBatch, ThreadBatchesTogetherStayWithinTheLimitFraction) {
    constexpr int kThreads = 8;
    constexpr unsigned long kLimit = 500000;
    ProtoSpace space;
    space.setHeapLimits(/*soft=*/0, /*hard=*/static_cast<int>(kLimit));
    const uint64_t cyclesStart = space.getGCCycleCount();

    BatchShared shared;
    shared.rounds = 200;
    shared.garbagePerRound = 2000;   // 4,000 cells per round per thread
    const unsigned long maxSum = runThreads(space, kThreads, shared);

    EXPECT_EQ(shared.done.load(), kThreads);
    EXPECT_EQ(shared.errors.load(), 0u);
    EXPECT_LE(maxSum, kLimit / 4)
        << "the threads' refill batches together held too much of the heap limit "
           "(largest single thread: " << shared.maxHeld.load() << " cells)";
    EXPECT_GE(space.getGCCycleCount() - cyclesStart, 2u) << "the limit did not make the collector run";
}

TEST(HeapLimitBatch, WithoutALimitRefillsAreUnchanged) {
    constexpr int kThreads = 2;
    ProtoSpace space;
    ASSERT_EQ(space.maxHeapSize, 0);

    BatchShared shared;
    shared.rounds = 20;
    shared.garbagePerRound = 20000;
    (void) runThreads(space, kThreads, shared);

    EXPECT_EQ(shared.done.load(), kThreads);
    EXPECT_EQ(shared.errors.load(), 0u);
    EXPECT_GE(shared.maxHeld.load(), static_cast<unsigned long>(ProtoSpace::CELL_CHUNK_SIZE) - 1)
        << "without a limit a refill must still hand out at least a full chunk";
}
