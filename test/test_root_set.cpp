// Tests for ProtoRootSet — embedder-owned GC root registry.
//
// Coverage:
//   - Basic add/resolve/remove round-trips.
//   - Stale handles after remove resolve to nullptr (don't accidentally
//     resurrect a freed slot's contents).
//   - Slot recycling after remove keeps generations distinct.
//   - Live size accounting matches add/remove balance.
//   - GC scan during STW counts pinned roots even when the JS-side
//     reference graph would otherwise let them be reclaimed.
//   - createRootSet / destroyRootSet round-trips, and ProtoSpace
//     destructor frees orphan sets without leaking.
//   - Concurrent add/remove from multiple threads is safe.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include <atomic>
#include <thread>
#include <vector>

using namespace proto;

class RootSetTest : public ::testing::Test {
protected:
    ProtoSpace* space = nullptr;
    ProtoContext* context = nullptr;

    void SetUp() override {
        space = new ProtoSpace();
        context = space->rootContext;
    }
    void TearDown() override {
        delete space;
    }
};

TEST_F(RootSetTest, AddResolveRemoveRoundTrip) {
    auto* rs = space->createRootSet("test");
    ASSERT_NE(rs, nullptr);

    const ProtoObject* obj = context->newObject(true);
    auto h = rs->add(obj);
    ASSERT_NE(h, ProtoRootSet::kNullHandle);
    EXPECT_EQ(rs->resolve(h), obj);
    EXPECT_EQ(rs->size(), 1u);

    rs->remove(h);
    EXPECT_EQ(rs->resolve(h), nullptr);
    EXPECT_EQ(rs->size(), 0u);
}

TEST_F(RootSetTest, AddNullReturnsNullHandle) {
    auto* rs = space->createRootSet("test");
    EXPECT_EQ(rs->add(nullptr), ProtoRootSet::kNullHandle);
    EXPECT_EQ(rs->size(), 0u);
}

TEST_F(RootSetTest, StaleHandleAfterSlotReuseDoesNotResolve) {
    auto* rs = space->createRootSet("test");
    const ProtoObject* obj1 = context->newObject(true);
    const ProtoObject* obj2 = context->newObject(true);

    auto h1 = rs->add(obj1);
    rs->remove(h1);

    // Slot is now free; the next add must reuse it but with a fresh
    // generation, so the stale h1 must NOT resolve to obj2.
    auto h2 = rs->add(obj2);
    EXPECT_EQ(rs->resolve(h2), obj2);
    EXPECT_EQ(rs->resolve(h1), nullptr);
    EXPECT_NE(h1, h2);
}

TEST_F(RootSetTest, MultiplePinsTrackedIndependently) {
    auto* rs = space->createRootSet("test");
    constexpr int N = 16;
    std::vector<const ProtoObject*> objs;
    std::vector<ProtoRootSet::Handle> handles;
    for (int i = 0; i < N; i++) {
        auto* o = context->newObject(true);
        objs.push_back(o);
        handles.push_back(rs->add(o));
    }
    EXPECT_EQ(rs->size(), static_cast<unsigned long>(N));
    for (int i = 0; i < N; i++) {
        EXPECT_EQ(rs->resolve(handles[i]), objs[i]);
    }
    // Remove half, the other half should still resolve.
    for (int i = 0; i < N; i += 2) rs->remove(handles[i]);
    EXPECT_EQ(rs->size(), static_cast<unsigned long>(N / 2));
    for (int i = 0; i < N; i++) {
        if (i % 2 == 0) EXPECT_EQ(rs->resolve(handles[i]), nullptr);
        else            EXPECT_EQ(rs->resolve(handles[i]), objs[i]);
    }
}

TEST_F(RootSetTest, ForEachIteratesLiveRoots) {
    auto* rs = space->createRootSet("test");
    constexpr int N = 8;
    std::vector<const ProtoObject*> kept;
    for (int i = 0; i < N; i++) {
        auto* o = context->newObject(true);
        auto h = rs->add(o);
        if (i % 2 == 0) kept.push_back(o);
        else            rs->remove(h);
    }
    int counted = 0;
    rs->forEachRoot(
        [](void* user, const ProtoObject* obj) {
            (void)obj;
            (*static_cast<int*>(user))++;
        },
        &counted);
    EXPECT_EQ(counted, static_cast<int>(kept.size()));
}

TEST_F(RootSetTest, RemoveOfStaleHandleIsNoop) {
    auto* rs = space->createRootSet("test");
    auto* o = context->newObject(true);
    auto h = rs->add(o);
    rs->remove(h);
    // Removing again must not throw and must not corrupt the bookkeeping.
    rs->remove(h);
    EXPECT_EQ(rs->size(), 0u);
}

TEST_F(RootSetTest, SpaceDestructorFreesOrphanSets) {
    // Local space so we can observe the destructor cleanup path.
    auto* localSpace = new ProtoSpace();
    auto* rs = localSpace->createRootSet("orphan");
    auto* o = localSpace->rootContext->newObject(true);
    rs->add(o);
    // Don't call destroyRootSet — destructor must clean up regardless.
    delete localSpace;
    SUCCEED();  // reaching here without crash is the assertion
}

TEST_F(RootSetTest, ExplicitDestroyDetachesFromSpace) {
    auto* rs = space->createRootSet("explicit");
    auto* o = context->newObject(true);
    rs->add(o);
    space->destroyRootSet(rs);
    // The set is gone; no observable effect on subsequent operations.
    auto* rs2 = space->createRootSet("after");
    EXPECT_EQ(rs2->size(), 0u);
}

TEST_F(RootSetTest, ConcurrentAddRemoveIsSafe) {
    auto* rs = space->createRootSet("concurrent");
    constexpr int kThreads = 4;
    constexpr int kOpsPerThread = 200;
    std::atomic<int> errors{0};

    auto worker = [&]() {
        std::vector<ProtoRootSet::Handle> handles;
        handles.reserve(kOpsPerThread);
        for (int i = 0; i < kOpsPerThread; i++) {
            auto* o = context->newObject(true);
            auto h = rs->add(o);
            if (rs->resolve(h) != o) errors++;
            handles.push_back(h);
        }
        for (auto h : handles) {
            rs->remove(h);
            if (rs->resolve(h) != nullptr) errors++;
        }
    };

    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; t++) ts.emplace_back(worker);
    for (auto& t : ts) t.join();

    EXPECT_EQ(errors.load(), 0);
    EXPECT_EQ(rs->size(), 0u);
}

TEST_F(RootSetTest, GCMarksPinnedObjects) {
    // Pin an object that has no other strong reachability path, then
    // trigger a GC and verify the object is still alive (its attribute
    // round-trips correctly).  Without ProtoRootSet integration the
    // GC would reclaim it.
    auto* rs = space->createRootSet("gc-test");
    const ProtoObject* obj = context->newObject(true);
    const ProtoString* k = context->fromUTF8String("marker")->asString(context);
    obj->setAttribute(context, k, context->fromInteger(0xDEADBEEFLL));
    auto h = rs->add(obj);

    // Provoke a GC.  Requesting many cells forces a collection cycle.
    for (int i = 0; i < 1000; i++) {
        (void) context->newObject(true);
    }
    space->triggerGC();
    // Give the GC thread a moment to do its work.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const ProtoObject* roundTrip = rs->resolve(h);
    ASSERT_EQ(roundTrip, obj);
    const ProtoObject* val = obj->getAttribute(context, k, false);
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(val->asLong(context), 0xDEADBEEFLL);
}
