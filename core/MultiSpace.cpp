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

#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace proto {
namespace multispace {

std::atomic<int> stopRequests{0};
bool cycleActive = false;
std::condition_variable_any cycleCV;

namespace {
std::atomic<int> cyclesNow{0};
std::atomic<int> cyclesMax{0};
}  // namespace

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

void clearCachesOf(ProtoContext* context, ProtoSpace* stopped) {
    if (context && context->thread) {
        if (auto* ext = toImpl<ProtoThreadImplementation>(context->thread)->extension)
            ext->clearCachesAfterStopTheWorld(context->space);
    }
    if (ProtoThreadImplementation* impl = implIn(stopped)) {
        if (impl->extension) impl->extension->clearCachesAfterStopTheWorld(stopped);
    }
}

// Waits out `space`'s stop-the-world, counted in its quorum.
void parkIn(ProtoSpace* space) {
    space->parkedThreads++;
    {
        std::unique_lock<std::recursive_mutex> lock(ProtoSpace::globalMutex);
        space->gcCV.notify_all();
        space->stopTheWorldCV.wait(lock, [space] { return !space->stwFlag.load(); });
    }
    space->parkedThreads--;
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
        clearCachesOf(context, own);
        return;
    }
    // Another space this thread belongs to.
    if (stopRequests.load(std::memory_order_relaxed) == 0) return;
    if (ProtoSpace* other = foreignStopToAnswer(own)) {
        parkIn(other);
        clearCachesOf(context, other);
    }
}

void goOut(ProtoSpace* own) {
    if (tlOutDepth++ != 0) return;  // only the 0 -> 1 transition
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
    for (ProtoSpace* s : rejoined) {
        if (!s->stwFlag.load(std::memory_order_acquire)) continue;
        parkIn(s);
    }
    for (ProtoSpace* s : rejoined) clearCachesOf(context, s);
    if (own && !context) {
        // No context to reach the caches through: clear this thread's
        // caches in `own` directly.
        if (ProtoThreadImplementation* impl = implIn(own))
            if (impl->extension) impl->extension->clearCachesAfterStopTheWorld(own);
    }
}

}  // namespace multispace
}  // namespace proto
