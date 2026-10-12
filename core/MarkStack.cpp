/*
 * MarkStack.cpp — diagnostics of the collector's mark phase (MarkStack.h).
 */

#include "MarkStack.h"

#include <atomic>
#include <mutex>

namespace proto {
namespace gcmark {

    namespace {
        // Heap-allocated and never destroyed: a collector thread may record
        // its last cycle while static destructors run at process exit.
        std::mutex& statsMutex() {
            static std::mutex* m = new std::mutex();
            return *m;
        }
        MarkStats& lastStats() {
            static MarkStats* s = new MarkStats();
            return *s;
        }
        std::atomic<std::size_t> capacityOverride{0};
    }  // namespace

    void recordMarkStats(const MarkStats& stats) {
        std::lock_guard<std::mutex> lock(statsMutex());
        const std::uint64_t cycles = lastStats().cycles + 1;
        lastStats() = stats;
        lastStats().cycles = cycles;
    }

    MarkStats lastMarkStats() {
        std::lock_guard<std::mutex> lock(statsMutex());
        return lastStats();
    }

    std::size_t markStackCapacity() {
        const std::size_t o = capacityOverride.load(std::memory_order_relaxed);
        return o ? o : kDefaultMarkStackEntries;
    }

    void setMarkStackCapacityForTest(std::size_t entries) {
        capacityOverride.store(entries, std::memory_order_relaxed);
    }

}  // namespace gcmark
}  // namespace proto
