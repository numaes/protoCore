// NewThreadRootsTests.cpp -- creating a thread must not detach anyone's roots.
//
// ProtoSpace::newThread builds a temporary context with no previous context
// and no thread.  ProtoContext's constructor registers such a context as
// ProtoSpace::mainContext, the root the collector scans for thread-less
// contexts, and newThread never put the previous value back.  A thread-less
// context in use when another thread created a thread -- protoST's main
// program, whose worker pool grows while actors block in I/O -- stopped being
// scanned, and the objects only it held were freed.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace proto;

namespace {

std::atomic<bool> g_created{false};

const ProtoObject* noop(ProtoContext*, const ProtoObject*, const ParentLink*,
                        const ProtoList*, const ProtoSparseList*) {
    return PROTO_NONE;
}

const ProtoObject* creator(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                           const ProtoList*, const ProtoSparseList*) {
    const ProtoThread* t = ctx->space->newThread(ctx, ProtoString::createSymbol(ctx, "child"),
                                                 noop, nullptr, nullptr);
    g_created = true;
    const_cast<ProtoThread*>(t)->join(ctx);
    return PROTO_NONE;
}

}  // namespace

TEST(NewThreadRoots, CreatingAThreadKeepsTheMainContextRoot) {
    ProtoSpace space;
    ProtoContext* before = space.mainContext;
    g_created = false;
    const ProtoThread* w = space.newThread(space.rootContext,
                                           ProtoString::createSymbol(space.rootContext, "creator"),
                                           creator, nullptr, nullptr);
    {
        ProtoContext::UnmanagedScope out(space.rootContext);
        while (!g_created) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const_cast<ProtoThread*>(w)->join(space.rootContext);
    }
    EXPECT_EQ(space.mainContext, before)
        << "newThread left space->mainContext on its temporary context";
}

// The consequence: objects held only by a thread-less context survive
// collections that follow a thread creation on another thread.
TEST(NewThreadRoots, AThreadLessContextSurvivesAThreadCreatedElsewhere) {
    ProtoSpace space;
    bool intact = false;
    std::thread host([&] {
        // A context on an unregistered OS thread has no ProtoThread: it is
        // rooted through space->mainContext.
        ProtoContext holder(&space, nullptr, nullptr, nullptr, nullptr, nullptr, 1);
        ASSERT_EQ(holder.thread, nullptr);
        const ProtoList* keep = holder.newList();
        for (int i = 0; i < 100; ++i) keep = keep->appendLast(&holder, holder.fromInteger(i));
        holder.getAutomaticLocals()[0] = keep->asObject(&holder);
        holder.safepoint();

        g_created = false;
        const ProtoThread* w = space.newThread(space.rootContext,
                                               ProtoString::createSymbol(space.rootContext, "creator"),
                                               creator, nullptr, nullptr);
        while (!g_created) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        (void) w;

        space.setHeapLimits(0, space.heapSize + 40000);
        const uint64_t start = space.getGCCycleCount();
        // Garbage made in the holder itself: a child context would re-register
        // itself (and then the holder) as mainContext and hide the defect.
        for (int b = 0; b < 200 && space.getGCCycleCount() - start < 3; ++b) {
            for (int i = 0; i < 5000; ++i) (void) holder.newObject(false);
            holder.safepoint();
        }
        space.setHeapLimits(0, 0);
        const ProtoList* back = holder.getAutomaticLocals()[0]->asList(&holder);
        intact = back && back->getSize(&holder) == 100;
        for (int i = 0; intact && i < 100; ++i) intact = back->getAt(&holder, i)->asLong(&holder) == i;
    });
    host.join();
    EXPECT_TRUE(intact) << "a collection after newThread freed objects of a thread-less context";
}
