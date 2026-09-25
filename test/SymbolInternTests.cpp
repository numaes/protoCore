// SymbolInternTests.cpp — createSymbol of an existing spelling allocates nothing.
//
// Symbols are unique and perennial: their cells are built with a null
// ProtoContext, so they come from posix_memalign and no cycle ever reclaims
// them.  createSymbol therefore has to find an existing spelling BEFORE
// building anything: it used to build the string first and then call intern,
// which built a second perennial copy in normalizeForSymbol, and both were
// dropped when the table turned out to already hold the name.  A name longer
// than INLINE_STRING_MAX_BYTES leaked about 500 bytes per call.
//
// Cells built with a null context never appear in a context's
// allocatedCellsCount nor in ProtoSpace::heapSize (they bypass both the
// per-thread freelist and the young chain), so the leak is measured as
// resident memory.
//
// P3 (2026-09-24): the symbol table is PROCESS-GLOBAL, so "is this spelling
// fresh?" no longer depends on the ProtoSpace a test builds — it depends on
// what the whole process has already interned, i.e. on test execution order and
// on --gtest_repeat.  Every spelling below is therefore made unique per test
// invocation through uniqueSpelling(); a case that relied on a literal being
// fresh would otherwise pass or fail by luck.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <unistd.h>

using namespace proto;

namespace {

// An assertion on resident memory is meaningless under a sanitizer: ASan's
// redzones and shadow map inflate every allocation, so the same 200k
// createSymbol calls that commit ~0 KB in a Release build commit tens of
// megabytes here.  The three residency cases below therefore skip under ASan
// rather than report a failure the code does not have.  (The first two cases
// pre-date P3 and were already red in an ASan build for this reason.)
#if defined(__SANITIZE_ADDRESS__)
#  define P3_RESIDENCY_IS_MEASURABLE 0
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define P3_RESIDENCY_IS_MEASURABLE 0
#  else
#    define P3_RESIDENCY_IS_MEASURABLE 1
#  endif
#else
#  define P3_RESIDENCY_IS_MEASURABLE 1
#endif

#define SKIP_IF_RESIDENCY_IS_NOT_MEASURABLE()                                 \
    do {                                                                      \
        if (!P3_RESIDENCY_IS_MEASURABLE)                                      \
            GTEST_SKIP() << "resident-memory assertions are not meaningful "  \
                            "under AddressSanitizer";                         \
    } while (0)

long residentKb() {
    long pages = 0, resident = 0;
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (f) {
        if (std::fscanf(f, "%ld %ld", &pages, &resident) != 2) resident = 0;
        std::fclose(f);
    }
    return resident * (sysconf(_SC_PAGESIZE) / 1024);
}

// A spelling no other test and no earlier repetition of this test can have
// interned.  P3 made the symbol table process-global, so freshness is a property
// of the process, not of a ProtoSpace.
std::string uniqueSpelling(const char* stem) {
    static std::atomic<unsigned long> counter{0};
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string out = "P3_";
    out += info ? info->name() : "no_test";
    out += '_';
    out += stem;
    out += '_';
    out += std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
    return out;
}

}  // namespace

// The canonical pointer is stable and no further memory is committed.
TEST(SymbolIntern, RepeatedCreateSymbolOfAnExistingNameAllocatesNothing) {
    SKIP_IF_RESIDENCY_IS_NOT_MEASURABLE();
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;
    const std::string name = uniqueSpelling("a_rather_long_symbol");  // heap-backed, not inline
    const ProtoString* first = ProtoString::createSymbol(ctx, name.c_str());
    ASSERT_NE(first, nullptr);

    // Warm up: the first calls may touch pages already reserved by the space.
    for (int i = 0; i < 1000; ++i) (void) ProtoString::createSymbol(ctx, name.c_str());

    const long rssBefore = residentKb();
    const unsigned long cellsBefore = ctx->allocatedCellsCount;
    const int heapBefore = space.heapSize;

    constexpr int kCalls = 200000;
    int mismatches = 0;
    for (int i = 0; i < kCalls; ++i) {
        if (ProtoString::createSymbol(ctx, name.c_str()) != first) ++mismatches;
    }

    EXPECT_EQ(mismatches, 0) << "createSymbol must return the canonical symbol every time";
    EXPECT_EQ(ctx->allocatedCellsCount, cellsBefore);
    EXPECT_EQ(space.heapSize, heapBefore);
    // Before the fix this grew by about 500 bytes per call (~100 MB here).
    EXPECT_LE(residentKb() - rssBefore, 4096)
        << "createSymbol of an existing name must not commit memory";
}

// The std::string overload and a name that ends up as an inline string behave
// the same way.
TEST(SymbolIntern, OverloadsAndShortNamesAreStableAndAllocationFree) {
    SKIP_IF_RESIDENCY_IS_NOT_MEASURABLE();
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    const std::string longName = uniqueSpelling("another_long_symbol_name");
    const ProtoString* fromChars = ProtoString::createSymbol(ctx, longName.c_str());
    const ProtoString* fromString = ProtoString::createSymbol(ctx, longName);
    EXPECT_EQ(fromChars, fromString) << "both overloads must return the same symbol";

    const ProtoString* shortA = ProtoString::createSymbol(ctx, "ab");
    const ProtoString* shortB = ProtoString::createSymbol(ctx, "ab");
    EXPECT_EQ(shortA, shortB) << "short names are inline strings and already pointer-stable";

    const long rssBefore = residentKb();
    for (int i = 0; i < 100000; ++i) {
        (void) ProtoString::createSymbol(ctx, longName);
        (void) ProtoString::createSymbol(ctx, "ab");
    }
    EXPECT_LE(residentKb() - rssBefore, 4096);
}

// A spelling that is not interned yet is still interned once, and stays
// canonical afterwards.
TEST(SymbolIntern, AFreshNameIsInternedOnceAndStaysCanonical) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    const std::string freshName = uniqueSpelling("freshly_interned_name");
    const ProtoString* fresh = ProtoString::createSymbol(ctx, freshName.c_str());
    ASSERT_NE(fresh, nullptr);
    EXPECT_TRUE(SymbolTable::isSymbol(reinterpret_cast<const ProtoObject*>(fresh)));
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(ProtoString::createSymbol(ctx, freshName.c_str()), fresh);
    }

    // An attribute stored under the fresh symbol is found through a separately
    // created symbol with the same spelling.
    const ProtoObject* object = ctx->newObject(false)->setAttribute(ctx, fresh, ctx->fromInteger(5));
    const ProtoString* again = ProtoString::createSymbol(ctx, freshName.c_str());
    const ProtoObject* value = object->getAttribute(ctx, again);
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(value->asLong(ctx), 5);
}

// P3: residency grows with distinct spellings, not with the number of spaces.  A
// host that builds a runtime per test case (protoST does) must not pay for the
// same 2000 names once per case.
//
// ProtoSpace teardown does not free its cell heap (nothing frees the blocks from
// core/ProtoSpace.cpp's only cell-block allocation), so every space construction
// costs tens of megabytes of resident memory whatever this test does.  A flat
// "grew less than 1 MB" bound would therefore measure the space, not the
// interning.  The test measures the SPACE FLOOR first — five spaces that intern
// nothing — and then asserts that five spaces which each intern the same 2000
// names cost no more than that floor plus a small allowance.  The allowance is
// what re-interning would blow through: 5 x 2000 names x ~500 bytes is ~5 MB.
//
// MUTATION THAT MUST TURN THIS RED: revert ProtoSpace's constructor to
// `symbolTable = new SymbolTable();`.
TEST(SymbolIntern, ResidencyDoesNotGrowWithSpaceCount) {
    SKIP_IF_RESIDENCY_IS_NOT_MEASURABLE();
    // One shared spelling set for this test invocation: the point is that the
    // second through sixth space intern nothing, so the set must be the same set.
    const std::string stem = uniqueSpelling("residency");
    auto internAll = [&stem](ProtoSpace& s) {
        ProtoContext c(&s, s.rootContext, nullptr, nullptr, nullptr, nullptr);
        for (int i = 0; i < 2000; ++i)
            (void) ProtoString::createSymbol(&c, (stem + std::to_string(i)).c_str());
    };

    { ProtoSpace warm; internAll(warm); }         // populate the global table

    // The floor: five spaces that intern nothing.
    const long floorBefore = residentKb();
    for (int rep = 0; rep < 5; ++rep) { ProtoSpace s; (void)s; }
    const long spaceFloorKb = residentKb() - floorBefore;

    // The measurement: five spaces that each intern all 2000 names.
    const long before = residentKb();
    for (int rep = 0; rep < 5; ++rep) { ProtoSpace s; internAll(s); }
    const long withInterningKb = residentKb() - before;

    const long attributable = withInterningKb - spaceFloorKb;
    std::fprintf(stderr,
                 "[residency] 5 empty spaces: %ld KB; 5 spaces x 2000 names: %ld KB; "
                 "attributable to interning: %ld KB\n",
                 spaceFloorKb, withInterningKb, attributable);
    ASSERT_GT(spaceFloorKb, 0L)
        << "the space floor measured nothing, so the comparison is meaningless";
    // Re-interning 5 x 2000 names would add roughly 5 MB on top of the floor.
    EXPECT_LT(attributable, 2048L)
        << "interning cost " << attributable << " KB beyond the five spaces\' own "
        << "heaps: names are being re-interned per space";
}
