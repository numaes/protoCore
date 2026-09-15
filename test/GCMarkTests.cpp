// GCMarkTests.cpp — null references on the collector's work list.
//
// Every reference the collector traces goes through one work list: roots
// pushed during the stop-the-world phase and the children that
// processReferences reports during mark.  A null entry on that list must
// never be dereferenced.  Two sources are covered:
//
//   * a root whose tagged value has no pointer bits (a "tagged null":
//     ProtoObject::isCellPointer accepts it, asCellPointer maps it to
//     nullptr), which the work list must skip;
//   * a Cell whose processReferences reports nullptr.  Instrumented and
//     debug builds abort and name the reporting cell's type, so the
//     offending implementation is identified instead of masked; other
//     builds skip the entry.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>

using namespace proto;

namespace {

// Request one collection cycle and park the calling thread at safepoints
// until the cycle has stopped the world and completed.  Returns false when
// no cycle completed within the timeout.
bool runOneCycle(ProtoSpace& space, ProtoContext* ctx) {
    const uint64_t start = space.getGCCycleCount();
    {
        std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
        space.gcStarted = true;
        space.gcCV.notify_all();
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        ctx->safepoint();
        if (space.getGCCycleCount() > start && !space.gcStarted.load()) return true;
        std::this_thread::yield();
    }
    return false;
}

// Test-only cell whose processReferences reports a null child.
class NullReportingCell final : public Cell {
public:
    explicit NullReportingCell(ProtoContext* context) : Cell(context) {}
    void processReferences(ProtoContext* context, void* self,
                           void (*method)(ProtoContext*, void*, const Cell*)) const override {
        method(context, self, nullptr);
    }
    const ProtoObject* implAsObject(ProtoContext*) const override { return PROTO_NONE; }
};

// Keeps a NullReportingCell in the young generation of a live context and
// runs collection cycles, so the Phase-2 young-generation scan reports its
// null child.
void collectWithNullReportingCell() {
    ProtoSpace space;
    ProtoContext sub(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    (void) new (&sub) NullReportingCell(&sub);
    for (int i = 0; i < 3; ++i) {
        if (!runOneCycle(space, &sub)) {
            std::fprintf(stderr, "no collection cycle completed\n");
            std::abort();
        }
    }
}

}  // namespace

// A root whose value is a tagged null reaches the work list as nullptr.  The
// mark loop must skip it; before the fix it dereferenced it (SIGSEGV at
// address 0x8, reading Cell::next_and_flags).
TEST(GCMark, NullReferenceIsSkipped) {
    ProtoSpace space;
    ProtoContext* root = space.rootContext;

    const ProtoObject* taggedNull =
        reinterpret_cast<const ProtoObject*>(static_cast<uintptr_t>(POINTER_TAG_LIST));
    ASSERT_TRUE(ProtoObject::isCellPointer(taggedNull));
    ASSERT_EQ(ProtoObject::asCellPointer(taggedNull), nullptr);

    ProtoRootSet* rs = space.createRootSet("gc-mark-null-reference");
    ASSERT_NE(rs, nullptr);
    const ProtoRootSet::Handle hNull = rs->add(taggedNull);
    const ProtoList* live = root->newList()->appendLast(root, root->fromInteger(42));
    const ProtoRootSet::Handle hLive = rs->add(live->asObject(root));

    const uint64_t cyclesBefore = space.getGCCycleCount();
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(runOneCycle(space, root)) << "collection cycle " << i << " did not complete";
    }
    EXPECT_GE(space.getGCCycleCount() - cyclesBefore, 3u);

    const ProtoObject* obj = rs->resolve(hLive);
    ASSERT_NE(obj, nullptr);
    const ProtoList* l = obj->asList(root);
    ASSERT_NE(l, nullptr);
    ASSERT_EQ(l->getSize(root), 1u);
    EXPECT_EQ(l->getAt(root, 0)->asLong(root), 42);

    rs->remove(hNull);
    rs->remove(hLive);
    space.destroyRootSet(rs);
}

// A processReferences implementation that reports nullptr is a bug in that
// implementation.  Instrumented and debug builds abort and name the cell
// type; release builds without instrumentation skip the null entry.
TEST(GCMarkDeathTest, NullReferenceFromProcessReferencesIsReported) {
#if defined(PROTOCORE_GC_INSTRUMENT) || !defined(NDEBUG)
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(collectWithNullReportingCell(),
                 "null reference reported by a cell of type 0 .None.");
#else
    collectWithNullReportingCell();
    SUCCEED();
#endif
}
