// HeapHeadroomWaitTests.cpp — the heap-headroom wait parks without submitting
// the waiting context's young generation.
//
// Under a hard heap limit, the outermost critical section's heap checkpoint
// (ProtoContext::heapLimitCheckpoint -> ProtoSpace::waitForHeapHeadroom)
// blocks until a collection cycle refills the freelist.  That wait runs inside
// native code, where the caller may hold a half-built structure only in C++
// locals: its cells are protected only while they stay in the context's young
// chain.  Handing that chain to the collector there (as ProtoContext::safepoint
// does once the context crosses maxAllocatedCellsPerContext) makes those cells
// candidates while nothing references them, so a later cycle frees them.
//
// The test makes the building context itself wait, deterministically:
//   * it builds a list held only in a C++ local, so the context's young chain
//     is far above the (lowered) submission threshold;
//   * the hard limit equals the current heap, so nothing can grow;
//   * each round drains the space-level free chunks with allocations that
//     open no critical section (newList), in a short-lived child context, and
//     stops as soon as the space freelist is empty;
//   * the next append in the building context opens the first outermost
//     critical section after that, so its heap checkpoint waits for a cycle
//     (which reclaims the child's garbage) with the building context.
// Before the fix the wait submitted the building context's young chain, the
// following cycle freed the list, and the next drain reused its cells.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"

#include <cstdint>

using namespace proto;

TEST(HeapHeadroomWait, StructureHeldInCppLocalSurvivesHeadroomWaits) {
    ProtoSpace space;
    ProtoContext* root = space.rootContext;
    space.maxAllocatedCellsPerContext = 256;

    constexpr int kInitialElements = 1000;
    constexpr int kRounds = 8;

    ProtoContext work(&space, root, nullptr, nullptr, nullptr, nullptr);
    // Held only in this C++ local and in `work`'s young chain.
    const ProtoList* list = work.newList();
    for (int i = 0; i < kInitialElements; ++i) {
        list = list->appendLast(&work, work.fromInteger(i));
    }
    ASSERT_GT(work.allocatedCellsCount, static_cast<unsigned long>(space.maxAllocatedCellsPerContext))
        << "the building context must be above the submission threshold";

    const uint64_t cyclesStart = space.getGCCycleCount();
    space.setHeapLimits(/*soft=*/0, /*hard=*/space.heapSize);

    for (int round = 0; round < kRounds; ++round) {
        {
            // Drain the space-level freelist with allocations that open no
            // critical section, so no heap checkpoint runs in this context.
            ProtoContext garbage(&space, &work, nullptr, nullptr, nullptr, nullptr);
            for (int batch = 0; batch < 100000 && (space.freeChunks || space.freeCells); ++batch) {
                for (int k = 0; k < 256; ++k) (void) garbage.newList();
            }
        }
        // First outermost critical section with the heap at its ceiling and
        // no free chunks: its checkpoint waits for headroom in `work`.  The
        // object allocated here does not reference the list, as tuple nodes
        // built from a list do not reference the list's own nodes: after the
        // wait nothing young in `work` keeps the list reachable.
        (void) work.newObject(false);
    }
    const uint64_t cycles = space.getGCCycleCount() - cyclesStart;
    space.setHeapLimits(0, 0);

    const unsigned long expected = kInitialElements;
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->getSize(&work), expected);
    int mismatches = 0;
    for (unsigned long i = 0; i < expected; ++i) {
        const ProtoObject* element = list->getAt(&work, static_cast<int>(i));
        if (!element || !element->isInteger(&work) ||
            element->asLong(&work) != static_cast<long long>(i)) {
            ++mismatches;
        }
    }
    EXPECT_EQ(mismatches, 0) << "the list held in a C++ local was freed during a headroom wait";
    EXPECT_GE(cycles, static_cast<uint64_t>(kRounds) / 2)
        << "too few cycles ran for the headroom wait to be exercised";
}
