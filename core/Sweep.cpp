/*
 * Sweep.cpp -- Phase 5, the sweep, and the collector's helper threads.  See
 * Sweep.h and docs/specs/2026-10-03-collector-throughput-design.md.
 */

#include "Sweep.h"
#include "AdaptiveHeap.h"

#include <algorithm>
#include <chrono>
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
    // flight.  sweepCursors() chains walked in lockstep, each prefetching its
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
        Cursor cur[kMaxSweepCursors];
        const int cursors = static_cast<int>(sweepCursors());
        const bool prefetch = sweepPrefetch();
        int active = 0;
        DirtySegment* nextSeg = list;
        auto load = [&](Cursor& c) -> bool {
            if (!nextSeg) return false;
            c.seg = nextSeg;
            nextSeg = nextSeg->next;
            if (prefetch && nextSeg) PROTO_PREFETCH(nextSeg);
            c.cell = c.seg->cellChain;
            if (prefetch && c.cell) PROTO_PREFETCH(c.cell);
            c.batchHead = c.batchTail = nullptr;
            c.batchCount = 0;
            c.survHead = nullptr;
            ++L.segments;
            return true;
        };
        while (active < cursors && load(cur[active])) ++active;
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
                if (prefetch && next) PROTO_PREFETCH(next);
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

        // The API's settings; -1: not set (environment, then default).
        std::atomic<int> gCursors{-1};
        std::atomic<int> gPrefetch{-1};
        std::atomic<int> gEngagement{-1};
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

        // --- Measured engagement ---------------------------------------
        //
        // Helpers take CPU from the mutators and add memory traffic; on a
        // machine whose memory system is already saturated they can make the
        // sweep slower.  Engagement::Measured keeps them only while they
        // shorten the sweep: the sweep's wall time per cell with helpers is
        // compared with the last sweep without them; when it is not shorter,
        // the next 1, 2, 4 ... 64 sweeps that want helpers run alone (the
        // first of them is the new comparison), then helpers are tried again.
        // Per space, on the collector thread; a mutex guards the map.
        std::mutex gEngageMutex;
        std::vector<std::pair<const ProtoSpace*, EngageState>>& engageStates() {
            static auto* v = new std::vector<std::pair<const ProtoSpace*, EngageState>>();
            return *v;
        }
        EngageState& engageStateOf(const ProtoSpace* space) {
            for (auto& e : engageStates()) if (e.first == space) return e.second;
            engageStates().emplace_back(space, EngageState());
            return engageStates().back().second;
        }

        bool mutatorsWait() {
            return adaptive::headroomWaitersTotal.load(std::memory_order_relaxed) > 0;
        }
    }  // namespace

    void noteSweep(EngageState& st, bool engaged, bool heldBack, proto_ulong swept, double nanos) {
        if (swept >= kMinCellsToMeasure) {
            const double perCell = nanos / static_cast<double>(swept);
            if (engaged) {
                if (st.soloNsPerCell > 0.0 && perCell >= st.soloNsPerCell) {
                    // Helpers did not shorten the sweep: hold them back.
                    st.backoff = std::min(kMaxEngageBackoff, std::max(1u, st.backoff * 2));
                    st.skip = st.backoff;
                } else {
                    st.backoff = 0;
                }
            } else {
                st.soloNsPerCell = perCell;
            }
        }
        if (heldBack && st.skip > 0) --st.skip;
    }

    unsigned sweepCursors() {
        const int set = gCursors.load(std::memory_order_relaxed);
        if (set > 0) return static_cast<unsigned>(set);
        static const unsigned fromEnv = [] {
            const char* v = std::getenv("PROTOCORE_GC_SWEEP_CURSORS");
            unsigned k = 0;
            if (v && *v) {
                for (const char* p = v; *p; ++p) {
                    if (*p < '0' || *p > '9') return 0u;
                    k = k * 10 + static_cast<unsigned>(*p - '0');
                    if (k > static_cast<unsigned>(kMaxSweepCursors)) return 0u;
                }
            }
            return k;
        }();
        return fromEnv >= 1 ? fromEnv : kDefaultSweepCursors;
    }

    void setSweepCursors(unsigned count) {
        gCursors.store(count == 0 ? -1 : static_cast<int>(std::min<unsigned>(count, kMaxSweepCursors)),
                       std::memory_order_relaxed);
    }

    bool sweepPrefetch() {
        const int set = gPrefetch.load(std::memory_order_relaxed);
        if (set >= 0) return set != 0;
        static const int fromEnv = [] {
            const char* v = std::getenv("PROTOCORE_GC_SWEEP_PREFETCH");
            if (v && std::strcmp(v, "0") == 0) return 0;
            if (v && std::strcmp(v, "1") == 0) return 1;
            return -1;
        }();
        return fromEnv != 0;
    }

    void setSweepPrefetch(int on) {
        gPrefetch.store(on < 0 ? -1 : (on ? 1 : 0), std::memory_order_relaxed);
    }

    Engagement engagement() {
        const int set = gEngagement.load(std::memory_order_relaxed);
        if (set >= 0) return static_cast<Engagement>(set);
        static const int fromEnv = [] {
            const char* v = std::getenv("PROTOCORE_GC_SWEEP_ENGAGE");
            if (!v) return -1;
            if (std::strcmp(v, "measured") == 0) return 0;
            if (std::strcmp(v, "waiting") == 0) return 1;
            if (std::strcmp(v, "always") == 0) return 2;
            return -1;
        }();
        return fromEnv >= 0 ? static_cast<Engagement>(fromEnv) : Engagement::Measured;
    }

    void setEngagement(int mode) {
        gEngagement.store(mode < 0 || mode > 2 ? -1 : mode, std::memory_order_relaxed);
    }

    void forgetSpace(const ProtoSpace* space) {
        std::lock_guard<std::mutex> lock(gEngageMutex);
        auto& v = engageStates();
        for (std::size_t i = 0; i < v.size(); ++i) {
            if (v[i].first == space) {
                v.erase(v.begin() + static_cast<std::ptrdiff_t>(i));
                return;
            }
        }
    }

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

        const auto start = std::chrono::steady_clock::now();
        const Engagement mode = helperCount() == 0 ? Engagement::WhileWaiting : engagement();
        // Measured engagement: is this cycle held back from the helpers?
        bool heldBack = false;
        if (mode == Engagement::Measured) {
            std::lock_guard<std::mutex> lock(gEngageMutex);
            heldBack = engageStateOf(space).skip > 0;
        }
        bool wanted = false;

        SweeperLocal L(space, deferFree, &deadCells);
        bool moreLeft = false;
        while (DirtySegment* run = claimRun(cursor, kClaimRun, &moreLeft)) {
            sweepSegments(run, L);
            // The collector starts alone: a cycle that one claim exhausts
            // never touches the pool.  The helpers are offered the rest once
            // mutators wait for headroom, which is when their CPU is free.
            if (!engaged && moreLeft && helperCount() > 0
                && (mode == Engagement::Always || mutatorsWait())) {
                wanted = true;
                if (!heldBack) engaged = tryStart(&job);
            }
        }
        if (engaged) finishJob(&job);
        L.finish();

        if (mode == Engagement::Measured) {
            const proto_ulong swept = L.swept + job.swept;
            const double ns = static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start).count());
            std::lock_guard<std::mutex> lock(gEngageMutex);
            EngageState& st = engageStateOf(space);
            noteSweep(st, engaged, heldBack && wanted, swept, ns);
            if (heldBack && wanted) {
                if (gPool) {
                    std::lock_guard<std::mutex> plock(gPool->m);
                    ++gPool->stats.heldBack;
                }
            }
        }

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

    unsigned numaNodeCount() {
#if defined(__linux__)
        unsigned nodes = 0;
        for (unsigned n = 0; n < 1024; ++n) {
            std::ifstream in("/sys/devices/system/node/node" + std::to_string(n) + "/cpulist");
            if (!in) break;
            ++nodes;
        }
        return nodes ? nodes : 1;
#elif defined(_WIN32)
        ULONG highest = 0;
        if (GetNumaHighestNodeNumber(&highest)) return static_cast<unsigned>(highest) + 1;
        return 1;
#else
        return 1;
#endif
    }

    std::uint64_t l3CacheBytes() {
#if defined(__linux__)
        for (unsigned idx = 0; idx < 8; ++idx) {
            const std::string base = "/sys/devices/system/cpu/cpu0/cache/index" + std::to_string(idx);
            std::ifstream lvl(base + "/level");
            int level = 0;
            if (!(lvl >> level)) break;
            if (level != 3) continue;
            std::ifstream sz(base + "/size");
            std::string text;
            if (!(sz >> text) || text.empty()) return 0;
            std::uint64_t value = 0;
            std::size_t i = 0;
            while (i < text.size() && text[i] >= '0' && text[i] <= '9')
                value = value * 10 + static_cast<std::uint64_t>(text[i++] - '0');
            if (i < text.size() && (text[i] == 'K' || text[i] == 'k')) value <<= 10;
            else if (i < text.size() && (text[i] == 'M' || text[i] == 'm')) value <<= 20;
            return value;
        }
        return 0;
#elif defined(__APPLE__)
        std::uint64_t bytes = 0;
        size_t len = sizeof(bytes);
        if (sysctlbyname("hw.l3cachesize", &bytes, &len, nullptr, 0) == 0) return bytes;
        return 0;
#elif defined(_WIN32)
        DWORD len = 0;
        GetLogicalProcessorInformationEx(RelationCache, nullptr, &len);
        if (len == 0) return 0;
        std::vector<char> buf(len);
        if (!GetLogicalProcessorInformationEx(RelationCache,
                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data()), &len))
            return 0;
        for (DWORD off = 0; off < len;) {
            auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data() + off);
            if (info->Relationship == RelationCache && info->Cache.Level == 3)
                return static_cast<std::uint64_t>(info->Cache.CacheSize);
            off += info->Size;
        }
        return 0;
#else
        return 0;
#endif
    }

    unsigned defaultHelperCount() {
        static const unsigned count = (physicalCoreCount() / numaNodeCount()) / 2;
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

    void setEngageAlways(bool always) { setEngagement(always ? 2 : -1); }

    PoolStats poolStats() {
        if (!gPool) return PoolStats();
        std::lock_guard<std::mutex> lock(gPool->m);
        return gPool->stats;
    }

}  // namespace sweep
}  // namespace proto
