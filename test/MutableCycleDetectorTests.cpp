// MutableCycleDetectorTests.cpp -- ProtoSpace::findMutableCycles, and the
// property it detects.
//
// Two halves, and the second is the one that makes the first believable:
//
//  1. THE MUTATION MATRIX.  The detector must FIRE on a cycle and go SILENT
//     when the same code stores a snapshot instead of the handle.  Both
//     directions are asserted, because a detector with false positives is
//     worse than none: it gets switched off, and then the real findings go with
//     it.  The mutation is one line -- `captureTheHandle` -- so the two
//     assertions differ in exactly the thing under test.
//
//  2. THE PROPERTY ITSELF, measured on table entries rather than asserted from
//     the documentation.  A cycle among mutables is never collected: its
//     entries survive every cycle, unchanged, for ever.  An ACYCLIC mutable
//     graph drains.  Both are measured here against the count the workload
//     created, never against `> 0`.
//
// See docs/MemoryModel.md section 7 for the property and the rule, and
// docs/GarbageCollector.md Phase 2 / Phase 5b for the two collector sites that
// combine to produce it.

#include <gtest/gtest.h>

#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace proto;

namespace {

/// Drive collection cycles to convergence, cooperating with stop-the-world.
/// `triggerGC()` is advisory (it does nothing unless free cells are below 20%),
/// so a test that allocates a bounded amount must set `gcStarted` directly --
/// and it MUST call `safepoint()` while waiting, or the collector blocks for
/// ever on a quorum this thread is withholding.
void runCycles(ProtoSpace& space, int cycles)
{
    ProtoContext* ctx = space.rootContext;
    for (int i = 0; i < cycles; ++i) {
        {
            std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
            space.gcStarted = true;
            space.gcCV.notify_all();
        }
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (space.gcStarted.load()
               && std::chrono::steady_clock::now() < deadline) {
            ctx->safepoint();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    ctx->safepoint();
}

unsigned long refOf(const ProtoObject* o)
{
    if (!proto::isObjectFast(o)) return 0;
    return toImpl<const ProtoObjectCell>(o)->mutable_ref;
}

/// The shape found in protoScala: a captured `var`.
///
/// `var f: () => Unit = null; f = () => f()` compiles a captured local to
/// `MAKE_CELL`, which is a protoCore MUTABLE (it has to be -- sharing a `var`
/// between closures is exactly what it is for), and then stores the closure
/// into it.  The closure captures the cell.  Value -> handle, and the cycle is
/// closed.
///
/// @param captureTheHandle  true reproduces the cycle.  false stores the cell's
///        CURRENT VALUE -- an immutable snapshot -- which is the documented
///        remedy for an INCIDENTAL back-reference, and is the mutation that must
///        make the detector go silent.
struct CapturedVar
{
    unsigned long cellRef    = 0;
    unsigned long closureRef = 0;
};

CapturedVar buildCapturedVar(ProtoContext* ctx, bool captureTheHandle)
{
    const ProtoString* kValue   = ProtoString::createSymbol(ctx, "cellValue");
    const ProtoString* kCapture = ProtoString::createSymbol(ctx, "capturedCell");

    const ProtoObject* cell    = ctx->newObject(/*mutableObject=*/true);
    const ProtoObject* closure = ctx->newObject(/*mutableObject=*/true);

    // The closure refers to the cell it captured...
    const ProtoObject* captured =
        captureTheHandle ? cell
                         // ... or to the cell's current value, which is a plain
                         // immutable object and closes nothing.
                         : cell->clone(ctx, /*isMutable=*/false);
    closure->setAttribute(ctx, kCapture, captured);

    // ... and the cell's current value is the closure.
    cell->setAttribute(ctx, kValue, closure);

    CapturedVar out;
    out.cellRef    = refOf(cell);
    out.closureRef = refOf(closure);
    return out;
}

/// Somewhere to hang the transients off, so they are reachable while the scan
/// runs without each one needing a root of its own.
const ProtoObject* anchor(ProtoContext* ctx, ProtoSpace& space,
                          const char* name, const ProtoObject* value)
{
    const ProtoString* key = ProtoString::createSymbol(ctx, name);
    return space.rootObject->setAttribute(ctx, key, value);
}

bool cycleCovers(const MutableGraphReport& r, unsigned long a, unsigned long b)
{
    for (const MutableCycle& c : r.cycles) {
        bool hasA = false, hasB = false;
        for (unsigned long x : c.refs) { hasA |= (x == a); hasB |= (x == b); }
        if (hasA && hasB) return true;
    }
    return false;
}

}  // namespace

//===========================================================================
// 1. The mutation matrix.
//===========================================================================

// THE DETECTOR FIRES.  protoScala's captured `var`, reproduced through the
// kernel API alone.
TEST(MutableCycleDetector, CapturedVarCycleIsReportedWithItsPath)
{
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    const CapturedVar v = buildCapturedVar(ctx, /*captureTheHandle=*/true);
    ASSERT_GT(v.cellRef, 0u);
    ASSERT_GT(v.closureRef, 0u);
    anchor(ctx, space, "capturedVarCycleAnchor", nullptr);

    const MutableGraphReport r = space.findMutableCycles(ctx);

    EXPECT_FALSE(r.truncated) << r.summary();
    EXPECT_GE(r.handles, 2u) << r.summary();
    ASSERT_FALSE(r.cycles.empty())
        << "the detector did not report a cycle between a mutable cell and the "
           "closure that captured it, which is the shape a captured `var` "
           "compiles to.  " << r.summary();
    EXPECT_TRUE(cycleCovers(r, v.cellRef, v.closureRef))
        << "a cycle was reported, but not the one between handle #"
        << v.cellRef << " (the cell) and handle #" << v.closureRef
        << " (the closure).  " << r.summary();

    // A cycle with no path is not actionable: the report must name the
    // attributes the owner has to go and change.
    bool named = false;
    for (const MutableCycle& c : r.cycles) {
        if (c.path.find("cellValue") != std::string::npos
            && c.path.find("capturedCell") != std::string::npos) named = true;
    }
    EXPECT_TRUE(named)
        << "the cycle was reported without naming the two attributes that "
           "close it, so the finding cannot be acted on.  " << r.summary();
}

// THE DETECTOR GOES SILENT.  The same code, one line different: the closure
// captures the cell's CURRENT VALUE instead of the cell.
TEST(MutableCycleDetector, SnapshotInsteadOfHandleGoesSilent)
{
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    const CapturedVar v = buildCapturedVar(ctx, /*captureTheHandle=*/false);
    ASSERT_GT(v.cellRef, 0u);
    ASSERT_GT(v.closureRef, 0u);

    const MutableGraphReport r = space.findMutableCycles(ctx);

    EXPECT_FALSE(r.truncated) << r.summary();
    EXPECT_TRUE(r.cycles.empty())
        << "the detector reported a cycle for a graph that has none.  A "
           "detector with false positives is worse than no detector, because "
           "it will be switched off.  " << r.summary();
    // And it is silent because the graph is acyclic, NOT because the scan saw
    // nothing: the cell -> closure edge is still there and still counted.
    EXPECT_GE(r.handleReferences, 1u)
        << "the scan found no reference to a mutable handle at all, so its "
           "silence "
           "says nothing about cycles.  " << r.summary();
}

// A single handle that refers to itself.  The smallest possible violation, and
// the one an SCC-size-2 rule would miss.
TEST(MutableCycleDetector, SelfReferenceIsASingleHandleCycle)
{
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    const ProtoObject* self = ctx->newObject(true);
    const unsigned long ref = refOf(self);
    self->setAttribute(ctx, ProtoString::createSymbol(ctx, "itself"), self);

    const MutableGraphReport r = space.findMutableCycles(ctx);
    ASSERT_EQ(r.cycles.size(), 1u) << r.summary();
    ASSERT_EQ(r.cycles[0].refs.size(), 1u) << r.summary();
    EXPECT_EQ(r.cycles[0].refs[0], ref) << r.summary();
    EXPECT_NE(r.cycles[0].path.find("itself"), std::string::npos) << r.summary();
}

// AN ACYCLIC HANDLE-TO-HANDLE EDGE IS NOT A VIOLATION and must not be
// reported.  `H1 -> H2` with no cycle means H2's liveness merely follows H1's,
// and Phase 5b releases H2's entry a cycle after H1 dies.  Reporting these
// would bury the real finding under one line per object in the program.
TEST(MutableCycleDetector, AcyclicEdgesBetweenMutablesAreNotReported)
{
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    // A mutable "class" and mutable "instances" that are its children: the
    // shape protoScala's class prototypes and protoST's classes both have.
    // Every instance handle references the class handle through the parent
    // chain it was BORN with -- an edge the scan must see and must not report.
    const ProtoObject* klass = ctx->newObject(true);
    klass->setAttribute(ctx, ProtoString::createSymbol(ctx, "className"),
                        ProtoString::createSymbol(ctx, "Point")->asObject(ctx));
    const ProtoString* kOwner = ProtoString::createSymbol(ctx, "owner");
    std::vector<const ProtoObject*> instances;
    for (int i = 0; i < 8; ++i) {
        const ProtoObject* inst = klass->newChild(ctx, /*isMutable=*/true);
        // And an explicit back-reference through the state as well, so the
        // edge exists by both routes.
        inst->setAttribute(ctx, kOwner, klass);
        instances.push_back(inst);
    }

    const MutableGraphReport r = space.findMutableCycles(ctx);
    EXPECT_FALSE(r.truncated) << r.summary();
    EXPECT_TRUE(r.cycles.empty())
        << "acyclic instance -> class edges were reported as cycles.  "
        << r.summary();
    EXPECT_GE(r.handleReferences, 8u)
        << "the scan did not see the 8 instance -> class edges, so its silence "
           "is not evidence of anything.  " << r.summary();
}

//===========================================================================
// 2. The property, measured.
//===========================================================================

namespace {

unsigned long tableEntries(ProtoSpace& space)
{
    return space.findMutableCycles(space.rootContext).handles;
}

}  // namespace

// AN ACYCLIC MUTABLE DRAINS.  table -> V, V does not reach H, nobody else holds
// H: H is unreachable, swept, finalized, and Phase 5b releases its entry
// (core/ProtoSpace.cpp:992-1000).
//
// The verdict is stated against the number of entries the workload created, not
// against `> 0`.  The tolerance is the documented lag of the release: a handle
// dropped after a cycle's stop-the-world belongs to the next cycle, so the last
// batch always trails.
TEST(MutableCycleDetector, AcyclicMutablesAreReleasedFromTheTable)
{
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    runCycles(space, 2);
    const unsigned long base = tableEntries(space);

    constexpr unsigned long kCount = 400;
    {
        // A child context so the handles have no root once it dies.
        ProtoContext child(&space, ctx);
        const ProtoString* k = ProtoString::createSymbol(&child, "payload");
        for (unsigned long i = 0; i < kCount; ++i) {
            const ProtoObject* m = child.newObject(true);
            m->setAttribute(&child, k, child.fromLong(static_cast<long>(i)));
        }
    }
    const unsigned long peak = tableEntries(space);
    ASSERT_GE(peak, base + kCount)
        << "the workload did not create " << kCount << " table entries (base "
        << base << ", peak " << peak << "), so there is nothing to measure";

    runCycles(space, 8);
    const unsigned long end = tableEntries(space);

    const unsigned long created  = peak - base;
    const unsigned long residual = (end > base) ? (end - base) : 0;
    // At least 90% of what the workload created must be gone.  A run that
    // released a handful would satisfy `< created` and mean nothing.
    EXPECT_LE(residual, created / 10)
        << "entries created " << created << ", still held " << residual
        << " after 8 cycles (base " << base << ", peak " << peak << ", end "
        << end << ").  An acyclic mutable must be released by Phase 5b";
}

// A CYCLE AMONG MUTABLES IS NEVER COLLECTED, and there is no monotone
// progress: the cycle after is bit-identical, so running more cycles does not
// help.  Both halves are measured -- the count, and its invariance.
TEST(MutableCycleDetector, CyclicMutablesAreNeverReleasedAndDoNotDecay)
{
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    runCycles(space, 2);
    const unsigned long base = tableEntries(space);

    constexpr unsigned long kPairs = 200;
    {
        ProtoContext child(&space, ctx);
        for (unsigned long i = 0; i < kPairs; ++i)
            buildCapturedVar(&child, /*captureTheHandle=*/true);
    }
    const unsigned long peak = tableEntries(space);
    ASSERT_GE(peak, base + 2 * kPairs)
        << "the workload did not create " << (2 * kPairs) << " table entries "
        << "(base " << base << ", peak " << peak << ")";

    runCycles(space, 8);
    const unsigned long afterFirst = tableEntries(space);
    runCycles(space, 8);
    const unsigned long afterSecond = tableEntries(space);

    EXPECT_GE(afterFirst, base + 2 * kPairs)
        << "cyclic mutables were released, which contradicts the documented "
           "property (base " << base << ", peak " << peak << ", after 8 cycles "
        << afterFirst << ").  If this is now false, docs/MemoryModel.md "
           "section 7 and the whole of conformance rule 13 need rewriting";
    EXPECT_EQ(afterSecond, afterFirst)
        << "the retained set changed between two rounds of 8 cycles ("
        << afterFirst << " -> " << afterSecond << ").  The property claims "
           "there is NO monotone progress: a later collection does not fix it";

    // And the detector accounts for every one of them, by handle.
    const MutableGraphReport r = space.findMutableCycles(ctx);
    EXPECT_EQ(r.cycles.size(), kPairs)
        << "expected one cycle per captured-var pair.  " << r.summary();
}

// AN ENTRY EXISTS ONLY ONCE THE MUTABLE HAS BEEN WRITTEN, and the scan
// therefore measures written mutables, not created ones.
//
// This is not a limitation, it is the mechanism: `newObject(true)` allocates a
// handle with a fresh `mutable_ref` and publishes NOTHING
// (core/ProtoContext.cpp:863-875); the first `setAttribute` is what CASes an
// entry into the shard (core/ProtoObject.cpp:1091-1099).  So a never-written
// mutable originates no marking and cannot be in a cycle -- which is also why a
// bare `ProtoSpace` reports zero handles even though `objectPrototype` is
// created mutable (core/ProtoSpace.cpp:1228): protoCore's bootstrap never
// writes to it.  The embedder's first `setAttribute` on it is what puts it in
// the table.
TEST(MutableCycleDetector, AnEntryAppearsOnTheFirstWriteNotAtCreation)
{
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    const MutableGraphReport bare = space.findMutableCycles(ctx);
    EXPECT_EQ(bare.handles, 0u)
        << "a bare space has a written mutable, which contradicts the "
           "documented bootstrap.  " << bare.summary();
    EXPECT_TRUE(bare.cycles.empty())
        << "protoCore's own bootstrap contains a mutable cycle, so every "
           "runtime built on it starts with one.  " << bare.summary();

    const ProtoObject* m = ctx->newObject(true);
    EXPECT_EQ(space.findMutableCycles(ctx).handles, 0u)
        << "creating a mutable published a table entry before any write";

    m->setAttribute(ctx, ProtoString::createSymbol(ctx, "written"), PROTO_TRUE);
    const MutableGraphReport after = space.findMutableCycles(ctx);
    EXPECT_EQ(after.handles, 1u)
        << "the first write did not publish a table entry.  " << after.summary();
    EXPECT_TRUE(after.cycles.empty()) << after.summary();
}

// The report always carries its numbers, on a clean result as much as on a
// finding.  A report whose counters are all zero is a broken scan that looks
// like a clean graph, and that confusion is the reason this family of checks
// exists.
TEST(MutableCycleDetector, SummaryAlwaysCarriesItsNumbers)
{
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;
    buildCapturedVar(ctx, /*captureTheHandle=*/false);

    const MutableGraphReport r = space.findMutableCycles(ctx);
    const std::string s = r.summary();
    EXPECT_NE(s.find("handles"), std::string::npos) << s;
    EXPECT_NE(s.find("cells walked"), std::string::npos) << s;
    EXPECT_NE(s.find("complete"), std::string::npos) << s;
    EXPECT_GT(r.cellsVisited, 0u) << s;
    EXPECT_GT(r.handles, 0u) << s;
}

// A truncated scan must say so, and must not be read as a clean bill of health.
TEST(MutableCycleDetector, ExhaustedBudgetIsReportedAsTruncated)
{
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;
    buildCapturedVar(ctx, /*captureTheHandle=*/true);

    const MutableGraphReport r = space.findMutableCycles(ctx, /*cellBudget=*/1);
    EXPECT_TRUE(r.truncated)
        << "a one-cell budget did not truncate the scan.  " << r.summary();
    EXPECT_FALSE(r.acyclicAndComplete())
        << "a truncated scan reported itself as an established absence of "
           "cycles.  " << r.summary();
    EXPECT_NE(r.summary().find("TRUNCATED"), std::string::npos) << r.summary();
}
