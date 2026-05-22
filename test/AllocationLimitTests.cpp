// AllocationLimitTests.cpp — heap allocation ceiling + reliable OOM detection.
//
// Exercises ProtoSpace::setHeapLimits and the limit-aware getFreeCells path:
//   * a hard ceiling caps heapSize; under it, a workload far larger than the
//     ceiling still completes because the GC keeps reclaiming garbage;
//   * a soft watermark does not hard-block allocation;
//   * a genuinely out-of-memory workload (a retained graph exceeding the
//     ceiling) reaches the out-of-memory escalation: the embedder callback is
//     invoked and the process performs the controlled abort.
//
// See docs/superpowers/specs/2026-05-22-allocation-limit-oom-design.md.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"
#include <vector>
#include <cstdio>

using namespace proto;

namespace {

// Allocate `count` non-mutable objects through a short-lived child context.
// The context is destroyed on scope exit, which submits its young generation
// to the GC — so everything allocated here becomes garbage and is reclaimable.
void allocGarbageBatch(ProtoSpace& space, int count) {
    ProtoContext sub(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    for (int j = 0; j < count; ++j) {
        (void) sub.newObject(false);
    }
}

} // namespace

// A hard ceiling must cap heapSize, and a workload that allocates far more
// *garbage* than the ceiling must still complete — every batch is reclaimed by
// the GC, so the reclaim-wait path in getFreeCells keeps serving cells without
// the heap ever growing past the limit.
TEST(AllocationLimitTest, HardLimitCapsHeapAndReclamationSustainsWorkload) {
    ProtoSpace space;
    const int base = space.heapSize;          // cells already allocated at startup
    const int hard = base + 40000;            // a modest ceiling above the baseline
    space.setHeapLimits(/*soft=*/0, /*hard=*/hard);

    // 100 batches x 5000 objects = 500,000 allocations — more than 12x the
    // ceiling. This can only complete if the GC reclaims each destroyed
    // batch's garbage and getFreeCells waits for it rather than growing the
    // heap unboundedly.
    for (int i = 0; i < 100; ++i) {
        allocGarbageBatch(space, 5000);
    }

    EXPECT_LE(space.heapSize, hard)
        << "heapSize grew past the configured hard ceiling";
    // Sanity: the ceiling was actually engaged (the workload needed far more
    // than the heap ever held).
    EXPECT_GT(100 * 5000, hard - base);
}

// A soft watermark biases the allocator toward reclamation but must never
// hard-block: a garbage workload still completes and stays within the hard
// ceiling.
TEST(AllocationLimitTest, SoftLimitDoesNotBlockAllocation) {
    ProtoSpace space;
    const int base = space.heapSize;
    const int soft = base + 15000;
    const int hard = base + 60000;
    space.setHeapLimits(soft, hard);

    for (int i = 0; i < 80; ++i) {
        allocGarbageBatch(space, 5000);
    }

    EXPECT_LE(space.heapSize, hard)
        << "heapSize grew past the hard ceiling with a soft limit set";
}

// setHeapLimits clamps a soft watermark above the hard ceiling, and rejects
// negative values.
TEST(AllocationLimitTest, SetHeapLimitsValidatesArguments) {
    ProtoSpace space;
    space.setHeapLimits(/*soft=*/100000, /*hard=*/10000);
    EXPECT_EQ(space.maxHeapSize, 10000);
    EXPECT_LE(space.softHeapLimit, space.maxHeapSize)
        << "a soft watermark above the hard ceiling must be clamped";

    space.setHeapLimits(-5, -9);
    EXPECT_EQ(space.softHeapLimit, 0);
    EXPECT_EQ(space.maxHeapSize, 0);
}

// --- Out-of-memory escalation -----------------------------------------------

// Records (via stderr, which the death test below matches) that the embedder
// out-of-memory callback was invoked. Frees nothing, so protoCore proceeds to
// the controlled abort.
static ProtoObject* oomMarkerCallback(ProtoContext*) {
    std::fprintf(stderr, "OOM_CALLBACK_FIRED\n");
    std::fflush(stderr);
    return nullptr;
}

// A genuinely out-of-memory workload — a *retained* graph that exceeds the
// hard ceiling — must be detected reliably: the live (reachable) set itself
// meets the ceiling, so no collection can help. protoCore must invoke the
// out-of-memory callback and then perform the controlled abort.
//
// The workload runs inside the death test's forked child: it builds its own
// ProtoSpace (so the GC thread is created post-fork), keeps every allocating
// context alive so the objects stay reachable from the GC roots, and allocates
// until the ceiling is genuinely exhausted by live data.
TEST(AllocationLimitDeathTest, UnrecoverableOomInvokesCallbackThenAborts) {
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    ASSERT_DEATH({
        ProtoSpace space;
        space.outOfMemoryCallback = oomMarkerCallback;
        const int base = space.heapSize;
        space.setHeapLimits(/*soft=*/0, /*hard=*/base + 30000);

        // Keep every context alive: a live context's young generation is a GC
        // root, so the objects allocated here stay reachable and cannot be
        // reclaimed. The live set therefore grows without bound until it
        // reaches the ceiling — genuine, unrecoverable OOM.
        std::vector<ProtoContext*> retained;
        for (;;) {
            ProtoContext* c = new ProtoContext(
                &space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
            retained.push_back(c);
            for (int j = 0; j < 5000; ++j) {
                (void) c->newObject(false);
            }
        }
    }, "OOM_CALLBACK_FIRED");
}
