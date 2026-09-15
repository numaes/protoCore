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

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <cstdio>
#include <string>
#include <unistd.h>

using namespace proto;

namespace {

long residentKb() {
    long pages = 0, resident = 0;
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (f) {
        if (std::fscanf(f, "%ld %ld", &pages, &resident) != 2) resident = 0;
        std::fclose(f);
    }
    return resident * (sysconf(_SC_PAGESIZE) / 1024);
}

}  // namespace

// The canonical pointer is stable and no further memory is committed.
TEST(SymbolIntern, RepeatedCreateSymbolOfAnExistingNameAllocatesNothing) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;
    const std::string name = "a_rather_long_symbol";   // 20 bytes: heap-backed, not inline
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
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    const std::string longName = "another_long_symbol_name";
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

    const ProtoString* fresh = ProtoString::createSymbol(ctx, "freshly_interned_name");
    ASSERT_NE(fresh, nullptr);
    EXPECT_TRUE(SymbolTable::isSymbol(reinterpret_cast<const ProtoObject*>(fresh)));
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(ProtoString::createSymbol(ctx, "freshly_interned_name"), fresh);
    }

    // An attribute stored under the fresh symbol is found through a separately
    // created symbol with the same spelling.
    const ProtoObject* object = ctx->newObject(false)->setAttribute(ctx, fresh, ctx->fromInteger(5));
    const ProtoString* again = ProtoString::createSymbol(ctx, "freshly_interned_name");
    const ProtoObject* value = object->getAttribute(ctx, again);
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(value->asLong(ctx), 5);
}
