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
std::atomic<int> mutableReleasesPending{0};
// Never destroyed by a static destructor: a collector may wait on it while
// the process exits, and destroying a condition variable with a waiter is
// undefined behaviour (glibc's pthread_cond_destroy may block on it).  A
// function, not a namespace-scope object: a space constructed by another
// translation unit's static initializer starts its collector before this
// file's initializers may have run.
std::condition_variable_any& cycleCV() {
    static std::condition_variable_any* cv = new std::condition_variable_any();
    return *cv;
}

namespace {
std::atomic<int> cyclesNow{0};
std::atomic<int> cyclesMax{0};
}  // namespace

proto_ulong countMutableEntriesOfSpace(ProtoContext* context, proto_ulong spaceId) {
    (void) context;
    struct Count { proto_ulong id; proto_ulong n; } count{spaceId, 0};
    for (int s = 0; s < ProtoSpace::MUTABLE_ROOT_SHARDS; ++s) {
        ProtoSparseList* root = globalMutableShards[s].root.load(std::memory_order_acquire);
        if (!root) continue;
        // A shard root is reachable from the table only until the next CAS
        // replaces it: the walk must not let a collection run, and it does
        // not -- it allocates nothing and never parks.
        walkSparseListNoPark(root, &count,
            [](void* self, proto_ulong key, const ProtoObject*) {
                auto* c = static_cast<Count*>(self);
                if ((key >> kMutableRefSpaceShift) == c->id) ++c->n;
            });
    }
    return count.n;
}

namespace {
// Guarded by globalMutex.  Never destroyed by a static destructor, like the
// registry below: a collector still running while the process exits takes it
// in Phase 5b.
std::vector<proto_ulong>& destroyedIds() {
    static std::vector<proto_ulong>* ids = new std::vector<proto_ulong>();
    return *ids;
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

void recordDestroyedSpace(proto_ulong spaceId) {
    std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
    destroyedIds().push_back(spaceId);
}

void appendRefsOfDestroyedSpaces(std::vector<proto_ulong>& refs, const proto_ulong* keepId) {
    std::vector<proto_ulong> ids;
    {
        std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
        ids.swap(destroyedIds());
        if (keepId) {
            const auto kept = std::remove(ids.begin(), ids.end(), *keepId);
            if (kept != ids.end()) {
                ids.erase(kept, ids.end());
                destroyedIds().push_back(*keepId);
            }
        }
    }
    if (ids.empty()) return;
    std::sort(ids.begin(), ids.end());
    for (int s = 0; s < ProtoSpace::MUTABLE_ROOT_SHARDS; ++s) {
        const ProtoSparseList* root = globalMutableShards[s].root.load(std::memory_order_acquire);
        if (!root) continue;
        forEachKey(root, [&](proto_ulong key) {
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
    proto_ulong id;
};

// Guarded by ProtoSpace::globalMutex.  Never destroyed by a static
// destructor: a process may exit while a space's collector (or a thread of
// it) still runs, and both reach this registry.
std::vector<Entry>& registry() {
    static std::vector<Entry>* entries = new std::vector<Entry>();
    return *entries;
}
proto_ulong nextSpaceId = 0;

// The id of the one registered space while the process has exactly one, and
// kNoSoleSpace otherwise.  Written under globalMutex by registerSpace and
// unregisterSpace; read without it by goOut / comeBack (see "Leaving and
// rejoining the quorum" below).  Ids are never reused, so an equal id names
// the same registration, not merely a space at the same address.
constexpr proto_ulong kNoSoleSpace = ~static_cast<proto_ulong>(0);
std::atomic<proto_ulong> soleSpaceId{kNoSoleSpace};

void updateSoleSpaceLocked() {
    const auto& r = registry();
    soleSpaceId.store(r.size() == 1 ? r.front().id : kNoSoleSpace, std::memory_order_release);
}

// The space that started the calling OS thread, if any, and that thread's
// ProtoThreadImplementation in it.
thread_local ProtoSpace* tlWorkerSpace = nullptr;
thread_local ProtoThreadImplementation* tlWorkerImpl = nullptr;

// Unmanaged nesting of the calling OS thread, across all its spaces, and the
// spaces it announced itself out of at the outermost entry (by id, so a space
// destroyed in the meantime is recognised and skipped).
thread_local int tlOutDepth = 0;
thread_local std::vector<Entry> tlOutOf;

bool registered(const ProtoSpace* space, proto_ulong id) {
    for (const Entry& e : registry())
        if (e.space == space && e.id == id) return true;
    return false;
}

// The spaces the calling OS thread constructed, as their implicit main
// thread.  A space counts its main thread in runningThreads from construction
// (ProtoSpace's constructor), and nothing uncounted it when that thread
// exited while the space lived on -- a runtime that builds a space on a
// helper thread and destroys it from another one.  Every later
// stop-the-world then waited for a thread that no longer existed, and so did
// ~ProtoSpace's terminal stop.  At thread exit each space still registered
// uncounts it (and drops it from its parked count if it exited while out).
struct MainThreadHolder {
    std::vector<Entry> spaces;
    ~MainThreadHolder();
};
thread_local MainThreadHolder tlMainOf;

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
#ifdef PROTOCORE_GC_INSTRUMENT
    const auto parkStart = std::chrono::steady_clock::now();
#endif
    space->impl->parkWaiters.fetch_add(1, std::memory_order_seq_cst);
    space->parkedThreads++;
    {
        std::unique_lock<std::recursive_mutex> lock(ProtoSpace::globalMutex);
        space->gcCV.notify_all();
        waitOutStop(space, lock);   // never returns from a terminal stop
        space->impl->parkWaiters.fetch_sub(1, std::memory_order_seq_cst);
    }
    space->parkedThreads--;
#ifdef PROTOCORE_GC_INSTRUMENT
    gcprof::mutatorParkNs.fetch_add(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - parkStart).count()),
        std::memory_order_relaxed);
    gcprof::mutatorParks.fetch_add(1, std::memory_order_relaxed);
#endif
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

proto_ulong registerSpace(ProtoSpace* space) {
    std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
    const proto_ulong id = nextSpaceId++;
    // The constructing thread is the space's adopted main thread.
    ensureQuiescenceRecord();
    registry().push_back(Entry{space, id});
    // tlOutOf must outlive tlMainOf, whose destructor reads it: thread-locals
    // are destroyed in the reverse order of their construction.
    (void)tlOutOf.size();
    tlMainOf.spaces.push_back(Entry{space, id});
    updateSoleSpaceLocked();
    return id;
}

void unregisterSpace(ProtoSpace* space) {
    std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
    auto& r = registry();
    for (auto it = r.begin(); it != r.end(); ++it) {
        if (it->space == space) { r.erase(it); break; }
    }
    updateSoleSpaceLocked();
}

proto_ulong liveSpaceCount() {
    std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
    return registry().size();
}

void setWorkerSpace(ProtoSpace* space, ProtoThreadImplementation* impl) {
    tlWorkerSpace = space;
    tlWorkerImpl = impl;
    if (space) ensureQuiescenceRecord();
}

bool isWorkerOf(const ProtoSpace* space) {
    return space == tlWorkerSpace;
}

bool isMember(const ProtoSpace* space) {
    if (space == tlWorkerSpace) return true;
    if (space->mainThreadId != std::this_thread::get_id()) return false;
    // The main thread's id, unless that thread has exited and the OS has
    // given its id to the calling thread.
    return !(space->impl && space->impl->mainThreadExited.load(std::memory_order_acquire));
}

MainThreadHolder::~MainThreadHolder() {
    if (spaces.empty()) return;
    std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
    for (const Entry& e : spaces) {
        if (!registered(e.space, e.id) || !e.space->impl) continue;  // destroyed already
        e.space->impl->mainThreadExited.store(true, std::memory_order_release);
        if (isOutOf(e.space)) e.space->parkedThreads.fetch_sub(1, std::memory_order_seq_cst);
        e.space->runningThreads.fetch_sub(1, std::memory_order_seq_cst);
        e.space->gcCV.notify_all();
    }
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

// ---- Leaving and rejoining the quorum ----------------------------------------
//
// A collector waits for its quorum (parkedThreads >= runningThreads) on gcCV
// in exactly one place: Phase 1 of gcThreadLoop, after raising stwFlag.  It
// raises the flag, evaluates the predicate and blocks, all under globalMutex
// (the wait releases the mutex atomically).  A thread leaving the quorum must
// wake it only then: an idle collector waits on the same condition variable
// for gcStarted, which parkedThreads does not affect, and waking it for every
// unmanaged region cost a futex wake, a mutex hand-off and a context switch
// on the collector per region (protoClojure's grep: one per input line).
//
// So goOut increments parkedThreads, THEN reads stwFlag, and notifies only
// when the flag is up; both operations are seq_cst, as are the collector's
// store of the flag and its predicate's load of the counter.  In the single
// total order of seq_cst operations either
//
//   * the thread's load of stwFlag precedes the collector's store: then its
//     increment precedes it too, so the predicate, evaluated after the store,
//     sees the increment and the collector does not block for this thread; or
//   * the load follows the store and returns true: the thread takes
//     globalMutex and notifies.  The collector holds the mutex from the store
//     until it blocks, so the notify cannot fall between its predicate and
//     its wait: it is either still to evaluate the predicate (and sees the
//     increment), already waiting (and is woken), or past the wait.
//
// No wakeup is lost.  comeBack is the mirror image: it decrements
// parkedThreads, THEN reads stwFlag (both seq_cst) and parks when the flag
// is up.  If the load precedes the collector's store, so does the decrement,
// and the predicate sees this thread as running and waits for it to park at
// a safepoint; otherwise the thread parks before touching any cell.  Either
// way no thread runs managed code while a stop-the-world it was counted out
// of is in progress.  A decrement needs no notify: it never satisfies the
// predicate.
//
// The registry walk needs globalMutex.  With one registered space (every
// runtime's normal case, docs/GLOBAL_MUTABLE_TABLE.md "One ProtoSpace per
// process") the walk can only find `own`, so both functions skip the mutex
// when soleSpaceId names `own`:
//
//   * a thread is a member of a space only if it constructed it or was
//     started by it, both done by the thread itself, so no space registered
//     concurrently by another thread can count the calling thread;
//   * comeBack takes the fast path only when the one space it left is still
//     the sole registered one, by id; any other case takes the mutex and
//     checks every space it left, as before.  A space destroyed while one of
//     its threads is out is no longer registered, so that thread takes the
//     mutex path, finds it gone and blocks for good (terminalPark): the
//     terminal stop of ~ProtoSpace.
void goOut(ProtoSpace* own) {
    if (tlOutDepth++ != 0) return;  // only the 0 -> 1 transition
    setOut(+1);
    tlOutOf.clear();
    if (own && soleSpaceId.load(std::memory_order_acquire) == spaceIdOf(own)) {
        if (isGcThreadOf(own)) return;
        own->parkedThreads.fetch_add(1, std::memory_order_seq_cst);
        tlOutOf.push_back(Entry{own, spaceIdOf(own)});
        if (own->stwFlag.load(std::memory_order_seq_cst)) {
            std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
            own->gcCV.notify_all();
        }
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
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
        s->parkedThreads.fetch_add(1, std::memory_order_seq_cst);
        tlOutOf.push_back(e);
        // Holding globalMutex: the collector of `s` is either before its
        // quorum predicate or blocked in the wait, never in between.
        if (s->stwFlag.load(std::memory_order_seq_cst)) s->gcCV.notify_all();
    }
    // A space being destroyed is no longer in the registry, but its
    // destructor's terminal stop still counts the threads out of it: the
    // calling thread runs in it, so it is alive until that stop's quorum.
    if (own && !isGcThreadOf(own) && !registered(own, spaceIdOf(own))
        && own->state == SPACE_STATE_ENDING) {
        own->parkedThreads.fetch_add(1, std::memory_order_seq_cst);
        tlOutOf.push_back(Entry{own, spaceIdOf(own)});
        if (own->stwFlag.load(std::memory_order_seq_cst)) own->gcCV.notify_all();
    }
}

void comeBack(ProtoSpace* own, ProtoContext* context) {
    if (tlOutDepth-- != 1) return;  // only the 1 -> 0 transition (an orphan return is a no-op)
    // Spaces rejoined: at most one per registered space.  One in the
    // single-space case, which then allocates nothing.
    ProtoSpace* single = nullptr;
    std::vector<ProtoSpace*> rejoined;
    if (tlOutOf.size() == 1
        && soleSpaceId.load(std::memory_order_acquire) == tlOutOf.front().id) {
        single = tlOutOf.front().space;
        single->parkedThreads.fetch_sub(1, std::memory_order_seq_cst);
        tlOutOf.clear();
    } else if (!tlOutOf.empty()) {
        bool homeGone = false;
        {
            std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
            for (const Entry& e : tlOutOf) {
                if (!registered(e.space, e.id)) {
                    // Destroyed (or being destroyed) meanwhile.  If it is the
                    // space this thread runs in, the thread cannot go on: its
                    // context, caches and cells belong to that space.  The
                    // pointer is compared, never read.
                    if (e.space == own || e.space == tlWorkerSpace) homeGone = true;
                    continue;
                }
                e.space->parkedThreads.fetch_sub(1, std::memory_order_seq_cst);
                rejoined.push_back(e.space);
            }
            tlOutOf.clear();
        }
        if (homeGone) {
            setOut(-1);   // terminalPark marks the thread out again, for good
            terminalPark();
        }
    }
    // Back in managed code: if a member space is stopped, wait it out as a
    // safepoint would.  Cycles are serialized, so one pass finds at most one.
    setOut(-1);
    if (single && single->stwFlag.load(std::memory_order_seq_cst)) parkIn(single);
    for (ProtoSpace* s : rejoined) {
        if (!s->stwFlag.load(std::memory_order_seq_cst)) continue;
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

void waitOutStop(ProtoSpace* space, std::unique_lock<std::recursive_mutex>& lock) {
    const ProtoSpaceImplementation* impl = space->impl;
    space->stopTheWorldCV.wait(lock, [space, impl] {
        return !space->stwFlag.load() || impl->terminal.load(std::memory_order_seq_cst);
    });
    if (!impl->terminal.load(std::memory_order_seq_cst)) return;
    // The space is being destroyed and will not resume.  Leave its counters
    // and wake the destructor (it waits for parkWaiters == 0 before it frees
    // anything), then never touch the space again.
    impl->parkWaiters.fetch_sub(1, std::memory_order_seq_cst);
    space->gcCV.notify_all();
    lock.unlock();
    terminalPark();
}

bool isOutOf(const ProtoSpace* space) {
    if (tlOutDepth == 0) return false;
    for (const Entry& e : tlOutOf)
        if (e.space == space) return true;
    return false;
}

bool homeSpaceGoneOnReturn() {
    if (tlOutDepth != 1 || tlOutOf.empty()) return false;
    // The single-space fast path of comeBack: the one registered space is the
    // one this thread left, so it is alive.
    if (tlOutOf.size() == 1
        && soleSpaceId.load(std::memory_order_acquire) == tlOutOf.front().id)
        return false;
    std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
    for (const Entry& e : tlOutOf)
        if (e.space == tlWorkerSpace && !registered(e.space, e.id)) return true;
    return false;
}

void terminalPark() {
    {
        std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
        // Out of the quorum of every live space this thread is still a member
        // of, permanently: their collectors must not wait for it.
        for (const Entry& e : registry()) {
            ProtoSpace* s = e.space;
            if (!isMember(s) || isGcThreadOf(s) || isOutOf(s)) continue;
            s->parkedThreads.fetch_add(1, std::memory_order_seq_cst);
            if (s->stwFlag.load(std::memory_order_seq_cst)) s->gcCV.notify_all();
        }
    }
    // Out of every grace period, for good (the record stays in use).
    setOut(+1);
    // Process-global and never destroyed: the thread waits here past the
    // space, and past static destruction at exit.
    static std::mutex* gateMutex = new std::mutex();
    static std::condition_variable* gate = new std::condition_variable();
    std::unique_lock<std::mutex> lock(*gateMutex);
    for (;;) gate->wait(lock);
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

proto_ulong ProtoSpace::liveSpaceCount() { return multispace::liveSpaceCount(); }

}  // namespace proto
