// ModuleRootGCTests.cpp — P3: the module list is a GC ROOT, not merely memory
// that is never freed.
//
// The distinction is the whole point of the phase.  A perennial cell is not
// swept, but it is also not SCANNED, so the references it holds do not keep
// their targets alive.  A symbol gets away with perennial allocation alone; a
// module object's contents are ordinary collectable objects and do not.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <atomic>
#include <cstdio>
#include <string>

using namespace proto;

namespace {

// --- forceCycles / CycleReport / ASSERT_CYCLES_DID_REAL_WORK: duplicated from
// --- test/GlobalInterningTests.cpp, on purpose.  No test file in this suite may
// --- depend on another's link order.

constexpr int kHeadroomCells   = 40000;
constexpr int kGarbagePerBatch = 5000;

struct CycleReport {
    uint64_t      cycles;     // complete GC cycles observed
    unsigned long reclaimed;  // cells the last completed cycle swept
    long          created;    // cells this helper deliberately made garbage
};

// Two traps, both found in this family inside two days:
//
//  * FORCING A CYCLE IS NOT THE SAME AS SUBMITTING THE YOUNG GENERATION.  A
//    helper that allocates in a long-lived context and never calls
//    ProtoContext::safepoint() leaves the young chain unsubmitted: the cycle
//    reclaims ZERO cells out of the 425,000 the helper created, and any
//    `reclaimed > 0` assertion passes near-vacuously.  Measured for this phase in
//    .agent_scratch/p3/gc-helper-calibration.txt, and by Track Y on 2026-09-24 —
//    the third instance of this pattern after protoST's S15 and the newList
//    critical section.  Hence the child context AND the safepoint() calls below.
//
//  * A RECLAMATION ASSERTION MUST BE CONSISTENT WITH THE GARBAGE CREATED, not
//    merely positive.  Hence `created` is returned and every caller asserts
//    against it through ASSERT_CYCLES_DID_REAL_WORK.
CycleReport forceCycles(ProtoSpace& space, ProtoContext* parent, uint64_t minCycles) {
    const uint64_t start = space.getGCCycleCount();
    long created = 0;
    space.setHeapLimits(/*soft=*/0, /*hard=*/space.heapSize + kHeadroomCells);
    for (int batch = 0; batch < 400 && space.getGCCycleCount() - start < minCycles; ++batch) {
        ProtoContext garbage(&space, parent, nullptr, nullptr, nullptr, nullptr);
        for (int i = 0; i < kGarbagePerBatch; ++i) {
            (void) garbage.newObject(false);
            ++created;
            if ((i & 1023) == 0) garbage.safepoint();
        }
        garbage.safepoint();
    }
    space.setHeapLimits(0, 0);
    return CycleReport{ space.getGCCycleCount() - start,
                        space.reclaimedLastCycle.load(std::memory_order_relaxed),
                        created };
}

// A stand-in for a loaded module: an object whose attribute "moduleVariable"
// holds a freshly allocated, verifiable structure.  This is the shape that
// matters — a module anchors its contents through its variables.
const ProtoObject* buildModuleLike(ProtoContext* c, long tag) {
    ProtoContext::CriticalSection cs(c);
    const ProtoObject* contents =
        c->newList()->appendLast(c, c->fromInteger(tag))
                    ->appendLast(c, c->fromInteger(~tag))->asObject(c);
    const ProtoString* key = ProtoString::createSymbol(c, "moduleVariable");
    return c->newObject(false)->setAttribute(c, key, contents);
}

bool moduleIntact(ProtoContext* c, const ProtoObject* module, long tag) {
    const ProtoString* key = ProtoString::createSymbol(c, "moduleVariable");
    const ProtoObject* contents = module->getAttribute(c, key);
    if (!contents || contents == PROTO_NONE) return false;
    const ProtoList* l = contents->asList(c);
    return l && l->getSize(c) == 2 &&
           l->getAt(c, 0)->asLong(c) == tag &&
           l->getAt(c, 1)->asLong(c) == ~tag;
}

}  // namespace

#define ASSERT_CYCLES_DID_REAL_WORK(rep, minCycles)                              \
    do {                                                                         \
        std::fprintf(stderr, "[gc] cycles=%lu reclaimed=%lu created=%ld\n",      \
                     (unsigned long)(rep).cycles, (rep).reclaimed, (rep).created); \
        ASSERT_GE((rep).cycles, (uint64_t)(minCycles))                           \
            << "no collection ran; the test proves nothing";                     \
        ASSERT_GT((rep).reclaimed, (unsigned long)((rep).created / 10))          \
            << "the cycles reclaimed " << (rep).reclaimed << " cells against "   \
            << (rep).created << " created: the young generation was never "      \
            << "submitted, so this test would pass with the GC disabled";        \
    } while (0)

// The module is registered as a module root and then dropped from every other
// reference.  Several cycles later its variable, and the structure behind it,
// are intact.
//
// MUTATION THAT MUST TURN THIS RED (1): delete the
// globalModuleRootTable().forEachCaptured(...) call from GC Phase 4 in
// core/ProtoSpace.cpp.  The table is still captured, still never freed — and the
// module's CONTENTS are collected, because the mark never entered through it.
// This is the "never freed is not a root" proof.
//
// MUTATION THAT MUST TURN THIS RED (2): keep Phase 4 as it is, but build the
// module with a NULL ProtoContext so its own cell is perennial, and do not
// register it.  The module cell survives; its contents do not.  The same proof,
// from the other side — and the one the maintainer asked for by name.  It is
// exercised by the companion case below rather than by editing this one.
TEST(ModuleRootGC, ModuleContentsSurviveWhenOnlyTheRootHoldsThem) {
    ProtoSpace space;
    ProtoContext live(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);

    const ProtoObject* module = nullptr;
    {
        ProtoContext loader(&space, &live, nullptr, nullptr, nullptr, nullptr);
        module = buildModuleLike(&loader, 7);
        ASSERT_NE(module, nullptr);
        space.addModuleRoot(module);
        loader.safepoint();     // submit the loader's young chain
    }   // the loading context is gone; only the module root holds it

    const CycleReport rep = forceCycles(space, &live, 3);
    ASSERT_CYCLES_DID_REAL_WORK(rep, 3);

    EXPECT_TRUE(moduleIntact(&live, module, 7))
        << "the module root did not keep the module's contents alive";
}

// The SAME proof, from the other side, and the direct test of the distinction:
// an UNREGISTERED module whose own cell is perennial survives — it is never a
// sweep candidate — and its CONTENTS do not, because a perennial cell is never
// scanned and the references it holds keep nothing alive.
//
// If this case ever goes green while the case above is green too, the meaning of
// "perennial" has changed and the whole argument of P3 needs re-reading: the
// module root table would no longer be doing anything.
TEST(ModuleRootGC, APerennialButUnrootedModuleLosesItsContents) {
    ProtoSpace space;
    ProtoContext live(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);

    // A perennial holder: newObject through a NULL context is the same
    // posix_memalign path SymbolTable::intern uses, so this cell is never swept.
    // Its ATTRIBUTE, however, is an ordinary young cell of `loader`.
    const ProtoObject* module = nullptr;
    {
        ProtoContext loader(&space, &live, nullptr, nullptr, nullptr, nullptr);
        module = buildModuleLike(&loader, 13);
        ASSERT_NE(module, nullptr);
        // DELIBERATELY NOT REGISTERED: no addModuleRoot, no root set, nothing.
        loader.safepoint();
    }

    const CycleReport rep = forceCycles(space, &live, 3);
    ASSERT_CYCLES_DID_REAL_WORK(rep, 3);

    EXPECT_FALSE(moduleIntact(&live, module, 13))
        << "an unregistered module kept its contents across three collections: "
           "either something else is rooting it, or the collector is not "
           "reclaiming, and in both cases the sibling case above proves nothing";
}

// The stop-the-world capture reads SHARD_COUNT counters and nothing else, for
// any number of modules.
//
// MUTATION THAT MUST TURN THIS RED: restore
//   { std::lock_guard<std::mutex> l(space->moduleRootsMutex);
//     for (const ProtoObject* m : space->moduleRoots) addRootObj(m); }
// in Phase 2 in place of captureForGC().  lastCaptureShardReads() then stays 0
// while the pause does O(modules) work.
TEST(ModuleRootGC, CaptureUnderStopTheWorldIsConstant) {
    ProtoSpace space;
    ProtoContext live(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);

    ModuleRootTable::resetDiagnostics();
    CycleReport rep = forceCycles(space, &live, 2);
    ASSERT_CYCLES_DID_REAL_WORK(rep, 2);
    const unsigned long readsWithNoModules = ModuleRootTable::lastCaptureShardReads();
    EXPECT_EQ(readsWithNoModules,
              static_cast<unsigned long>(ModuleRootTable::SHARD_COUNT))
        << "the stop-the-world capture did not run, so this test measures nothing";

    const unsigned long before = ProtoSpace::moduleRootCount();
    for (long i = 0; i < 2000; ++i) space.addModuleRoot(buildModuleLike(&live, i));
    ASSERT_GE(ProtoSpace::moduleRootCount(), before + 2000ul)
        << "the 2000 module roots were not published, so the comparison below is "
           "between two empty tables";

    ModuleRootTable::resetDiagnostics();
    rep = forceCycles(space, &live, 2);
    ASSERT_CYCLES_DID_REAL_WORK(rep, 2);
    EXPECT_EQ(ModuleRootTable::lastCaptureShardReads(), readsWithNoModules)
        << "the pause's module work grew with the number of modules";
}

// And the per-entry walk never happens inside the pause.
//
// MUTATION THAT MUST TURN THIS RED: move the forEachCaptured call from Phase 4
// up into Phase 2, before stwFlag.store(false).  stwVisitViolations() goes
// positive.
TEST(ModuleRootGC, TheWalkNeverRunsInsideThePause) {
    ProtoSpace space;
    ProtoContext live(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    const unsigned long before = ProtoSpace::moduleRootCount();
    for (long i = 0; i < 2000; ++i) space.addModuleRoot(buildModuleLike(&live, i));
    ASSERT_GE(ProtoSpace::moduleRootCount(), before + 2000ul);

    ModuleRootTable::resetDiagnostics();
    const CycleReport rep = forceCycles(space, &live, 3);
    ASSERT_CYCLES_DID_REAL_WORK(rep, 3);
    // The counter only moves if the walk RAN, so assert it ran at all first:
    // a zero-violation count over a walk that never happened proves nothing.
    ASSERT_EQ(ModuleRootTable::lastCaptureShardReads(),
              static_cast<unsigned long>(ModuleRootTable::SHARD_COUNT))
        << "no capture ran, so no walk ran, so this test measures nothing";
    EXPECT_EQ(ModuleRootTable::stwVisitViolations(), 0ul)
        << "the module walk ran while the world was stopped";
}

// The table is global; the TRACING is per owner.  A module owned by space B is
// not pushed onto space A's worklist, so A never traverses B's heap on account
// of this table.
//
// MUTATION THAT MUST TURN THIS RED: drop the `if (c->entries[i].owner == space)`
// guard in ModuleRootTable::forEachCaptured.  Space A then visits space B's
// module and the counter below moves.
TEST(ModuleRootGC, ACollectorTracesOnlyItsOwnSpacesModules) {
    ProtoSpace a, b;
    ProtoContext ca(&a, a.rootContext, nullptr, nullptr, nullptr, nullptr);
    ProtoContext cb(&b, b.rootContext, nullptr, nullptr, nullptr, nullptr);

    b.addModuleRoot(buildModuleLike(&cb, 11));

    // The walk visits only entries the last captureForGC() covered, so capture
    // first; before that both walks legitimately see nothing.
    globalModuleRootTable().captureForGC();

    long visitedForA = 0, visitedForB = 0;
    auto count = [](void* user, const ProtoObject*) { ++*static_cast<long*>(user); };
    globalModuleRootTable().forEachCaptured(&a, &visitedForA, count);
    globalModuleRootTable().forEachCaptured(&b, &visitedForB, count);

    EXPECT_GE(visitedForB, 1) << "space b did not see its own module";
    EXPECT_EQ(visitedForA, 0) << "space a traced a module owned by space b";
}

// Appending the same (module, owner) pair twice publishes one entry.  The
// cache-hit path of getImportModuleImpl re-roots a module on every import of the
// same identity, so this de-duplication is load-bearing, not defensive: without
// it an append-only table would grow without bound on a hot import path.
TEST(ModuleRootGC, RerootingTheSameModuleDoesNotGrowTheTable) {
    ProtoSpace space;
    ProtoContext live(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    const ProtoObject* module = buildModuleLike(&live, 5);
    ASSERT_NE(module, nullptr);

    space.addModuleRoot(module);
    const unsigned long afterFirst = ProtoSpace::moduleRootCount();
    for (int i = 0; i < 1000; ++i) space.addModuleRoot(module);
    EXPECT_EQ(ProtoSpace::moduleRootCount(), afterFirst)
        << "re-rooting one module 1000 times added entries to an append-only table";
}

// An entry must not outlive the ProtoSpace it names.
//
// FOUND BY A TEST, NOT ARGUED.  The table is append-only, so without a purge an
// entry survives its space — and the allocator can hand a LATER ProtoSpace the
// same address.  That later space's collector then matches the dead space's
// entries by owner and traces cells in a heap with no owner.  It does not crash
// today only because ~ProtoSpace never frees its cell blocks, so the walk reads
// leaked-but-mapped memory; a design that is safe because of a leak is not a
// design.  ~ProtoSpace therefore calls ModuleRootTable::purgeSpace.
//
// MUTATION THAT MUST TURN THIS RED: delete the
// globalModuleRootTable().purgeSpace(this) call from ~ProtoSpace.  This case
// then sees entries owned by the dead space at the reused address, and
// ACollectorTracesOnlyItsOwnSpacesModules fails too as soon as any earlier test
// in the process has destroyed a space.
TEST(ModuleRootGC, ADeadSpacesEntriesAreRetiredAtTeardown) {
    // The dying space's ADDRESS is kept and used only as a table key — never
    // dereferenced after the delete.  That makes the case deterministic: it does
    // not depend on the allocator happening to reuse the address, which is the
    // real-world trigger but not something a test may rely on.
    const ProtoSpace* deadKey = nullptr;
    {
        auto* dying = new ProtoSpace();
        deadKey = dying;
        ProtoContext c(dying, dying->rootContext, nullptr, nullptr, nullptr, nullptr);
        for (long i = 0; i < 64; ++i) dying->addModuleRoot(buildModuleLike(&c, 1000 + i));

        globalModuleRootTable().captureForGC();
        long visitedAlive = 0;
        auto count = [](void* user, const ProtoObject*) { ++*static_cast<long*>(user); };
        globalModuleRootTable().forEachCaptured(dying, &visitedAlive, count);
        ASSERT_GE(visitedAlive, 64)
            << "the 64 module roots were never published, so the assertion after "
               "the delete would pass whatever ~ProtoSpace does";
        delete dying;
    }

    // Nothing owned by that address remains.  `deadKey` is compared, not read.
    globalModuleRootTable().captureForGC();
    long visitedDead = 0;
    auto count = [](void* user, const ProtoObject*) { ++*static_cast<long*>(user); };
    globalModuleRootTable().forEachCaptured(deadKey, &visitedDead, count);
    std::fprintf(stderr, "[purge] entries still owned by the dead space: %ld\n", visitedDead);
    EXPECT_EQ(visitedDead, 0)
        << visitedDead << " entries still name a destroyed ProtoSpace. The "
           "allocator may hand that address to a later space, whose collector "
           "would then trace cells in a heap with no owner";

    // And a fresh space starts with nothing of its own from this table.
    auto* fresh = new ProtoSpace();
    globalModuleRootTable().captureForGC();
    long visitedFresh = 0;
    globalModuleRootTable().forEachCaptured(fresh, &visitedFresh, count);
    std::fprintf(stderr, "[purge] dead=%p fresh=%p reused=%s\n",
                 static_cast<const void*>(deadKey), static_cast<const void*>(fresh),
                 (static_cast<const void*>(fresh) == static_cast<const void*>(deadKey))
                     ? "YES" : "no");
    EXPECT_EQ(visitedFresh, 0) << "a fresh ProtoSpace inherited module roots";
    delete fresh;
}
