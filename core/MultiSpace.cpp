/*
 * MultiSpace.cpp -- several ProtoSpaces in one process.
 *
 * A space counts in its stop-the-world quorum every thread it started
 * (ProtoSpace::newThread) and the thread that constructed it, its adopted
 * main thread.  One OS thread that constructs two spaces is therefore a member
 * of both.  The rules here make such a thread behave as ONE thread across its
 * spaces:
 *
 *   * it answers the stop-the-world of any member space at whatever safepoint
 *     it reaches, whichever space's code it is running;
 *   * when it blocks (unmanaged region, heap-headroom wait) it leaves the
 *     quorum of every member space, not only the one it blocked through.
 *
 * Before these rules a collection of space B waited for the shared thread
 * while that thread ran space A's code, for as long as it did.
 *
 * With one space in the process every function here reduces to the previous
 * single-space code path.  See docs/GLOBAL_MUTABLE_TABLE.md.
 */

#include "../headers/proto_internal.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace proto {

alignas(64) ProtoSpace::MutableShardSlot globalMutableShards[ProtoSpace::MUTABLE_ROOT_SHARDS];

namespace multispace {

std::atomic<int> attention{0};
std::atomic<uint64_t> gcEpoch{0};
bool cycleActive = false;
std::condition_variable_any cycleCV;

namespace {
std::atomic<int> cyclesNow{0};
std::atomic<int> cyclesMax{0};
}  // namespace

unsigned long countMutableEntriesOfSpace(ProtoContext* context, unsigned long spaceId) {
    struct Count { unsigned long id; unsigned long n; } count{spaceId, 0};
    for (int s = 0; s < ProtoSpace::MUTABLE_ROOT_SHARDS; ++s) {
        ProtoSparseList* root = globalMutableShards[s].root.load(std::memory_order_acquire);
        if (!root) continue;
        root->processElements(context, &count,
            [](ProtoContext*, void* self, unsigned long key, const ProtoObject*) {
                auto* c = static_cast<Count*>(self);
                if ((key >> kMutableRefSpaceShift) == c->id) ++c->n;
            });
    }
    return count.n;
}

namespace {
std::vector<unsigned long>& destroyedIds() {   // guarded by globalMutex
    static std::vector<unsigned long> ids;
    return ids;
}

// Every key of a shard tree, without allocating.
template <typename F>
void forEachKey(const ProtoSparseList* root, F&& f) {
    ProtoObjectPointer p{};
    p.oid = reinterpret_cast<const ProtoObject*>(root);
    if (p.op.pointer_tag == POINTER_TAG_SPARSE_LIST_SMALL) {
        const auto* small = toImpl<const ProtoSparseListSmallImplementation>(root);
        for (unsigned i = 0; i < ProtoSparseListSmallImplementation::MAX_INLINE; ++i)
            if (small->keys[i] != 0 && small->values[i]) f(small->keys[i]);
        return;
    }
    std::vector<const ProtoSparseListImplementation*> stack;
    stack.push_back(toImpl<const ProtoSparseListImplementation>(root));
    while (!stack.empty()) {
        const ProtoSparseListImplementation* n = stack.back();
        stack.pop_back();
        if (!n) continue;
        if (!n->isEmpty && n->value) f(n->key);
        if (n->previous) stack.push_back(n->previous);
        if (n->next) stack.push_back(n->next);
    }
}
}  // namespace

void recordDestroyedSpace(unsigned long spaceId) {
    std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
    destroyedIds().push_back(spaceId);
}

void appendRefsOfDestroyedSpaces(std::vector<unsigned long>& refs) {
    std::vector<unsigned long> ids;
    {
        std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
        ids.swap(destroyedIds());
    }
    if (ids.empty()) return;
    std::sort(ids.begin(), ids.end());
    for (int s = 0; s < ProtoSpace::MUTABLE_ROOT_SHARDS; ++s) {
        const ProtoSparseList* root = globalMutableShards[s].root.load(std::memory_order_acquire);
        if (!root) continue;
        forEachKey(root, [&](unsigned long key) {
            if (std::binary_search(ids.begin(), ids.end(), key >> kMutableRefSpaceShift))
                refs.push_back(key);
        });
    }
}

void noteCycleStart() {
    const int now = cyclesNow.fetch_add(1) + 1;
    int seen = cyclesMax.load();
    while (now > seen && !cyclesMax.compare_exchange_weak(seen, now)) {}
}
void noteCycleEnd() { cyclesNow.fetch_sub(1); }
int cyclesHighWater() { return cyclesMax.load(); }
void resetCyclesHighWater() { cyclesMax.store(cyclesNow.load()); }

namespace {

struct Entry {
    ProtoSpace* space;
    unsigned long id;
};

// Guarded by ProtoSpace::globalMutex.
std::vector<Entry>& registry() {
    static std::vector<Entry> entries;
    return entries;
}
unsigned long nextSpaceId = 0;

// The space that started the calling OS thread, if any, and that thread's
// ProtoThreadImplementation in it.
thread_local ProtoSpace* tlWorkerSpace = nullptr;
thread_local ProtoThreadImplementation* tlWorkerImpl = nullptr;

// Unmanaged nesting of the calling OS thread, across all its spaces, and the
// spaces it announced itself out of at the outermost entry (by id, so a space
// destroyed in the meantime is recognised and skipped).
thread_local int tlOutDepth = 0;
thread_local std::vector<Entry> tlOutOf;

bool registered(const ProtoSpace* space, unsigned long id) {
    for (const Entry& e : registry())
        if (e.space == space && e.id == id) return true;
    return false;
}

// The calling thread's ProtoThreadImplementation in `space`, or nullptr.
ProtoThreadImplementation* implIn(const ProtoSpace* space) {
    if (space == tlWorkerSpace) return tlWorkerImpl;
    if (space->mainThreadId == std::this_thread::get_id() && space->rootContext &&
        space->rootContext->thread)
        return toImpl<ProtoThreadImplementation>(space->rootContext->thread);
    return nullptr;
}

// Critical-section depth of the calling thread's current context in `space`.
// A thread holding cells of `space` only in C++ locals must not be counted as
// parked or out by that space's collector.
unsigned int criticalDepthIn(const ProtoSpace* space) {
    ProtoThreadImplementation* impl = implIn(space);
    if (!impl || !impl->context) return 0;
    return impl->context->criticalSectionDepth;
}

bool isGcThreadOf(const ProtoSpace* space) {
    return space->gcThread && std::this_thread::get_id() == space->gcThread->get_id();
}

// Clears the calling thread's caches in every space it belongs to, if a
// stop-the-world completed anywhere since they were last cleared.  A thread
// shared by several spaces has one ProtoThreadImplementation, and so one pair
// of caches, per space.
void clearMemberCaches(ProtoContext* context) {
    if (context && context->thread) {
        if (auto* ext = toImpl<ProtoThreadImplementation>(context->thread)->extension)
            ext->clearCachesAfterStopTheWorld(context->space);
    }
    std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
    for (const Entry& e : registry()) {
        if (!isMember(e.space)) continue;
        if (ProtoThreadImplementation* impl = implIn(e.space))
            if (impl->extension) impl->extension->clearCachesAfterStopTheWorld(e.space);
    }
}

// ---- Quiescent-state records -------------------------------------------------

struct QuiescenceRecord {
    std::atomic<uint64_t> seq{0};      // advanced at every quiescent point
    std::atomic<int>      out{0};      // > 0 while parked or out of the quorum
    std::atomic<bool>     inUse{false};
    QuiescenceRecord*     next = nullptr;  // immutable once published
};

// Records are never freed: a record of an exited thread is marked unused and
// reused by the next thread that needs one.
std::atomic<QuiescenceRecord*> records{nullptr};

struct RecordHolder {
    QuiescenceRecord* r = nullptr;
    ~RecordHolder() {
        if (r) {
            r->out.store(0, std::memory_order_relaxed);
            r->inUse.store(false, std::memory_order_release);
        }
    }
};
thread_local RecordHolder tlRecord;

void setOut(int delta) {
    if (tlRecord.r) tlRecord.r->out.fetch_add(delta, std::memory_order_seq_cst);
}

// Waits out `space`'s stop-the-world, counted in its quorum.
void parkIn(ProtoSpace* space) {
    setOut(+1);
    space->parkedThreads++;
    {
        std::unique_lock<std::recursive_mutex> lock(ProtoSpace::globalMutex);
        space->gcCV.notify_all();
        space->stopTheWorldCV.wait(lock, [space] { return !space->stwFlag.load(); });
    }
    space->parkedThreads--;
    setOut(-1);
}

// A member space other than `except` with a raised stop-the-world that the
// calling thread may park for, or nullptr.
ProtoSpace* foreignStopToAnswer(const ProtoSpace* except) {
    std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
    for (const Entry& e : registry()) {
        ProtoSpace* s = e.space;
        if (s == except || !s->stwFlag.load()) continue;
        if (!isMember(s) || isGcThreadOf(s)) continue;
        if (criticalDepthIn(s) > 0) continue;
        return s;
    }
    return nullptr;
}

}  // namespace

unsigned long registerSpace(ProtoSpace* space) {
    std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
    const unsigned long id = nextSpaceId++;
    // The constructing thread is the space's adopted main thread.
    ensureQuiescenceRecord();
    registry().push_back(Entry{space, id});
    return id;
}

void unregisterSpace(ProtoSpace* space) {
    std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
    auto& r = registry();
    for (auto it = r.begin(); it != r.end(); ++it) {
        if (it->space == space) { r.erase(it); break; }
    }
}

unsigned long liveSpaceCount() {
    std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
    return registry().size();
}

void setWorkerSpace(ProtoSpace* space, ProtoThreadImplementation* impl) {
    tlWorkerSpace = space;
    tlWorkerImpl = impl;
    if (space) ensureQuiescenceRecord();
}

bool isMember(const ProtoSpace* space) {
    return space == tlWorkerSpace || space->mainThreadId == std::this_thread::get_id();
}

void parkForAnyStop(ProtoContext* context) {
    ProtoSpace* own = context ? context->space : nullptr;
    if (!own) return;
    // The context's own space: exactly the single-space rule.
    if (own->stwFlag.load(std::memory_order_relaxed)) {
        if (isGcThreadOf(own) || context->criticalSectionDepth > 0) return;
        parkIn(own);
        quiesce(context);
        return;
    }
    // Another space this thread belongs to.
    if (attention.load(std::memory_order_relaxed) == 0) return;
    if (ProtoSpace* other = foreignStopToAnswer(own)) parkIn(other);
    // Every caller is at a safepoint outside any critical section: this is a
    // quiescent point, which a collector's grace period may be waiting for.
    quiesce(context);
}

void goOut(ProtoSpace* own) {
    if (tlOutDepth++ != 0) return;  // only the 0 -> 1 transition
    setOut(+1);
    std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
    tlOutOf.clear();
    for (const Entry& e : registry()) {
        ProtoSpace* s = e.space;
        const bool mine = (s == own) || isMember(s);
        if (!mine || isGcThreadOf(s)) continue;
        // The context's own space keeps the historical rule (no critical-
        // section check: an unmanaged region inside one is the embedder's
        // contract violation, docs/EMBEDDER-CONFORMANCE.md rule 12).  Other
        // spaces are left only when this thread holds none of their cells in
        // a critical section.
        if (s != own && criticalDepthIn(s) > 0) continue;
        s->parkedThreads.fetch_add(1, std::memory_order_acq_rel);
        tlOutOf.push_back(e);
        s->gcCV.notify_all();
    }
}

void comeBack(ProtoSpace* own, ProtoContext* context) {
    (void) own;
    if (tlOutDepth-- != 1) return;  // only the 1 -> 0 transition (an orphan return is a no-op)
    std::vector<ProtoSpace*> rejoined;
    {
        std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
        for (const Entry& e : tlOutOf) {
            if (!registered(e.space, e.id)) continue;  // destroyed meanwhile
            e.space->parkedThreads.fetch_sub(1, std::memory_order_acq_rel);
            rejoined.push_back(e.space);
        }
        tlOutOf.clear();
    }
    // Back in managed code: if a member space is stopped, wait it out as a
    // safepoint would.  Cycles are serialized, so one pass finds at most one.
    setOut(-1);
    for (ProtoSpace* s : rejoined) {
        if (!s->stwFlag.load(std::memory_order_acquire)) continue;
        parkIn(s);
    }
    quiesce(context);
    if (own && !context) {
        // No context to reach the caches through: clear this thread's
        // caches in `own` directly.
        if (ProtoThreadImplementation* impl = implIn(own))
            if (impl->extension) impl->extension->clearCachesAfterStopTheWorld(own);
    }
}

void ensureQuiescenceRecord() {
    if (tlRecord.r) return;
    for (QuiescenceRecord* r = records.load(std::memory_order_acquire); r; r = r->next) {
        bool expected = false;
        if (r->inUse.compare_exchange_strong(expected, true)) {
            r->out.store(0);
            tlRecord.r = r;
            return;
        }
    }
    auto* r = new QuiescenceRecord();
    r->inUse.store(true);
    QuiescenceRecord* head = records.load(std::memory_order_relaxed);
    do { r->next = head; } while (!records.compare_exchange_weak(head, r));
    tlRecord.r = r;
}

void setQuiescenceOut(bool out) {
    ensureQuiescenceRecord();
    setOut(out ? +1 : -1);
}

void quiesce(ProtoContext* context) {
    if (tlRecord.r) tlRecord.r->seq.fetch_add(1, std::memory_order_seq_cst);
    // The caches only need a look when a stop-the-world completed somewhere
    // since this thread last looked; the member walk takes the global mutex.
    thread_local uint64_t seenEpoch = ~0ULL;
    const uint64_t epoch = gcEpoch.load(std::memory_order_acquire);
    if (epoch == seenEpoch) return;
    seenEpoch = epoch;
    clearMemberCaches(context);
}

void waitForGracePeriod() {
    // Make every poll of the process take its slow path, which announces a
    // quiescent state.
    attention.fetch_add(1, std::memory_order_seq_cst);
    struct Pending { QuiescenceRecord* r; uint64_t seq; };
    std::vector<Pending> pending;
    for (QuiescenceRecord* r = records.load(std::memory_order_acquire); r; r = r->next) {
        if (r == tlRecord.r) continue;
        if (!r->inUse.load(std::memory_order_seq_cst)) continue;
        if (r->out.load(std::memory_order_seq_cst) > 0) continue;
        pending.push_back({r, r->seq.load(std::memory_order_seq_cst)});
    }
    for (int round = 0; !pending.empty(); ++round) {
        std::vector<Pending> still;
        for (const Pending& p : pending) {
            if (!p.r->inUse.load(std::memory_order_seq_cst)) continue;
            if (p.r->out.load(std::memory_order_seq_cst) > 0) continue;
            if (p.r->seq.load(std::memory_order_seq_cst) != p.seq) continue;
            still.push_back(p);
        }
        pending.swap(still);
        if (pending.empty()) break;
        if (round < 64) std::this_thread::yield();
        else std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    attention.fetch_sub(1, std::memory_order_seq_cst);
}

}  // namespace multispace
}  // namespace proto
