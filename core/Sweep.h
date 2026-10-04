/*
 * Sweep.h -- Phase 5 of the collector, the sweep, and its helper threads
 * (internal).
 *
 * Design: docs/specs/2026-10-03-collector-throughput-design.md, sections 5.3
 * and 6, with the maintainer's decisions of section 14.
 *
 *   * SweeperLocal and sweepSegments: the state one sweeping thread owns and
 *     the multi-cursor inner loop.  The collector thread with no helper is
 *     the serial sweep of earlier versions (K = 0).
 *   * claimRun: the partition of a cycle's segment list into runs of
 *     kClaimRun segments, each claimed by exactly one sweeper.
 *   * sweepCycle: the whole Phase 5 of one cycle.  The collector thread
 *     sweeps; while mutators wait for headroom, up to K helper threads of one
 *     process-wide pool claim runs too.  The collector never waits for a
 *     helper to arrive, only for helpers that claimed work to finish it.
 *     Embedder finalizers (ProtoExternalPointer) found by helpers run on the
 *     collector thread after the join, one at a time.
 *
 * Synchronisation: std::mutex and std::condition_variable only -- never
 * std::counting_semaphore, std::latch or std::barrier (libstdc++ 13 loses
 * wakeups in the first; see the spec, section 3).
 *
 * Helpers are std::threads owned by protoCore, like the collector thread,
 * and are not ProtoThreads: they take no part in stop-the-world quorums or
 * grace periods, hold no cell between jobs, and never allocate.
 */

#ifndef PROTOCORE_SWEEP_H
#define PROTOCORE_SWEEP_H

#include "../headers/proto_internal.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace proto {
namespace sweep {

    // --- Free chunks (defined in ProtoSpace.cpp; globalMutex held) -----------

    ProtoSpace::FreeChunk* takeFreeChunk(ProtoSpace* space);
    void recycleFreeChunk(ProtoSpace* space, ProtoSpace::FreeChunk* chunk);
    /** Publish a chain of free cells as one chunk of `space`'s freelist.
     *  The caller holds ProtoSpace::globalMutex. */
    void publishFreeChunk(ProtoSpace* space, Cell* head, Cell* tail, proto_ulong count);

    // --- One sweeper ------------------------------------------------------------

    /** Segments a sweeper hands back in one compare-and-swap per batch. */
    constexpr proto_ulong kSegmentRecycleBatch = 1024;
    /** The most segment chains one sweeper walks in lockstep. */
    constexpr int kMaxSweepCursors = 32;
    /** The default number of chains walked in lockstep (sweepCursors()). */
    constexpr unsigned kDefaultSweepCursors = 8;

    /**
     * A LIFO chain of processed DirtySegments, private to its sweeper until
     * pushAllOnto publishes it with one compare-and-swap (release, so the
     * segments' contents are visible to the thread that pops them).
     */
    struct SegmentChain {
        DirtySegment* head = nullptr;
        DirtySegment* tail = nullptr;
        proto_ulong count = 0;
        void push(DirtySegment* seg);
        void pushAllOnto(std::atomic<DirtySegment*>& list);
    };

    /**
     * The state one sweeping thread owns: the free chunk under construction,
     * its counters, the processed-segment chains, its dead cells when they
     * are freed only after a grace period, and -- on a helper -- the mutable
     * refs its finalizers recorded and the embedder-finalized cells it left
     * to the collector thread.  finish() publishes the chunk and the chains.
     */
    struct SweeperLocal {
        ProtoSpace* space;
        bool deferFree;
        bool helper;
        std::vector<Cell*>* deadCells;
        Cell* chunkHead = nullptr;
        Cell* chunkTail = nullptr;
        proto_ulong chunkCount = 0;
        proto_ulong reclaimed = 0;
        proto_ulong swept = 0;
        proto_ulong segments = 0;
        bool severalAllocators = true;    // adaptive::severalAllocators, read at sweep start
        long long runway = 0;             // pacing's runway, read at sweep start
        proto_ulong wideSegments = 0;     // taken while the walk was wide
        proto_ulong narrowSegments = 0;   // taken while it was one chain
        SegmentChain freeSegs;
        SegmentChain penSegs;
        std::vector<proto_ulong> finalizedRefs;     // helpers: the finalizer sink
        std::vector<Cell*> deferredExternal;        // helpers: embedder finalizers

        SweeperLocal(ProtoSpace* s, bool defer, std::vector<Cell*>* dead, bool isHelper = false)
            : space(s), deferFree(defer), helper(isHelper), deadCells(dead) {}

        void addBatch(Cell* head, Cell* tail, proto_ulong count);
        void retire(DirtySegment* seg, Cell* survivors);
        void finish();
    };

    struct SegmentCursor;
    /** Where one sweeper takes its segments: runs claimed from the cycle's
     *  shared list (claimRun), handed out one segment at a time; `onClaim`
     *  runs after each claim with whether segments remain. */
    struct SegmentSource {
        SegmentCursor* cursor = nullptr;
        DirtySegment* run = nullptr;   // the rest of the current claimed run
        // The collector's source: while *shared is false no helper touches
        // the list, so segments are taken from it directly, without a claim.
        // The hook still runs every kClaimRun segments.
        bool owner = false;
        const bool* shared = nullptr;
        unsigned sinceHook = 0;
        void (*onClaim)(void*, bool) = nullptr;
        void* onClaimArg = nullptr;
        DirtySegment* next();
    };

    /** Sweep every segment `source` yields into `L`.  The cursors refill
     *  from the source as their chains end, so all of them stay busy until
     *  the shared list is exhausted. */
    void sweepSegments(SegmentSource& source, SweeperLocal& L);

    // --- Partition ----------------------------------------------------------------

    /** Segments per claim: about 770 cells at the measured ~6 cells per
     *  segment, a lock occupancy of about 2 % per sweeper. */
    constexpr unsigned kClaimRun = 128;

    /** The unclaimed rest of a cycle's segment list. */
    struct SegmentCursor {
        std::mutex lock;
        DirtySegment* next = nullptr;
    };

    /**
     * Detach up to `run` segments from the front of `cursor` and return them
     * as a nullptr-terminated list, or nullptr when none is left.  Every
     * segment is returned by exactly one call.  `moreLeft`, when given, says
     * whether segments remain after this claim.
     */
    DirtySegment* claimRun(SegmentCursor& cursor, unsigned run, bool* moreLeft = nullptr);

    // --- A cycle's sweep --------------------------------------------------------

    struct CycleSweep {
        proto_ulong reclaimed = 0;
        proto_ulong swept = 0;
        proto_ulong segments = 0;
        unsigned helpersJoined = 0;
        proto_ulong wideSegments = 0;     // since 2.14.2: segments of a wide walk
        proto_ulong narrowSegments = 0;   // and of a one-chain walk
    };

    /**
     * Phase 5 of one cycle of `space`, on its collector thread: sweep `list`
     * with the helpers when they are engaged.  On return every free chunk and
     * segment chain is published, the helpers' finalized mutable refs are
     * appended to space->gcFinalizedMutableRefs (before Phase 5b reads it),
     * their dead cells to `deadCells` (deferFree), and the embedder
     * finalizers they left have run on this thread.
     */
    CycleSweep sweepCycle(ProtoSpace* space, DirtySegment* list, bool deferFree,
                          std::vector<Cell*>& deadCells);

    // --- Configuration and lifecycle ----------------------------------------------
    //
    // Every hardware-sensitive parameter is configurable (environment and
    // ProtoSpace's static API); the defaults were measured on one
    // notebook-class CPU only (docs/reports/2026-10-03-collector-throughput.md,
    // "Hardware class").

    /** Chains walked in lockstep (1 = the single-chain walk): the API, else
     *  PROTOCORE_GC_SWEEP_CURSORS (1..32), else 8. */
    unsigned sweepCursors();
    void setSweepCursors(unsigned count);   // 0 restores the default
    /** Prefetch the next cell of each chain: the API, else
     *  PROTOCORE_GC_SWEEP_PREFETCH (0 or 1), else on. */
    bool sweepPrefetch();
    void setSweepPrefetch(int on);          // -1 restores the default

    /** When the helpers are offered a sweep, and when its walk is wide. */
    enum class Engagement {
        Measured = 0,      // while a mutator waits, and only while that pays (default)
        WhileWaiting = 1,  // while a mutator waits
        Always = 2         // every sweep (diagnosis, tests)
    };
    /** The API, else PROTOCORE_GC_SWEEP_ENGAGE (measured | waiting |
     *  always), else Measured. */
    Engagement engagement();
    void setEngagement(int mode);           // -1 restores the default

    /**
     * The chains a sweeper walks in lockstep now (since 2.14.2): `maxCursors`
     * (sweepCursors()) when `short_` (mutatorsShort: a mutator waits for
     * headroom, several threads allocate, or the cells left are below the
     * runway), or always under Engagement::Always; one chain otherwise.  A sweep nobody needs soon gains nothing from finishing
     * early, and one that runs far ahead of the mutators' consumption leaves
     * the cells it frees to go cold, or modified in the collector's cache,
     * before they are reused: protoClojure coll_alloc with one task ran 26 %
     * slower with eight chains than with one (2026-10-04 re-measurement),
     * whatever the cores the two threads ran on.  Re-read at every segment a
     * cursor ends, so a sweep widens as soon as a mutator runs short.
     */
    unsigned cursorsFor(unsigned maxCursors, Engagement mode, bool short_);

    /** The sweep of `space` should walk wide (relaxed reads; no lock; since
     *  2.14.2): a mutator of any space waits for heap headroom; or more than
     *  one thread of `space` allocated since its last cycle (`several`:
     *  several mutators consume faster than a one-chain walk frees, and its
     *  CPU time -- about twice the wide walk's per cell -- competes with
     *  them); or the cells left before the ceiling (the freelist plus the
     *  room below the soft limit, or the hard limit without one; the freelist
     *  alone without a limit) hold less than pacing's `runway` or two free
     *  chunks per running thread.  So a one-chain walk runs only while a
     *  single mutator has its next cycle's runway; the mutator takes the
     *  chunk published last first, so it reuses cells the sweep has just
     *  touched. */
    bool mutatorsShort(ProtoSpace* space, bool several, long long runway);

    /** Segments taken by a cursor of a wide walk (more than one chain) and
     *  of a one-chain walk, process-wide, since the start. */
    struct WalkStats {
        std::uint64_t wideSegments = 0;
        std::uint64_t narrowSegments = 0;
    };
    WalkStats walkStats();

    /** The measured engagement's state of one space (Engagement::Measured). */
    struct EngageState {
        double soloNsPerCell = 0.0;   // the last wide sweep without helpers
        unsigned backoff = 0;         // sweeps held back after the last failure
        unsigned skip = 0;            // sweeps still to hold back
    };
    constexpr proto_ulong kMinCellsToMeasure = 100000;
    constexpr unsigned kMaxEngageBackoff = 64;
    /** One sweep's verdict (pure): `engaged` -- helpers swept; `heldBack` --
     *  helpers were wanted but held back; `swept` cells in `nanos` of the
     *  sweep's wall time; `wide` -- the walk was wide (a mutator waited).  A
     *  sweep with helpers no faster per cell than the last wide one without
     *  them doubles the backoff (1, 2, 4 ... 64 sweeps); a faster one resets
     *  it.  Only a wide solo sweep is the comparison (since 2.14.2): helpers
     *  join only while mutators wait, when the walk is wide too.  Sweeps
     *  under kMinCellsToMeasure cells are not measured. */
    void noteSweep(EngageState& state, bool engaged, bool heldBack, proto_ulong swept, double nanos,
                   bool wide);
    /** No comparison yet: the next sweep that wants helpers runs without
     *  them and becomes it (pure). */
    bool holdBackForComparison(const EngageState& state);

    /** Physical cores, NUMA nodes and the L3 size of this machine (1, 1 and
     *  0 when unknown). */
    unsigned numaNodeCount();
    std::uint64_t l3CacheBytes();
    /** Drop `space`'s engagement measurements (~ProtoSpace). */
    void forgetSpace(const ProtoSpace* space);

    /** The default helper count: half the physical cores of one NUMA node
     *  ((physical cores / NUMA nodes) / 2; 0 on a single core): helpers then
     *  stay within a node's worth of cores on a multi-socket machine. */
    unsigned defaultHelperCount();
    /** Physical cores of this machine (1 when unknown). */
    unsigned physicalCoreCount();
    /** The helper count in force: setHelperCount, else PROTOCORE_GC_SWEEP_THREADS
     *  (0..64; any other value is ignored), else the default. */
    unsigned helperCount();
    void setHelperCount(unsigned count);
    /** Parse PROTOCORE_GC_SWEEP_THREADS: false unless 0..64 in decimal. */
    bool parseHelperCount(const char* text, unsigned& out);
    /** Stop and join the helpers when no space is left (~ProtoSpace, after
     *  its collector joined).  A later space starts a new pool. */
    void shutdownPoolIfNoSpaces();

    /** True on a helper thread (getFreeCells aborts there: helpers never
     *  allocate). */
    bool isHelperThread();
    /** Where ProtoObjectCell::finalize records a mutable ref on a helper;
     *  nullptr on every other thread (it then uses the space's vector). */
    std::vector<proto_ulong>* finalizedRefSink();

    // --- Test hooks ----------------------------------------------------------------

    /** Run `fn` with this thread marked as a helper (the getFreeCells guard
     *  death test). */
    void runAsHelperForTest(void (*fn)(void*), void* arg);
    /** Engage the helpers on every cycle (setEngagement(Always) /
     *  setEngagement(-1)). */
    void setEngageAlways(bool always);
    struct PoolStats {
        std::uint64_t jobs = 0;            // sweeps the helpers were offered
        std::uint64_t helperRuns = 0;      // helpers that joined a job
        std::uint64_t refusedBusy = 0;     // a collector found the pool busy
        std::uint64_t concurrentJobsMax = 0;
        unsigned threads = 0;              // helper threads alive
        std::uint64_t starts = 0;          // pools started
        std::uint64_t heldBack = 0;        // sweeps not offered: helpers did not pay
    };
    PoolStats poolStats();

}  // namespace sweep
}  // namespace proto

#endif
