#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"
#include <vector>

using namespace proto;

TEST(GCStressTest, LargeAllocationReclamation) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    unsigned long initialHeap = space.heapSize;
    unsigned long initialFree = space.freeCellsCount;

    std::cout << "Initial Heap Size: " << initialHeap << " blocks" << std::endl;

    // Perform massive allocations that should trigger GC multiple times
    // Allocation threshold is 20% free. 
    // Initial allocation in ProtoSpace triggers ~10240 blocks.
    // We allocate millions of objects.
    for (int i = 0; i < 200; ++i) {
        // Create a temporary context for each batch
        {
             ProtoContext subCtx(&space, ctx, nullptr, nullptr, nullptr, nullptr);
             for (int j = 0; j < 5000; ++j) {
                 subCtx.newObject(false);
             }
        }
        // Give GC time to process the newly submitted segments
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Wait for GC cycles to catch up with promotions
    for(int i=0; i<5; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "Final Heap Size: " << space.heapSize << " blocks" << std::endl;
    std::cout << "Final Free Cells: " << space.freeCellsCount << " blocks" << std::endl;

    // We created 1,000,000 objects (200 * 5000).
    // Given the conservative nature of context-local pinning, the heap will be larger
    // than a fully aggressive GC, but it should still show significant reclamation
    // (i.e., it should not be near 1 million blocks).
    ASSERT_LT(space.heapSize, 800000); 
}
