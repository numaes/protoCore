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
    for (int i = 0; i < 50; ++i) {
        // Create 10000 objects in a batch
        for (int j = 0; j < 10000; ++j) {
            ctx->newObject(false);
        }
        // Small delay to let GC work if it's concurrent
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    std::cout << "Final Heap Size: " << space.heapSize << " blocks" << std::endl;
    std::cout << "Final Free Cells: " << space.freeCellsCount << " blocks" << std::endl;

    // If GC works, the heap size should not grow proportionally to the millions of objects
    // because many should be collected (they are not referenced).
    // Total objects created: 50 * 10000 = 500,000.
    // Each object is a Cell. 500,000 cells > initial heap.
    // If heap size is way less than 500,000, GC reclaimed most of them.
    ASSERT_LT(space.heapSize, 100000); // 100k blocks is way less than 500k objects
}
