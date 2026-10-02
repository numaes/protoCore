// ContextReturnAnchorTests.cpp -- a context's return value must stay rooted
// while ~ProtoContext anchors it in the caller.
//
// ~ProtoContext hands the return value to `previous` by allocating a
// ReturnReference in `previous`'s young chain.  That allocation can block for
// a collection: at a stop-the-world poll, or in getFreeCells when the heap is
// at its hard limit and the freelists are empty (waitForHeapHeadroom).  If the
// dying context has already been popped off the thread's stack at that point,
// the collection's root scan reaches neither its return value nor its young
// chain, and a return value that is already a sweep candidate is freed while
// the caller is about to receive it.
//
// The test builds that situation deterministically:
//   * `inner` (a child of `callee`) builds an object holding a list and
//     returns it.  Destroying `inner` submits its young chain, so the object
//     and the list are sweep candidates, reachable only through `callee`
//     (its returnValue and the ReturnReference in its young chain);
//   * `callee` returns the same object to `caller`;
//   * the hard heap limit is set to the current heap size and `callee` drains
//     the space-level freelists and the thread's own freelist with
//     allocations that open no critical section;
//   * destroying `callee` then allocates the ReturnReference in `caller` with
//     every freelist empty and the heap at its ceiling, so that allocation
//     waits for a collection cycle -- in the middle of ~ProtoContext.
// The returned object and its list must be intact afterwards and after
// further cycles.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>

using namespace proto;

namespace {

void requestCycle(ProtoSpace& space) {
    std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
    space.gcStarted = true;
    space.gcCV.notify_all();
}

// Runs `count` collection cycles, parking `ctx` at safepoints meanwhile.
bool runCycles(ProtoSpace& space, ProtoContext* ctx, uint64_t count) {
    const uint64_t target = space.getGCCycleCount() + count;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (space.getGCCycleCount() < target) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        requestCycle(space);
        ctx->safepoint();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

constexpr int kElements = 200;

// The first word of a Cell is its vtable pointer.  A cell the collector freed
// and the allocator handed out again was reconstructed, possibly as another
// type: comparing that word with the one recorded at construction detects the
// reuse without a virtual call through a stale object.
uintptr_t vtableWord(const ProtoObject* obj) {
    uintptr_t word = 0;
    std::memcpy(&word, ProtoObject::asCellPointer(obj), sizeof(word));
    return word;
}

// The object built by `inner`: {payload: [0, 1, ..., kElements - 1]}.
int countMismatches(ProtoContext* ctx, const ProtoObject* obj, uintptr_t objVtable,
                    const ProtoString* key) {
    if (!obj || vtableWord(obj) != objVtable) return kElements + 1;
    const ProtoObject* payload = obj->getAttribute(ctx, key, false);
    if (!payload || payload == PROTO_NONE) return kElements + 1;
    const ProtoList* list = payload->asList(ctx);
    if (!list || list->getSize(ctx) != static_cast<unsigned long>(kElements)) return kElements + 1;
    int mismatches = 0;
    for (int i = 0; i < kElements; ++i) {
        const ProtoObject* element = list->getAt(ctx, i);
        if (!element || !element->isInteger(ctx) || element->asLong(ctx) != i) ++mismatches;
    }
    return mismatches;
}

}  // namespace

TEST(ContextReturnAnchor, ReturnValueSurvivesACollectionInsideTheDestructor) {
    ProtoSpace space;
    ProtoContext* root = space.rootContext;
    ASSERT_NE(root->thread, nullptr) << "the test needs the main thread's ProtoThread";
    auto* threadImpl = toImpl<ProtoThreadImplementation>(root->thread);
    ASSERT_NE(threadImpl->extension, nullptr);
    const ProtoString* key = ProtoString::createSymbol(root, "payload");

    ProtoContext caller(&space, root, nullptr, nullptr, nullptr, nullptr, 1);
    const ProtoObject* returned = nullptr;
    uintptr_t returnedVtable = 0;
    uint64_t cyclesDuringReturn = 0;
    {
        ProtoContext callee(&space, &caller, nullptr, nullptr, nullptr, nullptr);
        {
            ProtoContext inner(&space, &callee, nullptr, nullptr, nullptr, nullptr);
            const ProtoList* list = inner.newList();
            for (int i = 0; i < kElements; ++i) list = list->appendLast(&inner, inner.fromInteger(i));
            const ProtoObject* obj = inner.newObject(false);
            obj = obj->setAttribute(&inner, key, list->asObject(&inner));
            // Garbage the collection inside ~callee can reclaim, so the
            // headroom wait ends with a refilled freelist, not an OOM.
            for (int i = 0; i < 4096; ++i) (void) inner.newList();
            inner.returnValue = obj;
            returned = obj;
            returnedVtable = vtableWord(obj);
        }
        // `returned` and its list are sweep candidates now (inner's young
        // chain was submitted), rooted only through `callee`.
        callee.returnValue = returned;

        // Heap at its ceiling, every freelist empty: the next refill blocks
        // for a collection.  newList opens no critical section, so the drain
        // itself never waits.
        space.setHeapLimits(/*soft=*/0, /*hard=*/space.heapSize);
        for (long n = 0; n < 50000000L &&
                (space.freeChunks || space.freeCells || threadImpl->extension->freeCells); ++n) {
            (void) callee.newList();
        }
        ASSERT_EQ(space.freeChunks, nullptr);
        ASSERT_EQ(space.freeCells, nullptr);
        ASSERT_EQ(threadImpl->extension->freeCells, nullptr);
        cyclesDuringReturn = space.getGCCycleCount();
    }   // ~callee: the ReturnReference allocation in `caller` waits for a cycle.
    cyclesDuringReturn = space.getGCCycleCount() - cyclesDuringReturn;
    space.setHeapLimits(0, 0);
    caller.getAutomaticLocals()[0] = returned;

    EXPECT_GE(cyclesDuringReturn, 1u)
        << "no collection ran while ~ProtoContext anchored the return value";
    EXPECT_EQ(countMismatches(&caller, returned, returnedVtable, key), 0)
        << "the return value was freed while ~ProtoContext anchored it in the caller";

    // Reuse whatever the collection freed, then collect again: a freed
    // return value would now be overwritten.
    for (int i = 0; i < 20000; ++i) (void) caller.newList()->appendLast(&caller, caller.fromInteger(i));
    ASSERT_TRUE(runCycles(space, &caller, 3)) << "collection cycles did not complete";
    EXPECT_EQ(countMismatches(&caller, returned, returnedVtable, key), 0)
        << "the return value did not survive later collections";
}
