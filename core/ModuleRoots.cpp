/*
 * ModuleRoots.cpp — the process-global module list, and a GC root.
 *
 * P3 (2026-09-24). See ModuleRootTable in headers/proto_internal.h for the
 * design and for why this is a root rather than merely unfreed memory.
 */

#include "../headers/proto_internal.h"

#include <algorithm>

namespace proto {

namespace {
    // Diagnostics for the pause-cost tests (P3 D6). Process-wide and relaxed:
    // read only by tests, never by the collector's logic.
    std::atomic<unsigned long> g_lastCaptureShardReads{0};
    std::atomic<unsigned long> g_stwVisitViolations{0};

    // Shard by the module pointer, so concurrent loads of different modules
    // rarely contend. The low 6 bits are dropped: cells are 64-byte aligned.
    inline int shardFor(const ProtoObject* module) {
        const auto bits = reinterpret_cast<uintptr_t>(module);
        return static_cast<int>((bits >> 6) % ModuleRootTable::SHARD_COUNT);
    }
}

ModuleRootTable::~ModuleRootTable() {
    for (Shard& shard : shards) {
        Chunk* chunk = shard.first.load(std::memory_order_relaxed);
        while (chunk) {
            Chunk* next = chunk->next.load(std::memory_order_relaxed);
            delete chunk;
            chunk = next;
        }
    }
}

void ModuleRootTable::add(const ProtoObject* module, const ProtoSpace* owner) {
    if (!module || module == PROTO_NONE || !owner) return;
    Shard& shard = shards[shardFor(module)];
    std::lock_guard<std::mutex> lock(shard.mutex);

    // Already published for this owner? The cache-hit path of
    // getImportModuleImpl re-roots a module on every import of the same
    // identity, so this de-duplication is load-bearing, not defensive.
    size_t remaining = shard.published.load(std::memory_order_relaxed);
    for (Chunk* c = shard.first.load(std::memory_order_relaxed); c && remaining;
         c = c->next.load(std::memory_order_relaxed)) {
        const size_t n = std::min(remaining, CHUNK_SIZE);
        for (size_t i = 0; i < n; ++i)
            if (c->entries[i].module == module && c->entries[i].owner == owner) return;
        remaining -= n;
    }

    const size_t count = shard.published.load(std::memory_order_relaxed);
    const size_t index = count % CHUNK_SIZE;
    if (index == 0) {
        Chunk* chunk = new Chunk();
        if (shard.last) shard.last->next.store(chunk, std::memory_order_release);
        else            shard.first.store(chunk, std::memory_order_release);
        shard.last = chunk;
    }
    Entry& entry = shard.last->entries[index];
    entry.module = module;
    entry.owner  = owner;
    // Publish last: the GC walks only published entries, so both words above
    // must be visible before the count is.
    shard.published.store(count + 1, std::memory_order_release);
}

// GC Phase 2, under stop-the-world. O(SHARD_COUNT) counter reads; no entry is
// dereferenced. This is the ONLY work this table does inside the pause.
void ModuleRootTable::captureForGC() {
    unsigned long reads = 0;
    for (Shard& shard : shards) {
        shard.gcCaptured = shard.published.load(std::memory_order_acquire);
        ++reads;
    }
    g_lastCaptureShardReads.store(reads, std::memory_order_relaxed);
}

// GC Phase 4, after the world has resumed. Visits every entry the last
// captureForGC() covered whose owner is `space`.
void ModuleRootTable::forEachCaptured(
        const ProtoSpace* space, void* user,
        void (*visit)(void* user, const ProtoObject* module)) const {
    // The walk belongs to the concurrent mark and must NEVER run inside the
    // pause (P3 D6). Observing stwFlag here is what the pause-cost test reads.
    if (space && space->stwFlag.load(std::memory_order_relaxed))
        g_stwVisitViolations.fetch_add(1, std::memory_order_relaxed);

    for (const Shard& shard : shards) {
        size_t remaining = shard.gcCaptured;
        for (const Chunk* c = shard.first.load(std::memory_order_acquire); c && remaining;
             c = c->next.load(std::memory_order_acquire)) {
            const size_t n = std::min(remaining, CHUNK_SIZE);
            for (size_t i = 0; i < n; ++i)
                if (c->entries[i].owner == space) visit(user, c->entries[i].module);
            remaining -= n;
        }
    }
}

size_t ModuleRootTable::size() const {
    size_t total = 0;
    for (const Shard& shard : shards)
        total += shard.published.load(std::memory_order_acquire);
    return total;
}

unsigned long ModuleRootTable::lastCaptureShardReads() {
    return g_lastCaptureShardReads.load(std::memory_order_relaxed);
}
unsigned long ModuleRootTable::stwVisitViolations() {
    return g_stwVisitViolations.load(std::memory_order_relaxed);
}
void ModuleRootTable::resetDiagnostics() {
    g_lastCaptureShardReads.store(0, std::memory_order_relaxed);
    g_stwVisitViolations.store(0, std::memory_order_relaxed);
}

// The one table of this process. Function-local static, leaked on purpose for
// the same reason globalSymbolTable() is: its entries are perennial and it must
// outlive every ProtoSpace and every other static.
ModuleRootTable& globalModuleRootTable() {
    static ModuleRootTable* table = new ModuleRootTable();
    return *table;
}

} // namespace proto
