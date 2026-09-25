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

#include <cstdio>
#include <string>

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
//    helper that allocates in a child context and drops it WITHOUT calling
//    ProtoContext::safepoint() leaves the young chain unsubmitted: the cycle
//    reclaims single-digit cells instead of the ~205,000 the helper created, and
//    any `reclaimed > 0` assertion passes near-vacuously.  Measured by Track Y
//    on 2026-09-24 — the third instance of this pattern after protoST's S15 and
//    the newList critical section.  Hence the safepoint() calls below.
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
// The `/ 10` threshold is calibrated: the measured reclaimed/created ratio of
// this helper is ~0.9 (see .agent_scratch/p3/gc-helper-calibration.txt), while
// the same helper with both safepoint() calls removed reclaims single-digit
// cells out of >200,000 — a ratio of ~3e-5.  0.1 sits two orders of magnitude
// above the failure mode and an order of magnitude below the real one.
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
