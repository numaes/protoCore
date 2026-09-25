// ThreadExitReleaseTests.cpp - what an exiting managed thread gives back.
//
// THE DEFECT
// ----------
// A ProtoThread allocates from a private batch (ProtoThreadExtension::freeCells)
// that ProtoSpace::getFreeCells refills in bulk.  Until this test existed,
// nothing ever gave that batch back: on exit the unused tail belonged to no
// freelist and to no young generation, so no cycle could reclaim it and
// `freeCellsCount` simply fell.  Measured in a bare ProtoSpace with no runtime
// at all: 4,096 cells per empty-bodied thread and 8,192 per allocating one,
// with `heapSize` and `liveCellsLastCycle` both flat - the signature of memory
// that is neither live nor free.  The thread's root ProtoContext (and with it
// its whole un-submitted young generation, its automaticLocals array), the two
// per-thread caches (32 KiB + 24 KiB of malloc, invisible to `heapSize`) and
// the std::thread object went the same way.
//
// WHY THE FIX IS NOT IN `finalize`
// -------------------------------
// The release happens on the exiting thread itself (core/Thread.cpp,
// releaseExitingThread) and, for the std::thread object, in ProtoThread::join.
// It cannot happen in ProtoThreadImplementation::finalize: that runs on the GC
// thread inside the sweep, where docs/GarbageCollector.md section 7 forbids
// blocking, allocating and publishing - and the release blocks on
// ProtoSpace::globalMutex, submits a young generation and (for the
// std::thread) would have to join.  It would also never run at all: the thread
// cells live in the young chain of the scratch context ProtoSpace::newThread
// never destroys, so they are never sweep candidates.
//
// The aggregate assertions below are per-thread and against a denominator, so
// they say "a batch is not lost", not merely "some cells came back"; the
// white-box ones name the individual resources, so a partial fix cannot pass.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"
#include "CycleDriver.h"

#include <atomic>
#include <cstdio>
#include <vector>

using namespace proto;

namespace {

std::atomic<long> g_ran{0};

const ProtoObject* emptyBody(ProtoContext*, const ProtoObject*, const ParentLink*,
                             const ProtoList*, const ProtoSparseList*) {
    g_ran.fetch_add(1, std::memory_order_relaxed);
    return PROTO_NONE;
}

const ProtoObject* allocatingBody(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                                  const ProtoList*, const ProtoSparseList*) {
    long long n = 0;
    for (int i = 0; i < 400; ++i) n += (long long) ctx->newList()->getSize(ctx);
    (void) n;
    ctx->safepoint();
    g_ran.fetch_add(1, std::memory_order_relaxed);
    return PROTO_NONE;
}

// Cells the space believes it is holding: everything it took from the OS that
// is not on a freelist.  `heapSize - freeCellsCount`, the measure
// conformance/CycleDriver.h prescribes over any per-context counter.
long inUse(ProtoSpace& space) { return conformance::sample(space).inUse; }

struct ChurnResult {
    long base{0};
    long after{0};
    long heapBefore{0};
    long heapAfter{0};
    long threads{0};
    double perThread{0.0};
};

ChurnResult churn(ProtoSpace& space, ProtoContext* root, ProtoMethod body,
                  int rounds, int perRound, const char* label) {
    const ProtoString* name = ProtoString::createSymbol(root, "exit-release-worker");

    // Settle first: a space that has never collected has no meaningful
    // baseline, and the first cycles reclaim bootstrap garbage that would
    // otherwise be charged to the threads.
    conformance::driveCycles(space, root, /*maxCycles=*/6, /*deadlineMs=*/20000);

    ChurnResult r;
    r.base = inUse(space);
    r.heapBefore = space.heapSize;

    for (int round = 0; round < rounds; ++round) {
        {
            ProtoContext scope(&space, root, nullptr, nullptr, nullptr, nullptr);
            std::vector<const ProtoThread*> threads;
            for (int k = 0; k < perRound; ++k) {
                const ProtoThread* t =
                    space.newThread(&scope, name, body, scope.newList(), nullptr);
                if (t) threads.push_back(t);
            }
            for (const ProtoThread* t : threads)
                const_cast<ProtoThread*>(t)->join(&scope);
            r.threads += (long) threads.size();
        }
        root->returnValue = nullptr;
        conformance::driveCycles(space, root, /*maxCycles=*/6, /*deadlineMs=*/20000);
    }

    r.after = inUse(space);
    r.heapAfter = space.heapSize;
    r.perThread = r.threads ? (double) (r.after - r.base) / (double) r.threads : 0.0;

    std::printf("[ THREADS  ] %s: %ld threads, inUse %ld -> %ld (%+.1f cells per "
                "exiting thread), heapSize %ld -> %ld\n",
                label, r.threads, r.base, r.after, r.perThread,
                r.heapBefore, r.heapAfter);
    std::fflush(stdout);
    return r;
}

// The smallest batch protoCore ever hands a thread is kMinLimitedBatchCells
// (512) and the measured losses were 4,096 and 8,192.  Anything at or above
// this bound is a lost batch; anything below is the handful of cells the thread
// cells themselves cost (they live in a scratch context that is still never
// destroyed - see the note at the end of this file).
constexpr double kLostBatchThreshold = 256.0;

}  // namespace

// An empty-bodied thread still takes a batch: thread_main itself allocates, for
// the safepoint and for the threads-list rebuild on the way out.  4,096 cells
// per thread went missing before the fix.
TEST(ThreadExitRelease, AnEmptyThreadLosesNoAllocationBatch) {
    g_ran = 0;
    ProtoSpace space;
    ProtoContext root(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);

    const ChurnResult r = churn(space, &root, &emptyBody, /*rounds=*/8, /*perRound=*/2,
                                "empty body");

    ASSERT_EQ(g_ran.load(), r.threads) << "the thread bodies did not run";
    ASSERT_GE(r.threads, 16) << "too few threads to measure a per-thread loss";

    EXPECT_LT(r.perThread, kLostBatchThreshold)
        << "each exiting thread left " << r.perThread << " cells neither live nor "
           "free, over " << r.threads << " threads.  An allocation batch is at "
           "least 512 cells and the measured loss was 4,096 per empty thread, so "
           "this is a lost batch, not overhead";
    EXPECT_EQ(r.heapAfter, r.heapBefore)
        << "the space had to grow the heap to absorb the thread churn";
}

// An allocating thread lost twice as much: 8,192 cells per thread.
TEST(ThreadExitRelease, AnAllocatingThreadLosesNoAllocationBatch) {
    g_ran = 0;
    ProtoSpace space;
    ProtoContext root(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);

    const ChurnResult r = churn(space, &root, &allocatingBody, /*rounds=*/8, /*perRound=*/2,
                                "allocating body");

    ASSERT_EQ(g_ran.load(), r.threads) << "the thread bodies did not run";
    ASSERT_GE(r.threads, 16) << "too few threads to measure a per-thread loss";

    EXPECT_LT(r.perThread, kLostBatchThreshold)
        << "each exiting thread left " << r.perThread << " cells neither live nor "
           "free, over " << r.threads << " threads";
    EXPECT_EQ(r.heapAfter, r.heapBefore)
        << "the space had to grow the heap to absorb the thread churn";
}

// Naming the individual resources, so that returning the cells while still
// leaking the rest cannot pass.  Every field below is released by a different
// step: the context by releaseExitingThread step 1, the caches by step 2, the
// batch by step 3, and the std::thread object by ProtoThread::join - which is
// the only place that can, because a completed join is protoCore's only proof
// that the OS thread has stopped.
TEST(ThreadExitRelease, AJoinedThreadHoldsNoneOfItsResources) {
    g_ran = 0;
    ProtoSpace space;
    ProtoContext root(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    const ProtoString* name = ProtoString::createSymbol(&root, "exit-release-probe");

    const ProtoThread* t =
        space.newThread(&root, name, &allocatingBody, root.newList(), nullptr);
    ASSERT_NE(t, nullptr);

    const auto* impl = toImpl<const ProtoThreadImplementation>(t);
    ProtoThreadExtension* ext = impl->extension;
    ASSERT_NE(ext, nullptr);
    EXPECT_NE(ext->osThread, nullptr) << "the thread was never spawned";

    const_cast<ProtoThread*>(t)->join(&root);
    ASSERT_EQ(g_ran.load(), 1);

    EXPECT_EQ(impl->context, nullptr)
        << "the thread's root ProtoContext was not destroyed, so its young "
           "generation was never submitted and its automaticLocals array leaked";
    EXPECT_EQ(ext->freeCells, nullptr)
        << "the thread's allocation batch was not returned to the space";
    EXPECT_EQ(ext->attributeCache, nullptr)
        << "the 32 KiB per-thread attribute cache was not freed";
    EXPECT_EQ(ext->mutableValueCache, nullptr)
        << "the 24 KiB per-thread mutable-value cache was not freed";
    EXPECT_EQ(ext->osThread, nullptr)
        << "the std::thread object was not released by the completed join";

    // Joining twice must be harmless now that the first join freed the object.
    const_cast<ProtoThread*>(t)->join(&root);

    std::printf("[ THREADS  ] a joined thread holds: context=%p batch=%p "
                "attrCache=%p mutCache=%p osThread=%p\n",
                (void*) impl->context, (void*) ext->freeCells,
                (void*) ext->attributeCache, (void*) ext->mutableValueCache,
                (void*) ext->osThread);
    std::fflush(stdout);
}

// What is NOT fixed, stated so that a future reader does not mistake the bound
// above for zero: ProtoSpace::newThread builds its two thread cells on a
// scratch ProtoContext it never destroys, so those cells (and that context's
// automaticLocals array) still leak - a handful of cells per thread rather than
// a batch.  Removing that leak means changing which context the thread cells
// live in, which is a different change from this one.
