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
    /** Segment chains one sweeper walks in lockstep. */
    constexpr int kSweepCursors = 8;

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

    /** Sweep every segment of `list` (nullptr-terminated, owned by the
     *  caller) into `L`. */
    void sweepSegments(DirtySegment* list, SweeperLocal& L);

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

    /** The default helper count: physical cores / 2 (0 on a single core). */
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
    /** Engage the helpers on every cycle, not only while mutators wait
     *  (also PROTOCORE_GC_SWEEP_ENGAGE=always). */
    void setEngageAlways(bool always);
    struct PoolStats {
        std::uint64_t jobs = 0;            // sweeps the helpers were offered
        std::uint64_t helperRuns = 0;      // helpers that joined a job
        std::uint64_t refusedBusy = 0;     // a collector found the pool busy
        std::uint64_t concurrentJobsMax = 0;
        unsigned threads = 0;              // helper threads alive
        std::uint64_t starts = 0;          // pools started
    };
    PoolStats poolStats();

}  // namespace sweep
}  // namespace proto

#endif
