// GlobalInterningTests.cpp — P3: interning is process-global.
//
// Phase P3 makes ProtoString::createSymbol return one canonical pointer per
// spelling PER PROCESS, not per ProtoSpace.  Before P3 the guarantee was
// half-global with silent partial success: protoCore embeds a short ASCII
// string in the pointer word (INLINE_STRING_MAX_BYTES == 6), so a 5-byte name
// matched across spaces BY ACCIDENT while a 7-byte name missed with no error at
// all — getAttribute simply returned PROTO_NONE, which is also a value.
//
// Measured 2026-09-24 during protoST Track Y; the trap is recorded verbatim in
// protoST/src/modules/STModuleProvider.cpp.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

using namespace proto;

namespace {

// A space plus a context in it, parented to its root context.
struct Space {
    ProtoSpace   space;
    ProtoContext ctx{&space, space.rootContext, nullptr, nullptr, nullptr, nullptr};
};

constexpr int kHeadroomCells   = 40000;
constexpr int kGarbagePerBatch = 5000;

struct CycleReport {
    uint64_t      cycles;     // complete GC cycles observed
    unsigned long reclaimed;  // cells the last completed cycle swept
    long          created;    // cells this helper deliberately made garbage
};

// Forces at least `minCycles` COMPLETE cycles that actually did work.
//
// Two traps, both found in this family inside two days:
//
//  * FORCING A CYCLE IS NOT THE SAME AS SUBMITTING THE YOUNG GENERATION.  A
//    helper that allocates in a LONG-LIVED context and never calls
//    ProtoContext::safepoint() leaves the young chain unsubmitted: the cycle
//    reclaims ZERO cells out of the 425,000 the helper created, and any
//    `reclaimed > 0` assertion passes vacuously.  Measured by Track Y on
//    2026-09-24 — the third instance of this pattern after protoST's S15 and the
//    newList critical section.  Hence the CHILD context AND the safepoint()
//    calls below: measured for this phase, either one alone is not enough.
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
            // Submit the young generation at a point the embedder considers
            // safe.  WITHOUT THIS THE HELPER MEASURES NOTHING.
            if ((i & 1023) == 0) garbage.safepoint();
        }
        garbage.safepoint();
    }
    space.setHeapLimits(0, 0);
    return CycleReport{ space.getGCCycleCount() - start,
                        space.reclaimedLastCycle.load(std::memory_order_relaxed),
                        created };
}

std::string readBack(ProtoContext* c, const ProtoString* s) {
    std::string out;
    s->toUTF8String(c, out);
    return out;
}

}  // namespace

// Every GC claim in P3 asserts through this, never against `reclaimed > 0`.
// It also SELF-REPORTS its three numbers, so a run that measured nothing is
// visible in the log rather than reported as a pass.
//
// The `/ 10` threshold is CALIBRATED, not chosen
// (.agent_scratch/p3/gc-helper-calibration.txt):
//   * as written                       280,000 / 425,000 = 0.66
//   * safepoint() removed, child kept  280,000 / 425,000 = 0.66  (UNCHANGED —
//     the child context's destructor already submits the chain, so deleting the
//     safepoints alone proves nothing.  Recorded rather than papered over.)
//   * the real Track Y shape: allocate into the long-lived PARENT context and
//     never safepoint                        0 / 425,000 = 0.0, and protoCore
//     reports `last cycle reclaimed 0` and exits.
// 0.1 therefore sits well below the real ratio and infinitely above the failure
// mode.  A `reclaimed > 0` assertion could not have failed in Track Y's case;
// this one cannot pass in it.
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

// The 5-byte case: inside INLINE_STRING_MAX_BYTES, so the pointer word carries
// the bytes and the two spaces agree WITHOUT any table.  This case passes both
// before and after P3, and it is in the suite to document the accident that made
// the 7-byte failure so hard to see.
TEST(GlobalInterning, ShortNameMatchedAcrossSpacesEvenBeforeP3) {
    Space a, b;
    const ProtoString* sa = ProtoString::createSymbol(&a.ctx, "value");   // 5 bytes
    const ProtoString* sb = ProtoString::createSymbol(&b.ctx, "value");
    ASSERT_NE(sa, nullptr);
    EXPECT_EQ(sa, sb) << "a name within INLINE_STRING_MAX_BYTES is embedded in "
                         "the pointer word and never reaches a symbol table";
}

// The 7-byte case: beyond INLINE_STRING_MAX_BYTES, so the name is a real
// interned cell.  BEFORE P3 THIS TEST FAILS.  That failure is the bug.
TEST(GlobalInterning, LongNameIsOnePointerAcrossSpaces) {
    Space a, b;
    const ProtoString* sa = ProtoString::createSymbol(&a.ctx, "Counter");  // 7 bytes
    const ProtoString* sb = ProtoString::createSymbol(&b.ctx, "Counter");
    ASSERT_NE(sa, nullptr);
    ASSERT_NE(sb, nullptr);
    EXPECT_EQ(sa, sb) << "interning must be process-global (P3)";
}

// The same failure as the family actually experiences it: a binding written in
// one space is unreadable from another, and the read reports ABSENCE rather than
// an error.  BEFORE P3 THIS TEST FAILS for "Counter" and would pass for "value".
TEST(GlobalInterning, AttributeWrittenInOneSpaceIsReadableFromAnother) {
    Space a, b;
    const ProtoString* keyA = ProtoString::createSymbol(&a.ctx, "Counter");
    const ProtoObject* holder =
        a.ctx.newObject(false)->setAttribute(&a.ctx, keyA, a.ctx.fromInteger(42));
    ASSERT_NE(holder, nullptr);

    const ProtoString* keyB = ProtoString::createSymbol(&b.ctx, "Counter");
    const ProtoObject* got = holder->getAttribute(&b.ctx, keyB);
    ASSERT_NE(got, nullptr);
    EXPECT_NE(got, PROTO_NONE)
        << "the read reported absence, not an error: this is the silent failure P3 closes";
    EXPECT_EQ(got->asLong(&b.ctx), 42);
}

// A symbol is PERENNIAL: its cells come from posix_memalign with a null
// ProtoContext, so no cycle can reclaim them.  Perennial allocation alone is
// sufficient here — and only here — because a symbol's references point only at
// its own perennial nodes.
//
// MUTATION THAT MUST TURN THIS RED: in SymbolTable::normalizeForSymbol and in
// ProtoString::createSymbol, pass the caller's context instead of /*ctx=*/nullptr
// to ProtoStringImplementation::fromUTF8Bytes.  The symbol's cells become young
// cells of a context that then dies, and the canonical pointer is swept.
//
// The symbol is deliberately interned in a SHORT-LIVED child context that is
// then destroyed.  That is what makes the claim load-bearing: interning it in a
// context that outlives the cycles would keep the cells alive through that
// context's young chain, and the mutation above would not be detected.
TEST(GlobalInterning, SymbolsSurviveManyCollectionCycles) {
    ProtoSpace space;
    ProtoContext live(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);

    const char* kName = "PerennialAttributeName";   // 22 bytes, well past inline
    const ProtoString* first = nullptr;
    {
        ProtoContext interner(&space, &live, nullptr, nullptr, nullptr, nullptr);
        first = ProtoString::createSymbol(&interner, kName);
        ASSERT_NE(first, nullptr);
        interner.safepoint();   // submit the interning context's young chain
    }   // the interning context is gone; only perennial allocation holds the symbol

    const CycleReport rep = forceCycles(space, &live, 3);
    ASSERT_CYCLES_DID_REAL_WORK(rep, 3);

    EXPECT_EQ(ProtoString::createSymbol(&live, kName), first)
        << "the canonical pointer changed across a GC cycle";
    EXPECT_EQ(readBack(&live, first), kName) << "the symbol's own nodes were reclaimed";
}

// The table is process-global, so a symbol interned by a space that has since
// been destroyed is still canonical and still readable.  Run under ASan.
//
// MUTATION THAT MUST TURN THIS RED: restore `delete symbolTable;` in
// ~ProtoSpace.  ASan reports a use-after-free on the bucket chain; without ASan
// the second createSymbol returns a wild pointer.
TEST(GlobalInterning, SymbolSurvivesTheSpaceThatInternedIt) {
    const char* kName = "NameInternedByADeadSpace";
    const ProtoString* fromDeadSpace = nullptr;
    {
        ProtoSpace a;
        ProtoContext ca(&a, a.rootContext, nullptr, nullptr, nullptr, nullptr);
        fromDeadSpace = ProtoString::createSymbol(&ca, kName);
        ASSERT_NE(fromDeadSpace, nullptr);
    }   // space a is gone

    ProtoSpace b;
    ProtoContext cb(&b, b.rootContext, nullptr, nullptr, nullptr, nullptr);
    EXPECT_EQ(ProtoString::createSymbol(&cb, kName), fromDeadSpace);
    EXPECT_EQ(readBack(&cb, fromDeadSpace), kName);
}

// A symbol interned by space A is read through space B's context every time B
// looks a name up (contentEqual walks the stored symbol's rope).  That read MUST
// allocate nothing, or a global table would allocate into the reading space's
// heap on behalf of another.
//
// MUTATION THAT MUST TURN THIS RED: make ProtoString::toUTF8String build an
// intermediate protoCore string (flatten the rope through strConcat before
// iterating) — the String.prototype rope-flatten anti-pattern.  heapSize and
// allocatedCellsCount then move.
TEST(GlobalInterning, CrossSpaceLookupAllocatesNothing) {
    ProtoSpace a;
    ProtoContext ca(&a, a.rootContext, nullptr, nullptr, nullptr, nullptr);
    const char* kName = "ALongNameInternedInSpaceA";
    const ProtoString* canonical = ProtoString::createSymbol(&ca, kName);
    ASSERT_NE(canonical, nullptr);

    ProtoSpace b;
    ProtoContext cb(&b, b.rootContext, nullptr, nullptr, nullptr, nullptr);
    ASSERT_EQ(ProtoString::createSymbol(&cb, kName), canonical);   // warm the path

    const int  heapBefore  = b.heapSize;
    const auto cellsBefore = cb.allocatedCellsCount;
    for (int i = 0; i < 1000; ++i)
        ASSERT_EQ(ProtoString::createSymbol(&cb, kName), canonical);

    EXPECT_EQ(b.heapSize, heapBefore) << "the reading space grew its heap";
    EXPECT_EQ(cb.allocatedCellsCount, cellsBefore)
        << "the reading context allocated cells to look up a foreign symbol";
}

// The cached literals ProtoSpace's constructor interns ("__data__" 8 bytes,
// "setAttribute" 12, "callMethod" 10) are all past INLINE_STRING_MAX_BYTES, so
// before P3 each space had its OWN pointer for them.  After P3 they are shared,
// which is the fix working: literalData is an attribute key.
//
// MUTATION THAT MUST TURN THIS RED: revert ProtoSpace's constructor to
// `symbolTable = new SymbolTable();`.
TEST(GlobalInterning, CachedLiteralsAreSharedAcrossSpaces) {
    ProtoSpace a, b;
    EXPECT_EQ(a.literalData,         b.literalData);
    EXPECT_EQ(a.literalSetAttribute, b.literalSetAttribute);
    EXPECT_EQ(a.literalCallMethod,   b.literalCallMethod);
}

// A space constructed after another has interned N names re-interns none of
// them: the table grows with distinct SPELLINGS, not with space count.  The
// spellings carry the test's own name so the case is independent of execution
// order and of --gtest_repeat (P3 D8).
//
// globalSymbolCount() is process-wide, so neither this test nor any other may
// assert an absolute count — only that it does not change.
// The pointer-identity half of the assertion is load-bearing.  A per-space table
// would leave globalSymbolCount() unchanged too — because the per-space tables
// would no longer populate the global one at all — so the count alone passes
// vacuously under exactly the mutation this test exists to catch.
TEST(GlobalInterning, ASecondSpaceInternsNothingItsPredecessorAlreadyDid) {
    const std::string prefix = "P3SecondSpaceProbe";
    const ProtoString* fromA[500] = {};
    ProtoSpace a;
    {
        ProtoContext ca(&a, a.rootContext, nullptr, nullptr, nullptr, nullptr);
        for (int i = 0; i < 500; ++i) {
            fromA[i] = ProtoString::createSymbol(&ca, (prefix + std::to_string(i)).c_str());
            ASSERT_NE(fromA[i], nullptr);
        }
    }
    const unsigned long afterA = globalSymbolCount();

    ProtoSpace b;
    ProtoContext cb(&b, b.rootContext, nullptr, nullptr, nullptr, nullptr);
    for (int i = 0; i < 500; ++i) {
        const ProtoString* s =
            ProtoString::createSymbol(&cb, (prefix + std::to_string(i)).c_str());
        ASSERT_NE(s, nullptr);
        ASSERT_EQ(s, fromA[i])
            << "spelling " << i << " got a second canonical pointer in the second space";
    }

    EXPECT_EQ(globalSymbolCount(), afterA)
        << "the second space re-interned names the first had already interned";
}

// P3 D5: the tuple interner is PER SPACE.  Two spaces building a tuple of the
// same two globally-interned symbols get two distinct tuple nodes, each in its
// own heap, each internally consistent.
//
// This test exists to stop a future reader from "completing" the ruling "all
// interning should be global".  The deviation and its safety argument are in
// protoScala/docs/platform/GLOBAL-INTERNING-SPEC.md section 2.4 and D5.
//
// MUTATION THAT MUST TURN THIS RED: replace `context->space->tupleInterner` in
// ProtoTupleImplementation's interning path (core/ProtoTuple.cpp) with a
// process-global singleton.  The two addresses then coincide, which is exactly
// the cross-space coupling D5 declines.
TEST(GlobalInterning, TupleInternerStaysPerSpace) {
    ProtoSpace a, b;
    ProtoContext ca(&a, a.rootContext, nullptr, nullptr, nullptr, nullptr);
    ProtoContext cb(&b, b.rootContext, nullptr, nullptr, nullptr, nullptr);

    // Two names past INLINE_STRING_MAX_BYTES, so they are real interned cells
    // and — after P3 — the same pointers in both spaces.
    const ProtoString* n1a = ProtoString::createSymbol(&ca, "TupleElementOne");
    const ProtoString* n2a = ProtoString::createSymbol(&ca, "TupleElementTwo");
    const ProtoString* n1b = ProtoString::createSymbol(&cb, "TupleElementOne");
    const ProtoString* n2b = ProtoString::createSymbol(&cb, "TupleElementTwo");
    ASSERT_EQ(n1a, n1b);
    ASSERT_EQ(n2a, n2b);   // the premise: identical slot pointers

    // test/test_tuple.cpp's own two-element builder: a ProtoList through
    // newTupleFromList.  Not an invented one.
    auto buildPair = [](ProtoContext* c, const ProtoString* x, const ProtoString* y) {
        return c->newTupleFromList(
            c->newList()->appendLast(c, reinterpret_cast<const ProtoObject*>(x))
                        ->appendLast(c, reinterpret_cast<const ProtoObject*>(y)));
    };
    const ProtoTuple* ta  = buildPair(&ca, n1a, n2a);
    const ProtoTuple* tb  = buildPair(&cb, n1b, n2b);
    const ProtoTuple* ta2 = buildPair(&ca, n1a, n2a);
    ASSERT_NE(ta, nullptr);
    ASSERT_NE(tb, nullptr);
    ASSERT_NE(ta2, nullptr);

    EXPECT_NE(ta, tb)  << "the tuple interner has been made global; see P3 D5";
    // Load-bearing: a mutation that broke per-space interning entirely would
    // otherwise pass the assertion above.
    EXPECT_EQ(ta, ta2) << "per-space tuple interning stopped working";
}

// --- Concurrency: the global table is now contended ACROSS spaces -------------

namespace {

constexpr int kP3Spellings = 200;
constexpr int kP3Threads   = 4;      // two per space

// All 200 spellings, built once, every one past INLINE_STRING_MAX_BYTES so every
// one is a real interned cell rather than an inline string.
std::vector<std::string>            gP3Spellings;
std::vector<const ProtoString*>     gP3Seen[kP3Threads];
std::atomic<unsigned long>          gP3Nulls{0};

// Runs on a REGISTERED protoCore thread (ProtoSpace::newThread), not a raw
// std::thread: an unregistered thread is not counted in runningThreads, never
// parks at a stop-the-world, and would make this test weaker rather than
// stronger (P2 D10).
const ProtoObject* p3InternWorkerMain(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                                       const ProtoList* args, const ProtoSparseList*) {
    const long slot = args->getAt(ctx, 0)->asLong(ctx);
    // A rotated order per thread, so the threads race on the same shard from
    // different directions rather than marching in lockstep.
    for (int k = 0; k < kP3Spellings; ++k) {
        const int i = (k + static_cast<int>(slot) * 37) % kP3Spellings;
        const ProtoString* s = ProtoString::createSymbol(ctx, gP3Spellings[i].c_str());
        if (!s) gP3Nulls.fetch_add(1, std::memory_order_relaxed);
        gP3Seen[slot][i] = s;
    }
    return PROTO_NONE;
}

}  // namespace

// Four registered protoCore threads — two per space — intern 200 overlapping
// spellings concurrently; every thread must agree on the canonical pointer for
// every name, and so must the main thread.
//
// MUTATION THAT MUST TURN THIS RED: drop the double-checked re-check inside the
// shard lock in SymbolTable::intern, so two threads that normalise the same
// spelling concurrently both insert.  Two canonical pointers for one name.
// Repeated, because the failure mode is a race: a single round let the mutation
// through in one of three observed runs.  Five rounds with fresh spellings each
// time makes detection near-certain while keeping the case under 150 ms.
TEST(GlobalInterning, ConcurrentInterningAcrossSpacesAgreesOnOnePointer) {
  int totalDisagreements = 0;
  for (int round = 0; round < 5; ++round) {
    // Spellings carry this test's own name so the case is independent of
    // execution order and of --gtest_repeat (P3 D8).
    gP3Spellings.clear();
    static std::atomic<unsigned long> run{0};
    const std::string stem =
        "P3ConcurrentInterningProbe_" + std::to_string(run.fetch_add(1)) + "_";
    for (int i = 0; i < kP3Spellings; ++i)
        gP3Spellings.push_back(stem + std::to_string(i));
    for (int t = 0; t < kP3Threads; ++t)
        gP3Seen[t].assign(kP3Spellings, nullptr);
    gP3Nulls.store(0);

    ProtoSpace a, b;
    ProtoContext* rootA = a.rootContext;
    ProtoContext* rootB = b.rootContext;

    std::vector<const ProtoThread*> workers;
    for (int t = 0; t < kP3Threads; ++t) {
        ProtoContext* root = (t % 2 == 0) ? rootA : rootB;
        const ProtoList* args = root->newList()->appendLast(root, root->fromInteger(t));
        workers.push_back(root->space->newThread(
            root, ProtoString::createSymbol(root, "p3-intern-worker"),
            p3InternWorkerMain, args, nullptr));
        ASSERT_NE(workers.back(), nullptr);
    }
    {
        ProtoContext::UnmanagedScope parked(rootA);
        for (const ProtoThread* w : workers) const_cast<ProtoThread*>(w)->join(rootA);
    }

    EXPECT_EQ(gP3Nulls.load(), 0ul) << "createSymbol returned null under contention";

    // A fifth set, interned on the main thread through a third context.
    ProtoContext main(&a, rootA, nullptr, nullptr, nullptr, nullptr);
    int disagreements = 0;
    for (int i = 0; i < kP3Spellings; ++i) {
        const ProtoString* canonical = ProtoString::createSymbol(&main, gP3Spellings[i].c_str());
        ASSERT_NE(canonical, nullptr);
        for (int t = 0; t < kP3Threads; ++t) {
            if (gP3Seen[t][i] != canonical) {
                if (++disagreements <= 5)
                    std::fprintf(stderr,
                                 "[intern] spelling %d: thread %d saw %p, main saw %p\n",
                                 i, t, (const void*)gP3Seen[t][i], (const void*)canonical);
            }
        }
    }
    totalDisagreements += disagreements;
  }
  EXPECT_EQ(totalDisagreements, 0)
      << totalDisagreements << " of " << (5 * kP3Spellings * kP3Threads)
      << " (spelling, thread) pairs disagreed about the canonical pointer";
}
