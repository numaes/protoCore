/*
 * ProtoRootSet.cpp
 *
 * Embedder-owned GC root sets.  See ProtoRootSet in protoCore.h for
 * motivation and the public API.
 *
 * Implementation outline:
 *
 *   - A flat vector of slots, each holding either a live `(handle,
 *     ProtoObject*)` pair or a "free" marker.  The vector grows
 *     monotonically; slot indices are stable for the life of the
 *     root set.
 *
 *   - A `std::mutex` protects mutation (add/remove) and iteration.
 *     Adds and removes are short and contention is bounded by the
 *     embedder's call rate; on the steady state they are amortised
 *     O(1) thanks to the free-list of recyclable slots.  Iteration
 *     happens only from the GC thread during STW, when mutators are
 *     parked, so the lock is uncontended in that path too.
 *
 *   - Handles are 64-bit and encode `(slot_index, generation)` so a
 *     stale handle from a freed slot cannot accidentally resolve to
 *     a different object after the slot is reused.  Generation
 *     counters wrap at 2^32; reuse after wrap-around is benign at
 *     normal call rates.
 *
 * The implementation is intentionally simple: no atomics, no
 * lock-free pop.  In benchmarks the JS thread does at most a few
 * thousand pin/unpin per second, far below the threshold where mutex
 * contention would matter, and it is much easier to reason about
 * than a hand-rolled lock-free free list.
 */

#include "../headers/protoCore.h"
#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

namespace proto {

struct ProtoRootSet::Impl {
    std::string name;
    ProtoSpace* owner;

    // Slot layout: low 32 bits = slot index, high 32 bits = generation.
    static constexpr unsigned long long kFreeMarker = 0;  // generation 0 == free

    struct Slot {
        const ProtoObject* obj = nullptr;
        unsigned int generation = 0;  // 0 means free; >=1 means live
    };

    std::vector<Slot> slots;
    std::vector<unsigned int> freeList;  // indices of free slots
    unsigned int nextGeneration = 1;
    unsigned long liveCount = 0;
    mutable std::mutex mutex;
};

ProtoRootSet::ProtoRootSet(ProtoSpace* owner, const char* name)
    : impl_(new Impl{}) {
    impl_->owner = owner;
    impl_->name = name ? name : "";
    // Reserve a small initial capacity to avoid reallocs in the common
    // case (a few hundred outstanding async callbacks).
    impl_->slots.reserve(64);
}

ProtoRootSet::~ProtoRootSet() {
    delete impl_;
}

ProtoRootSet::Handle ProtoRootSet::add(const ProtoObject* obj) {
    if (!obj) return kNullHandle;
    std::lock_guard<std::mutex> lock(impl_->mutex);

    unsigned int idx;
    if (!impl_->freeList.empty()) {
        idx = impl_->freeList.back();
        impl_->freeList.pop_back();
    } else {
        idx = static_cast<unsigned int>(impl_->slots.size());
        impl_->slots.emplace_back();
    }
    auto& slot = impl_->slots[idx];
    unsigned int gen = impl_->nextGeneration++;
    if (gen == 0) gen = impl_->nextGeneration++;  // skip the "free" marker
    slot.obj = obj;
    slot.generation = gen;
    impl_->liveCount++;
    return (static_cast<Handle>(gen) << 32) | static_cast<Handle>(idx);
}

const ProtoObject* ProtoRootSet::resolve(Handle h) const {
    if (h == kNullHandle) return nullptr;
    unsigned int idx = static_cast<unsigned int>(h & 0xFFFFFFFFu);
    unsigned int gen = static_cast<unsigned int>(h >> 32);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (idx >= impl_->slots.size()) return nullptr;
    const auto& slot = impl_->slots[idx];
    if (slot.generation != gen) return nullptr;
    return slot.obj;
}

void ProtoRootSet::remove(Handle h) {
    if (h == kNullHandle) return;
    unsigned int idx = static_cast<unsigned int>(h & 0xFFFFFFFFu);
    unsigned int gen = static_cast<unsigned int>(h >> 32);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (idx >= impl_->slots.size()) return;
    auto& slot = impl_->slots[idx];
    if (slot.generation != gen) return;  // stale handle — silent no-op
    slot.obj = nullptr;
    slot.generation = 0;  // mark free
    impl_->freeList.push_back(idx);
    impl_->liveCount--;
}

unsigned long ProtoRootSet::size() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->liveCount;
}

const char* ProtoRootSet::getName() const {
    return impl_->name.c_str();
}

void ProtoRootSet::forEachRoot(
    void (*visit)(void* user, const ProtoObject* obj),
    void* user) const {
    if (!visit) return;
    // The GC normally calls this during STW where the lock is
    // uncontended; locking unconditionally also makes the method
    // safe to invoke from diagnostic code at any time.
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const auto& slot : impl_->slots) {
        if (slot.generation != 0 && slot.obj) {
            visit(user, slot.obj);
        }
    }
}

// ---- ProtoSpace integration -------------------------------------------

ProtoRootSet* ProtoSpace::createRootSet(const char* name) {
    auto* rs = new ProtoRootSet(this, name);
    std::lock_guard<std::mutex> lock(rootSetsMutex_);
    rootSets_.push_back(rs);
    return rs;
}

void ProtoSpace::destroyRootSet(ProtoRootSet* rs) {
    if (!rs) return;
    {
        std::lock_guard<std::mutex> lock(rootSetsMutex_);
        auto it = std::find(rootSets_.begin(), rootSets_.end(), rs);
        if (it != rootSets_.end()) rootSets_.erase(it);
    }
    delete rs;
}

void ProtoSpace::forEachRootSet(
    void (*visit)(void* user, ProtoRootSet* rs),
    void* user) const {
    if (!visit) return;
    // The GC thread calls this during STW; mutators are parked so no
    // createRootSet/destroyRootSet can run concurrently.  We still
    // take the lock for correctness against pre-STW callers.
    std::lock_guard<std::mutex> lock(rootSetsMutex_);
    for (auto* rs : rootSets_) {
        visit(user, rs);
    }
}

} // namespace proto
