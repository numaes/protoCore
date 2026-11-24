// ShapeIndexScalingTests.cpp — adding N attributes to one object must not
// walk the whole ProtoShape chain on every add.
//
// From 3.6.1 to 3.6.2 (exclusive) proving that a new name was absent read
// every block of the chain, so N adds cost O(N^2): 40,000 keys took ~40x as
// long as 10,000.  Indexed blocks (ProtoShape::isIndexed) now bound the walk.
//
// These tests count work, never wall-clock time:
//   - the blocks a lookup reads (shapeFindOffsetForTesting, the walk that
//     setAttribute / getAttribute run) stay under a constant for every add;
//   - the index is a balanced tree, so its height stays O(log N);
//   - the cells allocated per add stay O(log N).
// They run at N = 1,000 and N = 65,536; with the old linear walk the
// visited-block bound fails at the 65th attribute.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace proto;

namespace {

// Longest walk allowed for a chain of `count` names.  Up to
// SHAPE_INDEX_FIRST names there is no index: every block may be read
// (SHAPE_INDEX_FIRST / 4 = 16).  Past it, at most SHAPE_INDEX_STRIDE / 4 = 8
// blocks are read before an indexed block, plus one index probe.  The
// tighter bound past 64 catches a wrong stride.
unsigned maxVisits(proto_ulong count) {
    return count <= ProtoShape::SHAPE_INDEX_FIRST
        ? static_cast<unsigned>(ProtoShape::SHAPE_INDEX_FIRST / 4)
        : static_cast<unsigned>(ProtoShape::SHAPE_INDEX_STRIDE / 4 + 1);
}

const ProtoShapeIndex* firstIndexOf(const ProtoShape* shape) {
    for (const ProtoShape* b = shape; b != nullptr; b = b->nextBlock())
        if (b->isIndexed()) return b->index();
    return nullptr;
}

class ShapeIndexScalingTest : public ::testing::TestWithParam<int> {
protected:
    ProtoSpace* space = nullptr;
    ProtoContext* ctx = nullptr;

    void SetUp() override {
        space = new ProtoSpace();
        ctx = space->rootContext;
    }

    void TearDown() override { delete space; }

    std::vector<const ProtoString*> names(const std::string& prefix, int n) {
        std::vector<const ProtoString*> out;
        out.reserve(n);
        for (int i = 0; i < n; ++i)
            out.push_back(ProtoString::createSymbol(ctx, (prefix + std::to_string(i)).c_str()));
        return out;
    }

    // Adds `n` names one setAttribute at a time to an immutable object and
    // checks, after every add, the walk an add of a further (absent) name
    // would make.  Returns the object.
    const ProtoObject* buildChecked(const std::vector<const ProtoString*>& n,
                                    const ProtoString* absent,
                                    proto_ulong* cellsOut) {
        const ProtoObject* obj = ctx->newObject(false);
        const proto_ulong c0 = ctx->allocatedCellsCount;
        for (size_t i = 0; i < n.size(); ++i) {
            obj = obj->setAttribute(ctx, n[i], ctx->fromInteger(static_cast<long long>(i)));
            const ProtoShape* shape = toImpl<const ProtoObjectCell>(obj)->shape;
            unsigned visited = 0;
            EXPECT_EQ(shapeFindOffsetForTesting(shape, absent, &visited), -1);
            if (visited > maxVisits(shape->count)) {
                ADD_FAILURE() << "an add at count " << (i + 1) << " walks " << visited
                              << " blocks (bound " << maxVisits(shape->count) << ")";
                break;
            }
        }
        if (cellsOut) *cellsOut = ctx->allocatedCellsCount - c0;
        return obj;
    }
};

TEST_P(ShapeIndexScalingTest, AddWalksABoundedNumberOfBlocks) {
    const int n = GetParam();
    const auto keys = names("k", n);
    const ProtoString* absent = ProtoString::createSymbol(ctx, "never_added");
    proto_ulong cells = 0;
    const ProtoObject* obj = buildChecked(keys, absent, &cells);

    const ProtoShape* shape = toImpl<const ProtoObjectCell>(obj)->shape;
    ASSERT_EQ(shape->count, static_cast<proto_ulong>(n));

    // Every name resolves to its own offset through the same bounded walk.
    for (int i = 0; i < n; ++i) {
        unsigned visited = 0;
        ASSERT_EQ(shapeFindOffsetForTesting(shape, keys[i], &visited), i) << "attribute " << i;
        ASSERT_LE(visited, maxVisits(shape->count)) << "attribute " << i;
    }

    // The index is a balanced tree: height O(log N).
    if (const ProtoShapeIndex* ix = firstIndexOf(shape)) {
        const double bound = 1.45 * std::log2(static_cast<double>(n) + 2.0) + 2.0;
        EXPECT_LE(static_cast<double>(ix->names->height), bound);
        EXPECT_LE(ix->names->size, static_cast<proto_ulong>(n));
    } else {
        EXPECT_LE(static_cast<proto_ulong>(n), ProtoShape::SHAPE_INDEX_FIRST);
    }

    // Cells per add: the object cell, the value-tree path, the shape block
    // and, amortized, one persistent AVL insert into the index: O(log N).
    const double perAdd = static_cast<double>(cells) / n;
    const double cellBound = 8.0 + 2.5 * std::log2(static_cast<double>(n));
    EXPECT_LE(perAdd, cellBound) << "cells per add " << perAdd;

    // Values read back through getAttribute (cache on).
    for (int i = 0; i < n; ++i)
        ASSERT_EQ(obj->getAttribute(ctx, keys[i])->asLong(ctx), i) << "attribute " << i;
}

TEST_P(ShapeIndexScalingTest, MutableObjectAddsStayBounded) {
    // The protoJS case: one mutable object receiving N keys.
    const int n = GetParam();
    const auto keys = names("m", n);
    const ProtoString* absent = ProtoString::createSymbol(ctx, "never_added_m");
    const ProtoObject* obj = ctx->newObject(true);
    for (int i = 0; i < n; ++i) {
        obj->setAttribute(ctx, keys[i], ctx->fromInteger(i));
        // An immutable clone shares the mutable's current shape.
        const ProtoShape* shape = toImpl<const ProtoObjectCell>(obj->newClone(ctx, false))->shape;
        ASSERT_NE(shape, nullptr);
        ASSERT_EQ(shape->count, static_cast<proto_ulong>(i + 1));
        unsigned visited = 0;
        ASSERT_EQ(shapeFindOffsetForTesting(shape, absent, &visited), -1);
        ASSERT_LE(visited, maxVisits(shape->count)) << "an add at count " << (i + 1);
    }
    for (int i = 0; i < n; ++i)
        ASSERT_EQ(obj->getAttribute(ctx, keys[i])->asLong(ctx), i) << "attribute " << i;
}

TEST_P(ShapeIndexScalingTest, BulkSetAttributesBuildsTheSameIndex) {
    const int n = GetParam();
    const auto keys = names("b", n);
    std::vector<const ProtoObject*> values;
    values.reserve(n);
    for (int i = 0; i < n; ++i) values.push_back(ctx->fromInteger(i));
    // Two batches, the second crossing several indexed blocks.
    const int half = n / 3;
    const ProtoObject* obj = ctx->newObject(false)->setAttributes(
        ctx, static_cast<unsigned>(half), keys.data(), values.data());
    obj = obj->setAttributes(ctx, static_cast<unsigned>(n - half), keys.data() + half, values.data() + half);
    const ProtoShape* shape = toImpl<const ProtoObjectCell>(obj)->shape;
    ASSERT_EQ(shape->count, static_cast<proto_ulong>(n));
    const ProtoString* absent = ProtoString::createSymbol(ctx, "never_added_b");
    unsigned visited = 0;
    EXPECT_EQ(shapeFindOffsetForTesting(shape, absent, &visited), -1);
    EXPECT_LE(visited, maxVisits(shape->count));
    for (int i = 0; i < n; ++i) {
        ASSERT_EQ(shapeFindOffsetForTesting(shape, keys[i], &visited), i) << "attribute " << i;
        ASSERT_LE(visited, maxVisits(shape->count));
        ASSERT_EQ(obj->getAttribute(ctx, keys[i])->asLong(ctx), i);
    }
}

INSTANTIATE_TEST_SUITE_P(Sizes, ShapeIndexScalingTest, ::testing::Values(1000, 65536));

TEST(ShapeIndex, RemoveAndReAddKeepTheOffsetAcrossAnIndex) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;
    std::vector<const ProtoString*> keys;
    for (int i = 0; i < 300; ++i)
        keys.push_back(ProtoString::createSymbol(ctx, ("r" + std::to_string(i)).c_str()));
    const ProtoObject* obj = ctx->newObject(false);
    for (int i = 0; i < 300; ++i) obj = obj->setAttribute(ctx, keys[i], ctx->fromInteger(i));
    // Attribute 10 sits below several indexed blocks: found through an index.
    obj = obj->removeAttribute(ctx, keys[10]);
    EXPECT_EQ(obj->hasOwnAttribute(ctx, keys[10]), PROTO_FALSE);
    obj = obj->setAttribute(ctx, keys[10], ctx->fromInteger(1010));
    const ProtoShape* shape = toImpl<const ProtoObjectCell>(obj)->shape;
    EXPECT_EQ(shape->count, 300u) << "re-adding a removed name must reuse its offset";
    unsigned visited = 0;
    EXPECT_EQ(shapeFindOffsetForTesting(shape, keys[10], &visited), 10);
    EXPECT_EQ(obj->getAttribute(ctx, keys[10])->asLong(ctx), 1010);
    EXPECT_EQ(obj->getAttribute(ctx, keys[299])->asLong(ctx), 299);
}

TEST(ShapeIndex, IndexedCountBoundaries) {
    EXPECT_FALSE(ProtoShape::isIndexedCount(0));
    for (proto_ulong c = 1; c <= 68; ++c)
        EXPECT_EQ(ProtoShape::isIndexedCount(c), c >= 65) << "count " << c;
    EXPECT_FALSE(ProtoShape::isIndexedCount(69));
    EXPECT_FALSE(ProtoShape::isIndexedCount(96));
    EXPECT_TRUE(ProtoShape::isIndexedCount(97));
    EXPECT_TRUE(ProtoShape::isIndexedCount(100));
    EXPECT_FALSE(ProtoShape::isIndexedCount(101));
}

// Detects an untraced index only when the collector frees its cells AND the
// allocations below reuse them: the sweep leaves a freed cell's payload in
// place and clears the mark bit, the only per-cell liveness record, so no
// existing hook can tell a freed cell from a live one after the cycle.  The
// 200,000 allocations make reuse near certain on this heap; with the
// index's `names` left untraced this test fails.
TEST(ShapeIndex, IndexSurvivesCollection) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;
    std::vector<const ProtoString*> keys;
    for (int i = 0; i < 2000; ++i)
        keys.push_back(ProtoString::createSymbol(ctx, ("g" + std::to_string(i)).c_str()));
    ProtoRootSet* rs = space.createRootSet("shape-index-test");
    ProtoRootSet::Handle h = ProtoRootSet::kNullHandle;
    {
        ProtoContext sub(&space, ctx, nullptr, nullptr, nullptr, nullptr);
        const ProtoObject* obj = sub.newObject(false);
        for (int i = 0; i < 2000; ++i) obj = obj->setAttribute(&sub, keys[i], sub.fromInteger(i));
        h = rs->add(obj);
    }
    const uint64_t c0 = space.getGCCycleCount();
    for (int cycle = 0; cycle < 3; ++cycle) {
        {
            std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
            space.gcStarted = true;
            space.gcCV.notify_all();
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (space.gcStarted.load() && std::chrono::steady_clock::now() < deadline) {
            ctx->safepoint();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT_GT(space.getGCCycleCount(), c0) << "no collection ran; the test proves nothing";
    // Reuse freed cells: an untraced index would be overwritten here.
    for (int j = 0; j < 200000; ++j)
        ctx->newObject(false)->setAttribute(ctx, keys[j % 7], ctx->fromInteger(j));

    const ProtoObject* obj = rs->resolve(h);
    const ProtoShape* shape = toImpl<const ProtoObjectCell>(obj)->shape;
    const ProtoShapeIndex* ix = firstIndexOf(shape);
    ASSERT_NE(ix, nullptr);
    ASSERT_EQ(ix->getType(), CellType::ShapeIndex);
    for (int i = 0; i < 2000; ++i) {
        unsigned visited = 0;
        ASSERT_EQ(shapeFindOffsetForTesting(shape, keys[i], &visited), i);
        ASSERT_EQ(obj->getAttribute(ctx, keys[i])->asLong(ctx), i);
    }
    rs->remove(h);
    space.destroyRootSet(rs);
}

}  // namespace
