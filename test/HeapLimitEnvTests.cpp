// HeapLimitEnvTests.cpp — PROTOCORE_HEAP_LIMIT_CELLS.
//
// A ProtoSpace reads PROTOCORE_HEAP_LIMIT_CELLS once, at the end of its
// construction, and calls setHeapLimits(soft, hard):
//   <hard>         hard ceiling only (soft 0)
//   <soft>,<hard>  both
// Unset, 0 or a hard part of 0 means no limit; any other malformed value is
// ignored.  Every test restores the variable it changes.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"

#include <cstdlib>
#include <string>
#include <vector>

using namespace proto;

namespace {

constexpr const char* kVariable = "PROTOCORE_HEAP_LIMIT_CELLS";

// Sets (or, with nullptr, unsets) an environment variable for the lifetime
// of the object, then restores its previous state.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name) {
        if (const char* old = std::getenv(name)) {
            hadValue_ = true;
            oldValue_ = old;
        }
        if (value) {
            ::setenv(name, value, 1);
        } else {
            ::unsetenv(name);
        }
    }
    ~ScopedEnv() {
        if (hadValue_) {
            ::setenv(name_, oldValue_.c_str(), 1);
        } else {
            ::unsetenv(name_);
        }
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    const char* name_;
    bool hadValue_ = false;
    std::string oldValue_;
};

// Allocates garbage in short-lived child contexts, keeps `live` (a list of
// integers pinned in a root set) and checks it afterwards.
void allocateGarbageAndCheckLiveData(ProtoSpace& space, int batches, int objectsPerBatch) {
    ProtoContext* ctx = space.rootContext;
    ProtoRootSet* rs = space.createRootSet("heap-limit-env-live");
    constexpr int kLive = 100;
    ProtoRootSet::Handle liveHandle = ProtoRootSet::kNullHandle;
    {
        ProtoContext build(&space, ctx, nullptr, nullptr, nullptr, nullptr);
        const ProtoList* live = build.newList();
        for (int i = 0; i < kLive; ++i) live = live->appendLast(&build, build.fromInteger(i));
        liveHandle = rs->add(live->asObject(&build));
    }
    for (int b = 0; b < batches; ++b) {
        ProtoContext garbage(&space, ctx, nullptr, nullptr, nullptr, nullptr);
        for (int i = 0; i < objectsPerBatch; ++i) (void) garbage.newObject(false);
    }
    const ProtoList* live = rs->resolve(liveHandle)->asList(ctx);
    ASSERT_NE(live, nullptr);
    ASSERT_EQ(live->getSize(ctx), static_cast<unsigned long>(kLive));
    for (int i = 0; i < kLive; ++i) {
        EXPECT_EQ(live->getAt(ctx, i)->asLong(ctx), i) << "live element " << i;
    }
    rs->remove(liveHandle);
    space.destroyRootSet(rs);
}

}  // namespace

TEST(HeapLimitEnv, UnsetMeansUnlimited) {
    ScopedEnv env(kVariable, nullptr);
    ProtoSpace space;
    EXPECT_EQ(space.maxHeapSize, 0);
    EXPECT_EQ(space.softHeapLimit, 0);
}

TEST(HeapLimitEnv, ZeroMeansUnlimited) {
    for (const char* value : {"0", "000", "5000,0"}) {
        ScopedEnv env(kVariable, value);
        ProtoSpace space;
        EXPECT_EQ(space.maxHeapSize, 0) << "value \"" << value << "\"";
        EXPECT_EQ(space.softHeapLimit, 0) << "value \"" << value << "\"";
    }
}

TEST(HeapLimitEnv, SingleValueSetsTheHardCeilingOnly) {
    ScopedEnv env(kVariable, "750000");
    ProtoSpace space;
    EXPECT_EQ(space.maxHeapSize, 750000);
    EXPECT_EQ(space.softHeapLimit, 0);
}

TEST(HeapLimitEnv, SoftAndHardForm) {
    {
        ScopedEnv env(kVariable, "600000,900000");
        ProtoSpace space;
        EXPECT_EQ(space.softHeapLimit, 600000);
        EXPECT_EQ(space.maxHeapSize, 900000);
    }
    {
        // A soft watermark above the ceiling is clamped by setHeapLimits.
        ScopedEnv env(kVariable, "900000,600000");
        ProtoSpace space;
        EXPECT_EQ(space.softHeapLimit, 600000);
        EXPECT_EQ(space.maxHeapSize, 600000);
    }
}

TEST(HeapLimitEnv, InvalidValuesAreIgnored) {
    const std::vector<const char*> invalid = {
        "", "abc", "12abc", "-500000", "+500000", " 500000", "500000 ",
        "500000,", ",500000", "1,2,3", "5e5", "2147483648", "99999999999999999999",
    };
    for (const char* value : invalid) {
        ScopedEnv env(kVariable, value);
        ProtoSpace space;
        EXPECT_EQ(space.maxHeapSize, 0) << "value \"" << value << "\"";
        EXPECT_EQ(space.softHeapLimit, 0) << "value \"" << value << "\"";
    }
}

// With a low limit, a garbage-allocating loop runs collection cycles on its
// own (no triggerGC call), stays near the ceiling and keeps live data.
TEST(HeapLimitEnv, LowLimitRunsCyclesAndKeepsLiveData) {
    constexpr int kLimit = 1000000;
    ScopedEnv env(kVariable, "1000000");
    ProtoSpace space;
    ASSERT_EQ(space.maxHeapSize, kLimit);
    const uint64_t cyclesStart = space.getGCCycleCount();

    // 3,000,000 objects of 2 cells each: several times the ceiling.
    allocateGarbageAndCheckLiveData(space, 600, 5000);

    EXPECT_GE(space.getGCCycleCount() - cyclesStart, 2u)
        << "a low heap limit must make the collector run while garbage accumulates";
    // An allocation inside a critical section may overshoot the ceiling by at
    // most one refill batch (see ProtoSpace::setHeapLimits).
    EXPECT_LE(space.heapSize, kLimit + 65536);
}

// A limit far below the heap the bootstrap already allocated does not block
// construction and still lets a garbage-allocating loop make progress.
TEST(HeapLimitEnv, LimitBelowTheBootstrapHeapIsSane) {
    ScopedEnv env(kVariable, "1000");
    ProtoSpace space;
    ASSERT_EQ(space.maxHeapSize, 1000);
    const uint64_t cyclesStart = space.getGCCycleCount();

    // 1,000,000 objects: more than the cells left over from the bootstrap
    // allocation, so the loop must wait for cycles to reclaim garbage.
    allocateGarbageAndCheckLiveData(space, 200, 5000);

    EXPECT_GE(space.getGCCycleCount() - cyclesStart, 1u);
}
