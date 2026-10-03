/*
 * Sweep.cpp -- Phase 5, the sweep, and the collector's helper threads.  See
 * Sweep.h and docs/specs/2026-10-03-collector-throughput-design.md.
 */

#include "Sweep.h"
#include "AdaptiveHeap.h"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#else
#include <pthread.h>
#endif

namespace proto {
namespace sweep {

    namespace {
        thread_local bool tlHelper = false;
        thread_local std::vector<proto_ulong>* tlRefSink = nullptr;
    }

    bool isHelperThread() { return tlHelper; }
    std::vector<proto_ulong>* finalizedRefSink() { return tlRefSink; }

    // --- One sweeper ----------------------------------------------------------

    void SegmentChain::push(DirtySegment* seg) {
        seg->next = head;
        if (!tail) tail = seg;
        head = seg;
        ++count;
    }

    void SegmentChain::pushAllOnto(std::atomic<DirtySegment*>& list) {
        if (!head) return;
        DirtySegment* expected = list.load(std::memory_order_relaxed);
        do {
            tail->next = expected;
        } while (!list.compare_exchange_weak(expected, head,
                                             std::memory_order_release,
                                             std::memory_order_relaxed));
        head = tail = nullptr;
        count = 0;
    }

    // Merge one segment's dead cells into the running chunk, and publish the
    // chunk once it reaches CELL_CHUNK_SIZE.
    void SweeperLocal::addBatch(Cell* head, Cell* tail, proto_ulong count) {
        if (!head) return;
        // Prepend: the batch's tail points at the previous chunk head.  Order
        // does not matter for the freelist; publishFreeChunk sets the final
        // terminator.
        if (chunkHead) tail->internalSetNextRaw(chunkHead);
        else chunkTail = tail;
        chunkHead = head;
        chunkCount += count;
        reclaimed += count;
        if (chunkCount >= ProtoSpace::CELL_CHUNK_SIZE) {
            std::lock_guard<std::recursive_mutex> chunkLock(ProtoSpace::globalMutex);
            publishFreeChunk(space, chunkHead, chunkTail, chunkCount);
            chunkHead = chunkTail = nullptr;
            chunkCount = 0;
        }
    }

    // A segment is done: survivors re-chained in it go to the pen, an empty
    // one back to the free pool -- both through local chains published in
    // batches (SegmentChain).  The free pool's head is the line every mutator
    // pops at each context destruction, so one compare-and-swap per segment
    // of about 6 cells contended with all of them.
    void SweeperLocal::retire(DirtySegment* seg, Cell* survivors) {
#ifdef PROTOCORE_GC_REINCLUDE_SURVIVORS
        if (survivors) {
            // With stagger == 1 (default) the pen is folded back into
            // dirtySegments at the start of every cycle; with stagger > 1
            // only every Nth cycle (survivorStagger).
            seg->cellChain = survivors;
            penSegs.push(seg);
            return;
        }
#else
        (void) survivors;
#endif
        seg->cellChain = nullptr;
        freeSegs.push(seg);
        // The free chain goes back every kSegmentRecycleBatch segments, so
        // the mutators are not starved of segments during a long sweep.
        if (freeSegs.count >= kSegmentRecycleBatch)
            freeSegs.pushAllOnto(space->dirtySegmentFreePool);
    }

    // Publish everything still private: the segment chains (the survivor
    // chain once per sweeper: the pen is folded only under a later
    // stop-the-world) and the trailing partial chunk.
    void SweeperLocal::finish() {
        freeSegs.pushAllOnto(space->dirtySegmentFreePool);
        penSegs.pushAllOnto(space->survivorPen);
        if (chunkHead) {
            std::lock_guard<std::recursive_mutex> chunkLock(ProtoSpace::globalMutex);
            publishFreeChunk(space, chunkHead, chunkTail, chunkCount);
            chunkHead = chunkTail = nullptr;
            chunkCount = 0;
        }
    }

    // Each cell: one load of its header word; a dead cell is finalized and
    // chained into its segment's batch (or recorded for a deferred free); a
    // survivor is unmarked and prepended to its segment's survivor chain with
    // one store.
    //
    // Multi-cursor: a chain is a dependent pointer chase with one miss in
    // flight.  kSweepCursors chains walked in lockstep, each prefetching its
    // next cell (write intent) before the others are processed, keep several
    // misses in flight on one thread.  Each cursor keeps its own batch and
    // survivor chain, merged when its segment ends, so the result is the one
    // the single-chain walk produced.
    void sweepSegments(DirtySegment* list, SweeperLocal& L) {
        struct Cursor {
            DirtySegment* seg;
            Cell* cell;
            Cell* batchHead;
            Cell* batchTail;
            proto_ulong batchCount;
            Cell* survHead;
        };
        Cursor cur[kSweepCursors];
        int active = 0;
        DirtySegment* nextSeg = list;
        auto load = [&](Cursor& c) -> bool {
            if (!nextSeg) return false;
            c.seg = nextSeg;
            nextSeg = nextSeg->next;
            if (nextSeg) PROTO_PREFETCH(nextSeg);
            c.cell = c.seg->cellChain;
            if (c.cell) PROTO_PREFETCH(c.cell);
            c.batchHead = c.batchTail = nullptr;
            c.batchCount = 0;
            c.survHead = nullptr;
            ++L.segments;
            return true;
        };
        while (active < kSweepCursors && load(cur[active])) ++active;
        ProtoContext* const ctx = L.space->rootContext;
        while (active > 0) {
            for (int i = 0; i < active;) {
                Cursor& c = cur[i];
                Cell* cell = c.cell;
                if (!cell) {
                    L.addBatch(c.batchHead, c.batchTail, c.batchCount);
                    L.retire(c.seg, c.survHead);
                    if (!load(c)) cur[i] = cur[--active];
                    continue;
                }
                const uintptr_t header = cell->next_and_flags.load(std::memory_order_acquire);
                Cell* next = reinterpret_cast<Cell*>(header & ~PROTO_UL(0x3F));
                if (next) PROTO_PREFETCH(next);
                ++L.swept;
                if (!(header & PROTO_UL(0x1))) {
                    if (L.deferFree) {
                        L.deadCells->push_back(cell);
                    } else if (L.helper && cell->getType() == CellType::ExternalPointer) {
                        // An embedder finalizer: the collector thread runs it
                        // after the join, serially (spec 6.5, decision 5).
                        // The cell is not freed before then.
                        L.deferredExternal.push_back(cell);
                    } else {
                        cell->finalize(ctx);
                        cell->internalSetNextRaw(c.batchHead);
                        if (!c.batchTail) c.batchTail = cell;
                        c.batchHead = cell;
                        ++c.batchCount;
                    }
                } else {
#ifdef PROTOCORE_GC_REINCLUDE_SURVIVORS
                    // Unmark and prepend to the survivor chain in one store:
                    // the collector's threads are the only writers of a
                    // candidate's next_and_flags during sweep (Cell::setNext),
                    // and each candidate belongs to exactly one sweeper, so an
                    // unmark (a locked read-modify-write) followed by a second
                    // store is not needed.  Flag bits 1..5 are kept (always
                    // zero).  The old link is in `next` already.
                    cell->next_and_flags.store(
                        (reinterpret_cast<uintptr_t>(c.survHead) & ~PROTO_UL(0x3F))
                            | (header & PROTO_UL(0x3E)),
                        std::memory_order_release);
                    c.survHead = cell;
#else
                    cell->unmark();
#endif
                }
                c.cell = next;
                ++i;
            }
        }
    }

    // --- Partition ----------------------------------------------------------------

    DirtySegment* claimRun(SegmentCursor& cursor, unsigned run, bool* moreLeft) {
        if (run == 0) run = 1;
        std::lock_guard<std::mutex> lock(cursor.lock);
        DirtySegment* head = cursor.next;
        if (!head) {
            if (moreLeft) *moreLeft = false;
            return nullptr;
        }
        // The list's links are read only under this lock, and no segment is
        // ever pushed back onto it (a processed segment goes to the free pool
        // or the pen), so a claim cannot suffer ABA.
        DirtySegment* tail = head;
        for (unsigned k = 1; k < run && tail->next; ++k) tail = tail->next;
        cursor.next = tail->next;
        tail->next = nullptr;
        if (moreLeft) *moreLeft = cursor.next != nullptr;
        return head;
    }

    // --- The pool -----------------------------------------------------------------

    namespace {
        // A cycle's sweep offered to the helpers.  Fields below `cursor` are
        // guarded by the pool mutex.
        struct Job {
            ProtoSpace* space = nullptr;
            bool deferFree = false;
            SegmentCursor* cursor = nullptr;
            bool closed = false;
            unsigned active = 0;
            unsigned joined = 0;
            proto_ulong reclaimed = 0;
            proto_ulong swept = 0;
            proto_ulong segments = 0;
            std::vector<proto_ulong> refs;
            std::vector<Cell*> dead;
            std::vector<Cell*> external;
        };

        std::atomic<bool> gEngageAlways{false};
        // setHelperCount: -1 = not set (environment, then default).
        std::atomic<int> gConfiguredCount{-1};

        struct Pool {
            std::mutex m;
            std::condition_variable wake;
            std::condition_variable done;
            // Never destroyed by a static destructor (spec 6.10): a process
            // that exits with a live space leaves parked helpers to the OS,
            // as it does the collector thread.  A forked child replaces the
            // whole pool (the threads did not survive the fork).
            std::vector<std::thread>* threads = new std::vector<std::thread>();
            bool stopping = false;
            std::uint64_t generation = 0;
            Job* job = nullptr;
            PoolStats stats;
        };

        Pool* gPool = nullptr;
        std::once_flag gPoolOnce;

        void atforkPrepare() { if (gPool) gPool->m.lock(); }
        void atforkParent() { if (gPool) gPool->m.unlock(); }
        // The child has none of the helper threads: a fresh pool, the old
        // one (its mutex held by this thread, its std::thread objects naming
        // threads that do not exist) leaked on purpose.
        void atforkChild() {
            if (!gPool) return;
            PoolStats keep = gPool->stats;
            keep.threads = 0;
            gPool = new Pool();
            gPool->stats = keep;
        }

        Pool& pool() {
            std::call_once(gPoolOnce, [] {
                gPool = new Pool();
#if !defined(_WIN32)
                pthread_atfork(atforkPrepare, atforkParent, atforkChild);
#endif
            });
            return *gPool;
        }

        void helperWork(Job* j) {
            SweeperLocal L(j->space, j->deferFree, nullptr, /*helper=*/true);
            std::vector<Cell*> dead;
            L.deadCells = &dead;
            tlRefSink = &L.finalizedRefs;
            while (DirtySegment* run = claimRun(*j->cursor, kClaimRun)) sweepSegments(run, L);
            tlRefSink = nullptr;
            L.finish();
            Pool& p = *gPool;
            std::lock_guard<std::mutex> lock(p.m);
            j->reclaimed += L.reclaimed;
            j->swept += L.swept;
            j->segments += L.segments;
            j->refs.insert(j->refs.end(), L.finalizedRefs.begin(), L.finalizedRefs.end());
            j->dead.insert(j->dead.end(), dead.begin(), dead.end());
            j->external.insert(j->external.end(), L.deferredExternal.begin(), L.deferredExternal.end());
        }

        // `seen`: the generation when the thread was created, so a thread
        // created for a job joins it.
        void helperMain(Pool* p, unsigned index, std::uint64_t seen) {
            tlHelper = true;
#if defined(__linux__)
            pthread_setname_np(pthread_self(), "protocore-sweep");
#endif
            std::unique_lock<std::mutex> lock(p->m);
            for (;;) {
                p->wake.wait(lock, [&] { return p->stopping || p->generation != seen; });
                if (p->stopping) break;
                seen = p->generation;
                Job* j = p->job;
                if (!j || j->closed || index >= helperCount()) continue;
                ++j->active;
                ++j->joined;
                ++p->stats.helperRuns;
                lock.unlock();
                helperWork(j);
                lock.lock();
                if (--j->active == 0) p->done.notify_all();
            }
        }

        // Offer `j` to the helpers (the collector thread).  False when the
        // pool is busy with another space's job, stopping, or has no helper:
        // the collector then sweeps alone and never waits for the pool.
        bool tryStart(Job* j) {
            const unsigned want = helperCount();
            if (want == 0) return false;
            Pool& p = pool();
            std::lock_guard<std::mutex> lock(p.m);
            if (p.stopping) return false;
            if (p.job) {
                ++p.stats.refusedBusy;
                return false;
            }
            if (p.threads->size() < want) {
                if (p.threads->empty()) ++p.stats.starts;
                while (p.threads->size() < want) {
                    const unsigned index = static_cast<unsigned>(p.threads->size());
                    p.threads->emplace_back(helperMain, &p, index, p.generation);
                }
                p.stats.threads = static_cast<unsigned>(p.threads->size());
            }
            p.job = j;
            j->closed = false;
            ++p.generation;
            ++p.stats.jobs;
            p.stats.concurrentJobsMax = std::max<std::uint64_t>(p.stats.concurrentJobsMax, 1);
            p.wake.notify_all();
            return true;
        }

        // Close `j` to newcomers and wait for the helpers that joined it.
        void finishJob(Job* j) {
            Pool& p = *gPool;
            std::unique_lock<std::mutex> lock(p.m);
            j->closed = true;
            p.done.wait(lock, [&] { return j->active == 0; });
            p.job = nullptr;
        }

        // PROTOCORE_GC_SWEEP_ENGAGE=always: a diagnostic and test switch
        // that offers every cycle's sweep to the helpers.
        bool engageAlwaysFromEnvironment() {
            static const bool always = [] {
                const char* v = std::getenv("PROTOCORE_GC_SWEEP_ENGAGE");
                return v && std::strcmp(v, "always") == 0;
            }();
            return always;
        }

        bool helpersWanted() {
            return gEngageAlways.load(std::memory_order_relaxed)
                || engageAlwaysFromEnvironment()
                || adaptive::headroomWaitersTotal.load(std::memory_order_relaxed) > 0;
        }
    }  // namespace

    CycleSweep sweepCycle(ProtoSpace* space, DirtySegment* list, bool deferFree,
                          std::vector<Cell*>& deadCells) {
        CycleSweep result;
        SegmentCursor cursor;
        cursor.next = list;
        Job job;
        job.space = space;
        job.deferFree = deferFree;
        job.cursor = &cursor;
        bool engaged = false;

        SweeperLocal L(space, deferFree, &deadCells);
        bool moreLeft = false;
        while (DirtySegment* run = claimRun(cursor, kClaimRun, &moreLeft)) {
            sweepSegments(run, L);
            // The collector starts alone: a cycle that one claim exhausts
            // never touches the pool.  The helpers are offered the rest once
            // mutators wait for headroom, which is when their CPU is free.
            if (!engaged && moreLeft && helpersWanted()) engaged = tryStart(&job);
        }
        if (engaged) finishJob(&job);
        L.finish();

        result.reclaimed = L.reclaimed + job.reclaimed;
        result.swept = L.swept + job.swept;
        result.segments = L.segments + job.segments;
        result.helpersJoined = job.joined;
        if (!job.refs.empty())
            space->gcFinalizedMutableRefs.insert(space->gcFinalizedMutableRefs.end(),
                                                 job.refs.begin(), job.refs.end());
        if (!job.dead.empty()) deadCells.insert(deadCells.end(), job.dead.begin(), job.dead.end());
        // The embedder finalizers the helpers left, one at a time on this
        // thread, then their cells as one chunk.
        if (!job.external.empty()) {
            Cell* head = nullptr;
            Cell* tail = nullptr;
            for (Cell* cell : job.external) {
                cell->finalize(space->rootContext);
                cell->internalSetNextRaw(head);
                if (!tail) tail = cell;
                head = cell;
            }
            std::lock_guard<std::recursive_mutex> chunkLock(ProtoSpace::globalMutex);
            publishFreeChunk(space, head, tail, job.external.size());
            result.reclaimed += job.external.size();
        }
        return result;
    }

    // --- Configuration and lifecycle ------------------------------------------------

    unsigned physicalCoreCount() {
#if defined(_WIN32)
        DWORD len = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
        if (len == 0) return 1;
        std::vector<char> buf(len);
        if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()), &len))
            return 1;
        unsigned cores = 0;
        for (DWORD off = 0; off < len;) {
            auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + off);
            if (info->Relationship == RelationProcessorCore) ++cores;
            off += info->Size;
        }
        return cores ? cores : 1;
#elif defined(__APPLE__)
        int cores = 0;
        size_t size = sizeof(cores);
        if (sysctlbyname("hw.physicalcpu", &cores, &size, nullptr, 0) == 0 && cores > 0)
            return static_cast<unsigned>(cores);
        return std::max(1u, std::thread::hardware_concurrency());
#else
        // One entry per core: the set of logical CPUs that share it.
        const unsigned logical = std::max(1u, std::thread::hardware_concurrency());
        std::set<std::string> cores;
        for (unsigned cpu = 0; cpu < logical; ++cpu) {
            std::ifstream in("/sys/devices/system/cpu/cpu" + std::to_string(cpu)
                             + "/topology/thread_siblings_list");
            std::string siblings;
            if (in && std::getline(in, siblings)) cores.insert(siblings);
        }
        if (!cores.empty()) return static_cast<unsigned>(cores.size());
        return logical;
#endif
    }

    unsigned defaultHelperCount() {
        static const unsigned count = physicalCoreCount() / 2;
        return count;
    }

    bool parseHelperCount(const char* text, unsigned& out) {
        if (!text || !*text) return false;
        unsigned value = 0;
        for (const char* p = text; *p; ++p) {
            if (*p < '0' || *p > '9') return false;
            value = value * 10 + static_cast<unsigned>(*p - '0');
            if (value > 64) return false;
        }
        out = value;
        return true;
    }

    unsigned helperCount() {
        const int configured = gConfiguredCount.load(std::memory_order_relaxed);
        if (configured >= 0) return static_cast<unsigned>(configured);
        unsigned fromEnv = 0;
        if (parseHelperCount(std::getenv("PROTOCORE_GC_SWEEP_THREADS"), fromEnv)) return fromEnv;
        return defaultHelperCount();
    }

    void setHelperCount(unsigned count) {
        gConfiguredCount.store(static_cast<int>(std::min(count, 64u)), std::memory_order_relaxed);
    }

    void shutdownPoolIfNoSpaces() {
        if (!gPool) return;
        if (multispace::liveSpaceCount() != 0) return;
        Pool& p = *gPool;
        std::vector<std::thread>* threads = nullptr;
        {
            std::lock_guard<std::mutex> lock(p.m);
            if (p.threads->empty() || p.stopping) return;
            p.stopping = true;
            threads = p.threads;
            p.threads = new std::vector<std::thread>();
            p.stats.threads = 0;
            p.wake.notify_all();
        }
        // A helper in a job finishes its claimed work first; it waits for
        // nothing but globalMutex, held briefly, so the join cannot block.
        for (std::thread& t : *threads) t.join();
        delete threads;
        std::lock_guard<std::mutex> lock(p.m);
        p.stopping = false;
    }

    void runAsHelperForTest(void (*fn)(void*), void* arg) {
        const bool was = tlHelper;
        tlHelper = true;
        fn(arg);
        tlHelper = was;
    }

    void setEngageAlways(bool always) { gEngageAlways.store(always, std::memory_order_relaxed); }

    PoolStats poolStats() {
        if (!gPool) return PoolStats();
        std::lock_guard<std::mutex> lock(gPool->m);
        return gPool->stats;
    }

}  // namespace sweep
}  // namespace proto
