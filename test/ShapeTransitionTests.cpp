// ShapeTransitionTests.cpp — the shape transition table: objects that add the
// same attribute names in the same order share one ProtoShape chain.
//
// These tests assume the table is enabled (PROTOCORE_SHAPE_TRANSITIONS unset
// or not "0"); with it disabled they skip, and ShapeTransitionSwitch checks
// instead that the switch really turns sharing off (run the binary with
// PROTOCORE_SHAPE_TRANSITIONS=0 --gtest_filter='ShapeTransition*').

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace proto;

namespace {

bool transitionsDisabled() {
    const char* v = std::getenv("PROTOCORE_SHAPE_TRANSITIONS");
    return v && v[0] == '0';
}

const ProtoShape* shapeOf(const ProtoObject* obj) {
    return toImpl<const ProtoObjectCell>(obj)->shape;
}

class ShapeTransitionTest : public ::testing::Test {
protected:
    ProtoSpace* space = nullptr;
    ProtoContext* ctx = nullptr;

    void SetUp() override {
        if (transitionsDisabled()) GTEST_SKIP() << "PROTOCORE_SHAPE_TRANSITIONS=0";
        space = new ProtoSpace();
        ctx = space->rootContext;
    }

    void TearDown() override { delete space; }

    const ProtoString* sym(const std::string& s) {
        return ProtoString::createSymbol(ctx, s.c_str());
    }

    const ProtoObject* build(const std::vector<const ProtoString*>& names, bool mutableObj) {
        const ProtoObject* obj = ctx->newObject(mutableObj);
        for (size_t i = 0; i < names.size(); ++i)
            obj = obj->setAttribute(ctx, names[i], ctx->fromInteger(static_cast<long long>(i)));
        return obj;
    }

    std::vector<const ProtoString*> names(const std::string& prefix, int n) {
        std::vector<const ProtoString*> out;
        for (int i = 0; i < n; ++i) out.push_back(sym(prefix + std::to_string(i)));
        return out;
    }
};

// Shared state of the concurrent test's workers (a ProtoMethod is a plain
// function pointer, so the workers reach their inputs through globals).
std::vector<const ProtoString*>* gNames = nullptr;
std::atomic<const ProtoShape*> gShapes[8];
std::atomic<int> gNextSlot{0};
std::atomic<int> gMismatches{0};

const ProtoObject* buildSameShapeWorker(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                                        const ProtoArguments*, const ProtoKeywordArguments*) {
    const int slot = gNextSlot.fetch_add(1);
    const ProtoShape* first = nullptr;
    for (int round = 0; round < 200; ++round) {
        const ProtoObject* obj = ctx->newObject(false);
        for (size_t i = 0; i < gNames->size(); ++i)
            obj = obj->setAttribute(ctx, (*gNames)[i], ctx->fromInteger(round));
        const ProtoShape* s = shapeOf(obj);
        if (first == nullptr) first = s;
        else if (s != first) gMismatches.fetch_add(1);
    }
    gShapes[slot].store(first);
    return PROTO_NONE;
}

}  // namespace

TEST_F(ShapeTransitionTest, SameNamesInSameOrderShareTheShape) {
    const auto n = names("same_", 10);
    const ProtoObject* a = build(n, false);
    const ProtoObject* b = build(n, false);
    ASSERT_NE(shapeOf(a), nullptr);
    EXPECT_EQ(shapeOf(a), shapeOf(b));
    // Every prefix converges too: the chains are the same cells.
    EXPECT_EQ(shapeOf(a)->nextBlock(), shapeOf(b)->nextBlock());
    for (size_t i = 0; i < n.size(); ++i) {
        EXPECT_EQ(b->getAttribute(ctx, n[i])->asLong(ctx), static_cast<long long>(i));
    }
}

TEST_F(ShapeTransitionTest, MutableAndImmutableObjectsShareTheShape) {
    const auto n = names("mixed_", 6);
    EXPECT_EQ(shapeOf(build(n, false)), shapeOf(build(n, false)));
    const ProtoObject* m1 = build(n, true);
    const ProtoObject* m2 = build(n, true);
    for (size_t i = 0; i < n.size(); ++i) {
        EXPECT_EQ(m1->getAttribute(ctx, n[i])->asLong(ctx), static_cast<long long>(i));
        EXPECT_EQ(m2->getAttribute(ctx, n[i])->asLong(ctx), static_cast<long long>(i));
    }
}

TEST_F(ShapeTransitionTest, DifferentOrderGivesDifferentShapes) {
    auto n = names("order_", 5);
    const ProtoObject* a = build(n, false);
    std::swap(n[1], n[3]);
    const ProtoObject* b = build(n, false);
    EXPECT_NE(shapeOf(a), shapeOf(b));
    EXPECT_EQ(shapeOf(a)->count, shapeOf(b)->count);
}

TEST_F(ShapeTransitionTest, BulkAndSingleStepWritesConverge) {
    const auto n = names("bulk_", 9);
    const ProtoObject* single = build(n, false);
    std::vector<const ProtoObject*> values;
    for (size_t i = 0; i < n.size(); ++i) values.push_back(ctx->fromInteger(static_cast<long long>(i)));
    const ProtoObject* bulk = ctx->newObject(false)->setAttributes(
        ctx, static_cast<unsigned>(n.size()), n.data(), values.data());
    EXPECT_EQ(shapeOf(single), shapeOf(bulk));
    for (size_t i = 0; i < n.size(); ++i)
        EXPECT_EQ(bulk->getAttribute(ctx, n[i])->asLong(ctx), static_cast<long long>(i));
}

TEST_F(ShapeTransitionTest, ObjectsPastTheCapKeepPrivateShapes) {
    const auto n = names("wide_", 70);
    const ProtoObject* a = build(n, false);
    const ProtoObject* b = build(n, false);
    ASSERT_EQ(shapeOf(a)->count, 70u);
    // Shapes up to the cap are shared; the 65th attribute onward is private.
    EXPECT_NE(shapeOf(a), shapeOf(b));
    const ProtoShape* sa = shapeOf(a);
    while (sa && sa->count > 64) sa = sa->nextBlock();
    const ProtoShape* sb = shapeOf(b);
    while (sb && sb->count > 64) sb = sb->nextBlock();
    ASSERT_NE(sa, nullptr);
    EXPECT_EQ(sa->count, 64u);
    EXPECT_EQ(sa, sb);
    for (size_t i = 0; i < n.size(); ++i)
        EXPECT_EQ(b->getAttribute(ctx, n[i])->asLong(ctx), static_cast<long long>(i));
    // A bulk write that crosses the cap stays correct.
    std::vector<const ProtoObject*> values;
    for (size_t i = 0; i < n.size(); ++i) values.push_back(ctx->fromInteger(static_cast<long long>(i)));
    const ProtoObject* bulk = ctx->newObject(false)->setAttributes(
        ctx, static_cast<unsigned>(n.size()), n.data(), values.data());
    ASSERT_EQ(shapeOf(bulk)->count, 70u);
    for (size_t i = 0; i < n.size(); ++i)
        EXPECT_EQ(bulk->getAttribute(ctx, n[i])->asLong(ctx), static_cast<long long>(i));
}

TEST_F(ShapeTransitionTest, InternedShapesSurviveCollectionWithNoObjectLeft) {
    const auto n = names("gc_", 7);
    // Build the object in a child context that ends before the collection:
    // nothing references the object any more, only the table pins its shape.
    const ProtoShape* before = nullptr;
    {
        ProtoContext sub(space, ctx, nullptr, nullptr, nullptr, nullptr);
        const ProtoObject* obj = sub.newObject(false);
        for (size_t i = 0; i < n.size(); ++i)
            obj = obj->setAttribute(&sub, n[i], sub.fromInteger(static_cast<long long>(i)));
        before = shapeOf(obj);
    }
    // triggerGC() is gated on heap pressure, so request cycles directly and
    // cooperate at safepoints until each completes (GCSurvivorRechainTests).
    const uint64_t c0 = space->getGCCycleCount();
    for (int cycle = 0; cycle < 3; ++cycle) {
        {
            std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
            space->gcStarted = true;
            space->gcCV.notify_all();
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (space->gcStarted.load() && std::chrono::steady_clock::now() < deadline) {
            ctx->safepoint();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT_GT(space->getGCCycleCount(), c0) << "no collection ran; the test proves nothing";

    // Reuse freed cells: an unpinned shape would be overwritten here.
    for (int j = 0; j < 200000; ++j) ctx->newObject(false)->setAttribute(ctx, n[0], ctx->fromInteger(j));
    ASSERT_EQ(before->getType(), CellType::Shape);
    ASSERT_EQ(before->count, 7u);
    const ProtoShape* walk = before;
    for (int i = 6; i >= 0; --i) {
        const unsigned slot = static_cast<unsigned>(3 - (i % 4));
        if (i < 6 && i % 4 == 3) walk = walk->nextBlock();
        ASSERT_NE(walk, nullptr);
        EXPECT_EQ(walk->names[slot], n[i]) << "attribute " << i;
    }

    const ProtoObject* again = build(n, false);
    EXPECT_EQ(shapeOf(again), before);
    const ProtoShape* s = shapeOf(again);
    ASSERT_EQ(s->count, 7u);
    for (size_t i = 0; i < n.size(); ++i)
        EXPECT_EQ(again->getAttribute(ctx, n[i])->asLong(ctx), static_cast<long long>(i));
}

TEST_F(ShapeTransitionTest, ConcurrentBuildersConvergeOnOneShape) {
    auto n = names("conc_", 12);
    gNames = &n;
    gNextSlot.store(0);
    gMismatches.store(0);
    for (auto& s : gShapes) s.store(nullptr);
    constexpr int kThreads = 8;
    std::vector<const ProtoThread*> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.push_back(space->newThread(ctx, sym("shape-builder"), buildSameShapeWorker, nullptr, nullptr));
    for (const ProtoThread* th : threads) const_cast<ProtoThread*>(th)->join(ctx);
    gNames = nullptr;

    EXPECT_EQ(gMismatches.load(), 0);
    const ProtoShape* expected = shapeOf(build(n, false));
    for (int t = 0; t < kThreads; ++t) EXPECT_EQ(gShapes[t].load(), expected) << "thread " << t;
}

TEST(ShapeTransitionSpaces, ManySpacesCreateAndFreeTheirTables) {
    if (transitionsDisabled()) GTEST_SKIP() << "PROTOCORE_SHAPE_TRANSITIONS=0";
    for (int k = 0; k < 20; ++k) {
        auto* space = new ProtoSpace();
        ProtoContext* ctx = space->rootContext;
        const ProtoString* a = ProtoString::createSymbol(ctx, "space_a");
        const ProtoString* b = ProtoString::createSymbol(ctx, "space_b");
        const ProtoObject* o1 = ctx->newObject(false)->setAttribute(ctx, a, ctx->fromInteger(1))
                                    ->setAttribute(ctx, b, ctx->fromInteger(2));
        const ProtoObject* o2 = ctx->newObject(false)->setAttribute(ctx, a, ctx->fromInteger(3))
                                    ->setAttribute(ctx, b, ctx->fromInteger(4));
        EXPECT_EQ(shapeOf(o1), shapeOf(o2));
        EXPECT_EQ(o2->getAttribute(ctx, b)->asLong(ctx), 4);
        delete space;
    }
}

TEST(ShapeTransitionSwitch, DisabledTableGivesEveryObjectItsOwnShape) {
    if (!transitionsDisabled()) GTEST_SKIP() << "runs only with PROTOCORE_SHAPE_TRANSITIONS=0";
    auto* space = new ProtoSpace();
    ProtoContext* ctx = space->rootContext;
    const ProtoString* a = ProtoString::createSymbol(ctx, "switch_a");
    const ProtoObject* o1 = ctx->newObject(false)->setAttribute(ctx, a, ctx->fromInteger(1));
    const ProtoObject* o2 = ctx->newObject(false)->setAttribute(ctx, a, ctx->fromInteger(2));
    EXPECT_NE(shapeOf(o1), shapeOf(o2));
    delete space;
}
