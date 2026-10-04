// ParallelSweepTests.cpp -- the sweep with collector helper threads (M4).
//
// Design: docs/specs/2026-10-03-collector-throughput-design.md, sections 6
// and 10.1, with the maintainer's decisions of section 14 (embedder
// finalizers stay serial on the collector thread; helpers engage only while
// mutators wait -- these tests force them with the engage-always hook).
//
// Every case counts; none times.  The existing GC suites also run with three
// helpers engaged on every cycle (test/CMakeLists.txt, the ParallelSweep.*
// ctest entries with PROTOCORE_GC_SWEEP_THREADS=3 and
// PROTOCORE_GC_SWEEP_ENGAGE=always).

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"
#include "../core/AdaptiveHeap.h"
#include "../core/Sweep.h"
#include "SanitizerSupport.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace proto;

namespace {

// Helpers engaged on every cycle for the scope, K of them.
struct EngagedHelpers {
    unsigned before;
    explicit EngagedHelpers(unsigned k) : before(ProtoSpace::collectorHelperThreads()) {
        ProtoSpace::setCollectorHelperThreads(k);
        sweep::setEngageAlways(true);
    }
    ~EngagedHelpers() {
        sweep::setEngageAlways(false);
        ProtoSpace::setCollectorHelperThreads(before);
    }
};

std::uint64_t completed(ProtoSpace& space) {
    std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
    return adaptive::cyclesCompleted(&space);
}

// Request one cycle and wait for it at safepoints.
void runCycle(ProtoSpace& space) {
    const std::uint64_t before = completed(space);
    {
        std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
        if (!space.gcStarted) {
            adaptive::noteCycleRequested(&space);
            space.gcStarted = true;
        }
        space.gcCV.notify_all();
    }
    while (completed(space) == before) {
        space.rootContext->safepoint();
        std::this_thread::yield();
    }
}

// `objects` objects of garbage in contexts of three (segments of ~6 cells).
void garbage(ProtoSpace& space, int objects) {
    for (int done = 0; done < objects; done += 3) {
        ProtoContext sub(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
        for (int j = 0; j < 3; ++j) (void) sub.newObject(false);
    }
}

// A live list of `n` integers in a root set, built in chunks.
struct LiveList {
    ProtoRootSet* rs;
    ProtoRootSet::Handle h;
    LiveList(ProtoSpace& space, int n) {
        ProtoContext* root = space.rootContext;
        rs = space.createRootSet("parallel-sweep");
        h = rs->add(root->newList()->asObject(root));
        for (int done = 0; done < n;) {
            ProtoContext sub(&space, root, nullptr, nullptr, nullptr, nullptr);
            const ProtoList* list = rs->resolve(h)->asList(&sub);
            const int chunk = std::min(2000, n - done);
            for (int i = 0; i < chunk; ++i) list = list->appendLast(&sub, sub.fromInteger(done + i));
            rs->remove(h);
            h = rs->add(list->asObject(&sub));
            done += chunk;
        }
    }
    bool intact(ProtoSpace& space, int n) {
        ProtoContext* ctx = space.rootContext;
        const ProtoList* list = rs->resolve(h)->asList(ctx);
        if (static_cast<int>(list->getSize(ctx)) != n) return false;
        for (int i = 0; i < n; ++i) {
            const ProtoObject* e = list->getAt(ctx, i);
            if (!e || e->asLong(ctx) != i) return false;
        }
        return true;
    }
};

}  // namespace

// --- 10.1.1 Claiming partitions the list ---------------------------------------

namespace {
std::vector<DirtySegment> makeSegments(std::size_t n) {
    std::vector<DirtySegment> segs(n);
    for (std::size_t i = 0; i < n; ++i) {
        segs[i].cellChain = nullptr;
        segs[i].next = i + 1 < n ? &segs[i + 1] : nullptr;
    }
    return segs;
}
}  // namespace

TEST(ParallelSweepClaim, EverySegmentIsClaimedExactlyOnce) {
    const unsigned R = sweep::kClaimRun;
    for (std::size_t n : {std::size_t(0), std::size_t(1), std::size_t(R - 1), std::size_t(R),
                          std::size_t(R + 1), std::size_t(100000)}) {
        for (int claimers : {1, 8}) {
            std::vector<DirtySegment> segs = makeSegments(n);
            sweep::SegmentCursor cursor;
            cursor.next = n ? &segs[0] : nullptr;
            std::vector<std::atomic<int>> seen(n);
            for (auto& s : seen) s.store(0);
            std::atomic<int> emptyWhileLeft{0};
            auto claimer = [&] {
                for (;;) {
                    DirtySegment* run = sweep::claimRun(cursor, R);
                    if (!run) break;
                    std::size_t k = 0;
                    for (DirtySegment* s = run; s; s = s->next, ++k)
                        seen[static_cast<std::size_t>(s - segs.data())].fetch_add(1);
                    if (k == 0 || k > R) emptyWhileLeft.fetch_add(1);
                }
            };
            std::vector<std::thread> ts;
            for (int t = 0; t < claimers; ++t) ts.emplace_back(claimer);
            for (auto& t : ts) t.join();
            for (std::size_t i = 0; i < n; ++i)
                ASSERT_EQ(seen[i].load(), 1) << "segment " << i << " of " << n << ", " << claimers << " claimers";
            EXPECT_EQ(emptyWhileLeft.load(), 0);
            EXPECT_EQ(cursor.next, nullptr);
        }
    }
}

TEST(ParallelSweepClaim, MoreLeftReportsTheRest) {
    std::vector<DirtySegment> segs = makeSegments(sweep::kClaimRun + 1);
    sweep::SegmentCursor cursor;
    cursor.next = &segs[0];
    bool more = false;
    ASSERT_NE(sweep::claimRun(cursor, sweep::kClaimRun, &more), nullptr);
    EXPECT_TRUE(more);
    ASSERT_NE(sweep::claimRun(cursor, sweep::kClaimRun, &more), nullptr);
    EXPECT_FALSE(more);
    EXPECT_EQ(sweep::claimRun(cursor, sweep::kClaimRun, &more), nullptr);
}

// --- 10.1.2 Same result as the serial sweep -------------------------------------

namespace {
struct CycleResult {
    proto_ulong reclaimed;
    proto_ulong live;
    int freeCells;
    bool intact;
};

CycleResult oneCycle(unsigned helpers, unsigned cursors = 0, int prefetch = -1) {
    EngagedHelpers h(helpers);
    ProtoSpace::setSweepCursors(cursors);
    ProtoSpace::setSweepPrefetch(prefetch);
    ProtoSpace space;
    LiveList live(space, 30000);
    runCycle(space);          // the live list's build garbage
    garbage(space, 600000);
    runCycle(space);
    CycleResult r;
    r.reclaimed = space.reclaimedLastCycle.load();
    r.live = space.liveCellsLastCycle.load();
    {
        std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
        r.freeCells = space.freeCellsCount;
    }
    r.intact = live.intact(space, 30000);
    ProtoSpace::setSweepCursors(0);
    ProtoSpace::setSweepPrefetch(-1);
    return r;
}
}  // namespace

// The freelist count after a cycle also counts the cells the main thread's
// allocation batch holds, and the first space a test process builds after
// other tests ended with a batch 32,768 cells larger (measured: the first
// serial cycle of a run differed from every later one, serial or not).  One
// discarded cycle first makes the comparison about the sweep.
static void warmUp() { (void) oneCycle(0); }

TEST(ParallelSweep, HelpersFreeTheSameCellsAsTheSerialSweep) {
    warmUp();
    const std::uint64_t runs0 = sweep::poolStats().helperRuns;
    const CycleResult serial = oneCycle(0);
    EXPECT_EQ(sweep::poolStats().helperRuns, runs0) << "K = 0 used a helper";
    const CycleResult parallel = oneCycle(3);
    EXPECT_GT(sweep::poolStats().helperRuns, runs0) << "no helper joined the K = 3 sweep";
    EXPECT_TRUE(serial.intact);
    EXPECT_TRUE(parallel.intact);
    EXPECT_EQ(parallel.reclaimed, serial.reclaimed);
    EXPECT_EQ(parallel.live, serial.live);
    EXPECT_EQ(parallel.freeCells, serial.freeCells);
    EXPECT_GE(parallel.reclaimed, 1200000u);
}

// The hardware-sensitive knobs change how the sweep walks, never what it
// frees: one chain, 32 chains, no prefetch, with and without helpers.
TEST(ParallelSweep, CursorsAndPrefetchFreeTheSameCells) {
    warmUp();
    const CycleResult reference = oneCycle(0, 1, 0);
    for (unsigned helpers : {0u, 3u}) {
        for (unsigned cursors : {1u, 2u, 8u, 32u}) {
            for (int prefetch : {0, 1}) {
                const CycleResult r = oneCycle(helpers, cursors, prefetch);
                EXPECT_TRUE(r.intact);
                EXPECT_EQ(r.reclaimed, reference.reclaimed)
                    << helpers << " helpers, " << cursors << " cursors, prefetch " << prefetch;
                EXPECT_EQ(r.live, reference.live);
                EXPECT_EQ(r.freeCells, reference.freeCells);
            }
        }
    }
}

// After a cycle with helpers, no reachable cell carries a mark bit: the next
// cycle's mark would otherwise skip it (and its subgraph) and free it.
TEST(ParallelSweep, NoMarkBitSurvivesACycle) {
    EngagedHelpers h(3);
    ProtoSpace space;
    LiveList live(space, 20000);
    for (int i = 0; i < 4; ++i) {
        garbage(space, 300000);
        runCycle(space);
        ProtoContext* ctx = space.rootContext;
        const ProtoList* list = live.rs->resolve(live.h)->asList(ctx);
        int marked = 0;
        for (int k = 0; k < 20000; ++k) {
            const ProtoObject* e = list->getAt(ctx, k);
            if (ProtoObject::isCellPointer(e) && ProtoObject::asCellPointer(e)->isMarked()) ++marked;
        }
        const Cell* listCell = ProtoObject::asCellPointer(list->asObject(ctx));
        EXPECT_FALSE(listCell->isMarked());
        EXPECT_EQ(marked, 0);
    }
    EXPECT_TRUE(live.intact(space, 20000));
}

// --- 10.1.3 / 10.1.4 Embedder finalizers -----------------------------------------

namespace {
std::mutex gIdsMutex;
std::set<std::thread::id> gFinalizerThreads;
std::atomic<int> gInFlight{0};
std::atomic<int> gMaxInFlight{0};
std::atomic<long> gFinalized{0};

void recordingFinalizer(void*) {
    const int now = gInFlight.fetch_add(1) + 1;
    int seen = gMaxInFlight.load();
    while (now > seen && !gMaxInFlight.compare_exchange_weak(seen, now)) {}
    {
        std::lock_guard<std::mutex> lock(gIdsMutex);
        gFinalizerThreads.insert(std::this_thread::get_id());
    }
    gFinalized.fetch_add(1);
    gInFlight.fetch_sub(1);
}
}  // namespace

TEST(ParallelSweep, EmbedderFinalizersRunSeriallyOnTheCollectorThread) {
    EngagedHelpers h(3);
    gFinalizerThreads.clear();
    gMaxInFlight = 0;
    gFinalized = 0;
    ProtoSpace space;
    const std::thread::id collector = space.gcThread->get_id();
    const std::uint64_t runs0 = sweep::poolStats().helperRuns;
    constexpr int kPointers = 100000;
    // External pointers mixed with ordinary garbage, so the helpers meet
    // them in their runs.
    for (int done = 0; done < kPointers; done += 2) {
        ProtoContext sub(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
        (void) sub.fromExternalPointer(nullptr, recordingFinalizer);
        (void) sub.fromExternalPointer(nullptr, recordingFinalizer);
        for (int j = 0; j < 6; ++j) (void) sub.newObject(false);
    }
    runCycle(space);
    runCycle(space);
    EXPECT_GT(sweep::poolStats().helperRuns, runs0) << "no helper joined";
    EXPECT_EQ(gFinalized.load(), kPointers);
    EXPECT_EQ(gMaxInFlight.load(), 1) << "embedder finalizers overlapped";
    std::lock_guard<std::mutex> lock(gIdsMutex);
    ASSERT_EQ(gFinalizerThreads.size(), 1u) << "finalizers ran on several threads";
    EXPECT_EQ(*gFinalizerThreads.begin(), collector) << "a finalizer ran off the collector thread";
    EXPECT_EQ(gFinalizerThreads.count(std::this_thread::get_id()), 0u) << "a finalizer ran on a mutator";
}

// --- 10.1.5 Mutable refs reach Phase 5b -------------------------------------------

TEST(ParallelSweep, DroppedMutablesReleaseTheirEntriesWithHelpers) {
    EngagedHelpers h(3);
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;
    const ProtoString* key = ProtoString::createSymbol(ctx, "v");
    std::vector<proto_ulong> refs;
    constexpr int kObjects = 60000;
    for (int i = 0; i < kObjects; i += 3) {
        ProtoContext sub(&space, ctx, nullptr, nullptr, nullptr, nullptr);
        for (int j = 0; j < 3; ++j) {
            const ProtoObject* o = sub.newObject(true);
            o->setAttribute(&sub, key, sub.fromInteger(i + j));
            refs.push_back(toImpl<const ProtoObjectCell>(o)->mutable_ref);
        }
    }
    garbage(space, 300000);
    runCycle(space);
    runCycle(space);
    std::size_t left = 0;
    for (proto_ulong ref : refs) {
        const ProtoSparseList* root =
            globalMutableShards[ref % ProtoSpace::MUTABLE_ROOT_SHARDS].root.load();
        if (sparseListGetRaw(ctx, root, ref) != nullptr) ++left;
    }
    EXPECT_EQ(left, 0u) << "entries of dropped mutables were not released";
}

// --- 10.1.7 Helpers never allocate -----------------------------------------------

namespace {
ProtoSpace* gAllocSpace = nullptr;
void allocateOnHelper(void*) { (void) gAllocSpace->getFreeCells(gAllocSpace->rootContext); }
}  // namespace

TEST(ParallelSweepDeathTest, AHelperThatAllocatesAborts) {
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    EXPECT_DEATH({
        ProtoSpace space;
        gAllocSpace = &space;
        sweep::runAsHelperForTest(allocateOnHelper, nullptr);
    }, "collector helper thread tried to allocate");
}

// --- 10.1.8 Teardown --------------------------------------------------------------

// A space destroyed while a sweep with helpers runs: the destructor joins the
// collector, which waits only for helpers holding claimed work.
TEST(ParallelSweepTeardown, DestroyingASpaceDuringASweepWithHelpers) {
    EngagedHelpers h(3);
    for (int i = 0; i < 200; ++i) {
        ProtoSpace space;
        garbage(space, 30000);
        std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
        space.gcStarted = true;
        space.gcCV.notify_all();
        // destroyed with the cycle requested or running
    }
    SUCCEED();
}

TEST(ParallelSweepTeardown, TwoSpacesDestroyedInBothOrders) {
    EngagedHelpers h(3);
    for (int order = 0; order < 2; ++order) {
        auto* a = new ProtoSpace();
        auto* b = new ProtoSpace();
        garbage(*a, 200000);
        garbage(*b, 200000);
        runCycle(*a);
        runCycle(*b);
        if (order == 0) { delete a; delete b; }
        else { delete b; delete a; }
    }
    EXPECT_EQ(sweep::poolStats().threads, 0u) << "the last space did not stop the helpers";
}

TEST(ParallelSweepTeardown, TheLastSpaceStopsThePoolAndANewSpaceRestartsIt) {
    EngagedHelpers h(3);
    {
        ProtoSpace space;
        garbage(space, 300000);
        runCycle(space);
        EXPECT_EQ(sweep::poolStats().threads, 3u);
    }
    EXPECT_EQ(sweep::poolStats().threads, 0u);
    const std::uint64_t starts = sweep::poolStats().starts;
    const std::uint64_t runs = sweep::poolStats().helperRuns;
    {
        ProtoSpace space;
        garbage(space, 300000);
        runCycle(space);
        EXPECT_EQ(sweep::poolStats().starts, starts + 1);
        EXPECT_GT(sweep::poolStats().helperRuns, runs);
    }
}

TEST(ParallelSweepTeardownDeathTest, ExitWithALiveSpaceAndParkedHelpersIsClean) {
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    EXPECT_EXIT({
        ProtoSpace::setCollectorHelperThreads(3);
        sweep::setEngageAlways(true);
        auto* space = new ProtoSpace();   // never destroyed
        garbage(*space, 300000);
        runCycle(*space);
        std::exit(0);
    }, ::testing::ExitedWithCode(0), "");
}

// --- 10.1.9 Several spaces ---------------------------------------------------------

namespace {
proto_ulong twoSpacesReclaimed(unsigned helpers) {
    EngagedHelpers h(helpers);
    ProtoSpace a;
    ProtoSpace b;
    garbage(a, 300000);
    garbage(b, 300000);
    runCycle(a);   // deferred free: two spaces are live
    const proto_ulong ra = a.reclaimedLastCycle.load();
    runCycle(b);
    return ra + b.reclaimedLastCycle.load();
}
}  // namespace

TEST(ParallelSweepMultiSpace, DeferredFreeFreesTheSameCells) {
    const proto_ulong serial = twoSpacesReclaimed(0);
    const proto_ulong parallel = twoSpacesReclaimed(3);
    EXPECT_EQ(parallel, serial);
    EXPECT_GE(parallel, 1200000u);
}

// Both spaces' cycles requested at once: their sweeps never share the pool
// (one job at a time, enforced by the pool's single job slot; a collector
// that finds the pool busy sweeps alone and never waits for it), and both
// complete with every cell accounted for.
TEST(ParallelSweepMultiSpace, TwoSpacesCollectingBackToBackShareOnePool) {
    EngagedHelpers h(3);
    ProtoSpace a;
    ProtoSpace b;
    for (int i = 0; i < 5; ++i) {
        garbage(a, 200000);
        garbage(b, 200000);
        const std::uint64_t ca = completed(a), cb = completed(b);
        {
            std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
            a.gcStarted = true;
            b.gcStarted = true;
            a.gcCV.notify_all();
            b.gcCV.notify_all();
        }
        while (completed(a) == ca || completed(b) == cb) {
            a.rootContext->safepoint();
            b.rootContext->safepoint();
            std::this_thread::yield();
        }
        EXPECT_GE(a.reclaimedLastCycle.load(), 400000u);
        EXPECT_GE(b.reclaimedLastCycle.load(), 400000u);
    }
    EXPECT_LE(sweep::poolStats().concurrentJobsMax, 1u);
}

// --- 10.1.10 Fork -------------------------------------------------------------------

#if !defined(_WIN32)
TEST(ParallelSweepFork, AChildForkedAfterThePoolStartedCompletesACycle) {
#if defined(PROTO_TEST_TSAN)
    // ThreadSanitizer kills a child that starts a thread after a
    // multi-threaded fork ("not supported"), and every new space starts its
    // collector thread.  The case runs in the Release and ASan jobs.
    GTEST_SKIP() << "ThreadSanitizer does not support threads after a multi-threaded fork";
#endif
    EngagedHelpers h(3);
    ProtoSpace parent;
    garbage(parent, 300000);
    runCycle(parent);
    ASSERT_GT(sweep::poolStats().threads, 0u);
    const pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        // The child has no helper (and no collector of `parent`): a new
        // space's cycles start a new pool and complete.
        auto* child = new ProtoSpace();
        garbage(*child, 300000);
        runCycle(*child);
        const bool ok = child->reclaimedLastCycle.load() >= 600000
                        && sweep::poolStats().threads == 3;
        std::_Exit(ok ? 0 : 1);
    }
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status)) << "the child did not exit normally";
    EXPECT_EQ(WEXITSTATUS(status), 0);
}
#endif

// --- 10.1.11 Configuration -------------------------------------------------------

TEST(ParallelSweepConfig, ParseHelperCount) {
    unsigned k = 99;
    EXPECT_TRUE(sweep::parseHelperCount("0", k));  EXPECT_EQ(k, 0u);
    EXPECT_TRUE(sweep::parseHelperCount("1", k));  EXPECT_EQ(k, 1u);
    EXPECT_TRUE(sweep::parseHelperCount("64", k)); EXPECT_EQ(k, 64u);
    EXPECT_FALSE(sweep::parseHelperCount("65", k));
    EXPECT_FALSE(sweep::parseHelperCount("abc", k));
    EXPECT_FALSE(sweep::parseHelperCount("", k));
    EXPECT_FALSE(sweep::parseHelperCount("-1", k));
    EXPECT_FALSE(sweep::parseHelperCount(nullptr, k));
}

TEST(ParallelSweepConfig, DefaultIsHalfThePhysicalCoresOfANode) {
    EXPECT_EQ(sweep::defaultHelperCount(),
              (sweep::physicalCoreCount() / sweep::numaNodeCount()) / 2);
    EXPECT_GE(sweep::physicalCoreCount(), 1u);
    std::printf("[ config ] physical cores %u, default helpers %u\n",
                sweep::physicalCoreCount(), sweep::defaultHelperCount());
}

TEST(ParallelSweepConfig, CursorsPrefetchAndEngagementAreConfigurable) {
    EXPECT_EQ(sweep::kDefaultSweepCursors, 8u);
    ProtoSpace::setSweepCursors(1);
    EXPECT_EQ(ProtoSpace::sweepCursors(), 1u);
    ProtoSpace::setSweepCursors(100);
    EXPECT_EQ(ProtoSpace::sweepCursors(), 32u);
    ProtoSpace::setSweepCursors(0);
    EXPECT_GE(ProtoSpace::sweepCursors(), 1u);
    ProtoSpace::setSweepPrefetch(0);
    EXPECT_FALSE(ProtoSpace::sweepPrefetch());
    ProtoSpace::setSweepPrefetch(1);
    EXPECT_TRUE(ProtoSpace::sweepPrefetch());
    ProtoSpace::setSweepPrefetch(-1);
    for (int mode : {0, 1, 2}) {
        ProtoSpace::setCollectorHelperEngagement(mode);
        EXPECT_EQ(ProtoSpace::collectorHelperEngagement(), mode);
    }
    ProtoSpace::setCollectorHelperEngagement(7);   // invalid: the default
    ProtoSpace::setCollectorHelperEngagement(-1);
    std::printf("[ hardware ] physical cores %u, NUMA nodes %u, L3 %llu KiB; defaults: %u helpers, "
                "%u cursors, prefetch %d, engagement %d\n",
                sweep::physicalCoreCount(), sweep::numaNodeCount(),
                (unsigned long long) (sweep::l3CacheBytes() >> 10), sweep::defaultHelperCount(),
                ProtoSpace::sweepCursors(), ProtoSpace::sweepPrefetch() ? 1 : 0,
                ProtoSpace::collectorHelperEngagement());
}

// The measured engagement, as a pure rule: helpers that do not shorten the
// sweep are held back for 1, 2, 4 ... 64 sweeps; helpers that do are kept.
TEST(ParallelSweepConfig, MeasuredEngagementBacksOffWhenHelpersDoNotPay) {
    sweep::EngageState st;
    const proto_ulong n = 1000000;
    sweep::noteSweep(st, false, false, n, 20.0 * n, true);   // solo: 20 ns per cell
    EXPECT_DOUBLE_EQ(st.soloNsPerCell, 20.0);
    sweep::noteSweep(st, true, false, n, 12.0 * n, true);    // helpers faster: keep them
    EXPECT_EQ(st.skip, 0u);
    EXPECT_EQ(st.backoff, 0u);
    unsigned expected = 1;
    for (int failure = 0; failure < 9; ++failure) {
        sweep::noteSweep(st, true, false, n, 25.0 * n, true);   // helpers slower
        EXPECT_EQ(st.backoff, expected);
        EXPECT_EQ(st.skip, expected);
        // The held-back sweeps run alone and give the next comparison.
        const unsigned held = st.skip;
        for (unsigned k = 0; k < held; ++k) sweep::noteSweep(st, false, true, n, 20.0 * n, true);
        EXPECT_EQ(st.skip, 0u);
        expected = std::min(expected * 2, sweep::kMaxEngageBackoff);
    }
    EXPECT_EQ(st.backoff, sweep::kMaxEngageBackoff);
    sweep::noteSweep(st, true, false, n, 10.0 * n, true);    // helpers pay again
    EXPECT_EQ(st.backoff, 0u);
    // A small sweep says nothing.
    sweep::EngageState small;
    sweep::noteSweep(small, false, false, 1000, 1e9, true);
    EXPECT_DOUBLE_EQ(small.soloNsPerCell, 0.0);
    // A solo sweep walked one chain (nobody waited) is not the comparison:
    // helpers engage only while mutators wait, when the walk is wide.
    sweep::EngageState narrow;
    sweep::noteSweep(narrow, false, false, n, 60.0 * n, false);
    EXPECT_DOUBLE_EQ(narrow.soloNsPerCell, 0.0);
    // Without a comparison, the first sweep that wants helpers is held back
    // and becomes it.
    EXPECT_TRUE(sweep::holdBackForComparison(narrow));
    sweep::noteSweep(narrow, false, true, n, 20.0 * n, true);
    EXPECT_DOUBLE_EQ(narrow.soloNsPerCell, 20.0);
    EXPECT_FALSE(sweep::holdBackForComparison(narrow));
}

// --- Multi-cursor walk only while it pays (2.14.2) -------------------------------
//
// The 2026-10-04 re-measurement found protoClojure coll_alloc with one task
// 26 % slower than on 2.10.2 with the 8-chain walk, and as fast with one
// chain: a sweep that runs far ahead of the mutators' consumption leaves the
// cells it frees to go cold (or to sit modified in another core's cache)
// before they are reused, and every allocation then misses.  The mutator
// spent 10 G more cycles initialising new cells (perf, IBS).  The sweep's
// speed only buys time while a mutator waits for it, so the chains are
// walked in lockstep only then -- the rule that already engages the helpers.

TEST(ParallelSweepConfig, CursorsAreWideOnlyWhileAMutatorWaits) {
    using sweep::Engagement;
    EXPECT_EQ(sweep::cursorsFor(8, Engagement::Measured, false), 1u);
    EXPECT_EQ(sweep::cursorsFor(8, Engagement::Measured, true), 8u);
    EXPECT_EQ(sweep::cursorsFor(8, Engagement::WhileWaiting, false), 1u);
    EXPECT_EQ(sweep::cursorsFor(8, Engagement::WhileWaiting, true), 8u);
    // Diagnosis and tests: every sweep at the configured width.
    EXPECT_EQ(sweep::cursorsFor(8, Engagement::Always, false), 8u);
    EXPECT_EQ(sweep::cursorsFor(32, Engagement::Always, true), 32u);
    // One configured chain is one chain.
    EXPECT_EQ(sweep::cursorsFor(1, Engagement::Measured, true), 1u);
    EXPECT_EQ(sweep::cursorsFor(0, Engagement::Measured, true), 1u);
}

// The walk is wide when several threads allocate: they consume faster than a
// one-chain walk frees, and its CPU time (about twice the wide walk's per
// cell) competes with them (the 2026-10-04 adjustments report: protoJS
// records with six threads was 4-16 % slower with the one-chain walk).
TEST(ParallelSweepConfig, SeveralAllocatingThreadsWidenTheWalk) {
    ProtoSpace space;   // no limit, nobody waits
    EXPECT_FALSE(sweep::mutatorsShort(&space, false));
    EXPECT_TRUE(sweep::mutatorsShort(&space, true));
    // The refills since the last cycle end say how many threads allocate.
    runCycle(space);   // the count restarts at a cycle end
    {
        std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
        int a = 0, b = 0;
        adaptive::noteRefill(&space, &a);
        adaptive::noteRefill(&space, &a);
        EXPECT_FALSE(adaptive::severalAllocators(&space));
        adaptive::noteRefill(&space, &b);
        EXPECT_TRUE(adaptive::severalAllocators(&space));
    }
}

namespace {
std::atomic<int> gChurnersDone{0};
const ProtoObject* churnerMain(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                               const ProtoList*, const ProtoSparseList*) {
    for (int done = 0; done < 300000; done += 3) {
        ProtoContext sub(ctx->space, ctx, nullptr, nullptr, nullptr, nullptr);
        for (int j = 0; j < 3; ++j) (void) sub.newObject(false);
    }
    gChurnersDone.fetch_add(1);
    return PROTO_NONE;
}
}  // namespace

// Two threads allocated since the last cycle: its sweep walks wide.
TEST(ParallelSweep, ASweepAfterTwoAllocatingThreadsWalksWide) {
    ProtoSpace::setCollectorHelperEngagement(-1);
    ProtoSpace space;
    ProtoContext* root = space.rootContext;
    runCycle(space);   // the interval starts here
    gChurnersDone = 0;
    std::vector<const ProtoThread*> threads;
    for (int t = 0; t < 2; ++t)
        threads.push_back(space.newThread(root, ProtoString::createSymbol(root, "walk-churner"),
                                          churnerMain, nullptr, nullptr));
    {
        ProtoContext::UnmanagedScope parked(root);
        for (const ProtoThread* t : threads) const_cast<ProtoThread*>(t)->join(root);
    }
    ASSERT_EQ(gChurnersDone.load(), 2);
    const sweep::WalkStats before = sweep::walkStats();
    runCycle(space);
    const sweep::WalkStats after = sweep::walkStats();
    std::printf("[ walk ] narrow %llu wide %llu segments\n",
                (unsigned long long) (after.narrowSegments - before.narrowSegments),
                (unsigned long long) (after.wideSegments - before.wideSegments));
    EXPECT_GT(after.wideSegments, before.wideSegments);
    EXPECT_EQ(after.narrowSegments, before.narrowSegments);
}

// No mutator waits: the cycle's segments are walked one chain at a time.
TEST(ParallelSweep, ACycleNobodyWaitsForWalksOneChain) {
    ProtoSpace::setCollectorHelperEngagement(-1);
    ProtoSpace space;
    garbage(space, 300000);
    const sweep::WalkStats before = sweep::walkStats();
    runCycle(space);
    const sweep::WalkStats after = sweep::walkStats();
    std::printf("[ walk ] narrow %llu wide %llu segments\n",
                (unsigned long long) (after.narrowSegments - before.narrowSegments),
                (unsigned long long) (after.wideSegments - before.wideSegments));
    EXPECT_GT(after.narrowSegments, before.narrowSegments);
    EXPECT_EQ(after.wideSegments, before.wideSegments) << "a sweep nobody waited for walked several chains";
}

// A mutator waiting at the ceiling: the sweep it waits for walks wide.
TEST(ParallelSweep, ASweepAMutatorWaitsForWalksWide) {
    ProtoSpace::setCollectorHelperEngagement(-1);
    ProtoSpace space;
    space.setHeapLimits(0, relaxedLoad(space.heapSize) + 600000);
    const sweep::WalkStats before = sweep::walkStats();
    garbage(space, 3000000);
    const sweep::WalkStats after = sweep::walkStats();
    std::printf("[ walk ] narrow %llu wide %llu segments\n",
                (unsigned long long) (after.narrowSegments - before.narrowSegments),
                (unsigned long long) (after.wideSegments - before.wideSegments));
    EXPECT_GT(after.wideSegments, before.wideSegments) << "no sweep walked several chains while a mutator waited";
}

// Engagement::Always (diagnosis, tests): every sweep walks wide.
TEST(ParallelSweep, AlwaysEngagedWalksWide) {
    EngagedHelpers h(0);
    ProtoSpace space;
    garbage(space, 300000);
    const sweep::WalkStats before = sweep::walkStats();
    runCycle(space);
    const sweep::WalkStats after = sweep::walkStats();
    EXPECT_GT(after.wideSegments, before.wideSegments);
    EXPECT_EQ(after.narrowSegments, before.narrowSegments);
}

TEST(ParallelSweepConfig, TheApiOverridesAndZeroMeansNoPool) {
    EngagedHelpers h(0);
    EXPECT_EQ(ProtoSpace::collectorHelperThreads(), 0u);
    const std::uint64_t jobs = sweep::poolStats().jobs;
    ProtoSpace space;
    garbage(space, 300000);
    runCycle(space);
    EXPECT_EQ(sweep::poolStats().jobs, jobs) << "K = 0 offered a job to the pool";
    ProtoSpace::setCollectorHelperThreads(1000);
    EXPECT_EQ(ProtoSpace::collectorHelperThreads(), 64u);
}
