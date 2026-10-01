// SparseListBulkRemoveTests.cpp - removing many keys from a ProtoSparseList at once.
//
// sparseListRemoveSorted removes a sorted set of keys in one pass over the
// tree: subtrees that hold none of the keys are kept as they are, subtrees
// whose every key goes are dropped without allocating, and the survivors are
// joined back into a balanced tree. The collector uses it to release the
// entries of dead mutable objects from the process-wide mutable table; one
// removeAt per entry path-copied the tree once per key, and that garbage,
// allocated by the collector itself, could push the heap past its ceiling.
//
// These tests check that the result equals removing the keys one by one, that
// the tree stays a valid AVL tree with correct sizes, and that a dense range
// of keys costs far fewer cells than one removeAt per key.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <vector>

using namespace proto;

namespace {

struct Checked {
    int height;
    proto::proto_ulong size;
};

// Validates the AVL invariants of an AVL-form tree and collects its entries in
// order. Fails the test on a broken invariant.
Checked checkAvl(const ProtoSparseListImplementation* node,
                 std::vector<std::pair<proto::proto_ulong, const ProtoObject*>>& out) {
    if (!node || node->isEmpty) return {0, 0};
    const Checked l = checkAvl(node->previous, out);
    out.emplace_back(node->key, node->value);
    const Checked r = checkAvl(node->next, out);
    EXPECT_LE(std::abs(l.height - r.height), 1) << "unbalanced at key " << node->key;
    const int h = std::max(l.height, r.height) + 1;
    EXPECT_EQ(static_cast<int>(node->height), h) << "stale height at key " << node->key;
    EXPECT_EQ(static_cast<proto::proto_ulong>(node->size), l.size + r.size + 1) << "stale size at key " << node->key;
    return {h, l.size + r.size + 1};
}

std::vector<std::pair<proto::proto_ulong, const ProtoObject*>> entries(ProtoContext* ctx, const ProtoSparseList* sl) {
    std::vector<std::pair<proto::proto_ulong, const ProtoObject*>> out;
    ProtoObjectPointer pa{};
    pa.oid = reinterpret_cast<const ProtoObject*>(sl);
    if (pa.op.pointer_tag == POINTER_TAG_SPARSE_LIST_SMALL) {
        const auto* it = sl->getIterator(ctx);
        while (it->hasNext(ctx)) {
            out.emplace_back(it->nextKey(ctx), it->nextValue(ctx));
            it = const_cast<ProtoSparseListIterator*>(it)->advance(ctx);
        }
        std::sort(out.begin(), out.end());
        return out;
    }
    checkAvl(toImpl<const ProtoSparseListImplementation>(sl), out);
    return out;
}

const ProtoSparseList* build(ProtoContext* ctx, const std::vector<proto::proto_ulong>& keys) {
    const ProtoSparseList* sl = ctx->newSparseList();
    for (proto::proto_ulong k : keys) sl = sl->setAt(ctx, k, ctx->fromInteger(static_cast<long long>(k)));
    return sl;
}

const ProtoSparseList* removeOneByOne(ProtoContext* ctx, const ProtoSparseList* sl,
                                      const std::vector<proto::proto_ulong>& keys) {
    for (proto::proto_ulong k : keys) sl = sl->removeAt(ctx, k);
    return sl;
}

} // namespace

class SparseListBulkRemove : public ::testing::Test {
protected:
    ProtoSpace space;
    ProtoContext* ctx = nullptr;
    void SetUp() override { ctx = space.rootContext; }
};

TEST_F(SparseListBulkRemove, MatchesRemovingTheKeysOneByOne) {
    std::mt19937_64 rng(20260930);
    for (int round = 0; round < 60; ++round) {
        const int n = 1 + static_cast<int>(rng() % 700);
        std::vector<proto::proto_ulong> present;
        for (int i = 0; i < n; ++i) present.push_back(1 + rng() % 2000);
        const ProtoSparseList* tree = build(ctx, present);

        // A sorted, duplicate-free mix of present and absent keys, sometimes a
        // dense range, sometimes everything.
        std::vector<proto::proto_ulong> gone;
        const int mode = round % 4;
        if (mode == 0) {
            const proto::proto_ulong lo = 1 + rng() % 2000, hi = lo + rng() % 400;
            for (proto::proto_ulong k = lo; k <= hi; ++k) gone.push_back(k);
        } else if (mode == 1) {
            for (int i = 0; i < n / 3; ++i) gone.push_back(1 + rng() % 2200);
        } else if (mode == 2) {
            gone = present;
        } else {
            for (int i = 0; i < 5; ++i) gone.push_back(5000 + i);  // all absent
        }
        std::sort(gone.begin(), gone.end());
        gone.erase(std::unique(gone.begin(), gone.end()), gone.end());

        const ProtoSparseList* expected = removeOneByOne(ctx, tree, gone);
        const ProtoSparseList* actual = sparseListRemoveSorted(ctx, tree, gone.data(), gone.size());
        ASSERT_EQ(entries(ctx, actual), entries(ctx, expected)) << "round " << round;
        EXPECT_EQ(actual->getSize(ctx), expected->getSize(ctx)) << "round " << round;
        if (mode == 3) EXPECT_EQ(actual, tree) << "removing only absent keys must answer the tree itself";
    }
}

// Trees shaped like the mutable table's shards: keys carry a space id in the
// high bits (mutable_refs), and earlier single removals left empty nodes as
// children inside the tree.
TEST_F(SparseListBulkRemove, MatchesOnTreesThatAlreadyHadRemovals) {
    std::mt19937_64 rng(4242);
    const proto::proto_ulong space = PROTO_UL(3) << 40;
    for (int round = 0; round < 400; ++round) {
        const int n = 1 + static_cast<int>(rng() % 400);
        std::vector<proto::proto_ulong> present;
        for (int i = 0; i < n; ++i) present.push_back(space + 1 + rng() % 1000);
        const ProtoSparseList* tree = build(ctx, present);
        std::vector<proto::proto_ulong> earlier;
        for (int i = 0; i < n / 2; ++i) earlier.push_back(space + 1 + rng() % 1000);
        tree = removeOneByOne(ctx, tree, earlier);

        std::vector<proto::proto_ulong> gone;
        const proto::proto_ulong lo = space + 1 + rng() % 1000;
        const proto::proto_ulong span = rng() % 300;
        for (proto::proto_ulong k = lo; k <= lo + span; ++k) if (rng() % 4 != 0) gone.push_back(k);
        for (int i = 0; i < 20; ++i) gone.push_back(space + 1 + rng() % 1100);
        std::sort(gone.begin(), gone.end());
        gone.erase(std::unique(gone.begin(), gone.end()), gone.end());

        const ProtoSparseList* expected = removeOneByOne(ctx, tree, gone);
        const ProtoSparseList* actual = sparseListRemoveSorted(ctx, tree, gone.data(), gone.size());
        ASSERT_EQ(entries(ctx, actual), entries(ctx, expected)) << "round " << round;
    }
}

TEST_F(SparseListBulkRemove, NoKeysAnswersTheTreeItself) {
    const ProtoSparseList* tree = build(ctx, {3, 1, 4, 15, 9, 26, 5});
    EXPECT_EQ(sparseListRemoveSorted(ctx, tree, nullptr, 0), tree);
}

TEST_F(SparseListBulkRemove, WorksOnTheSmallForm) {
    const ProtoSparseList* tree = build(ctx, {7, 2});
    const proto::proto_ulong gone[] = {2, 99};
    const ProtoSparseList* r = sparseListRemoveSorted(ctx, tree, gone, 2);
    EXPECT_EQ(r->getSize(ctx), 1u);
    EXPECT_EQ(r->getAt(ctx, 2), PROTO_NONE);
    EXPECT_NE(r->getAt(ctx, 7), PROTO_NONE);
}

// The collector's case: mutable_refs are handed out in sequence, so the dead
// ones of a cycle are mostly dense runs. Removing such a run must cost far
// fewer cells than one path copy per key.
TEST_F(SparseListBulkRemove, ADenseRunCostsFarFewerCellsThanOneRemoveAtPerKey) {
    std::vector<proto::proto_ulong> keys;
    for (proto::proto_ulong k = 1; k <= 20000; ++k) keys.push_back(k);
    const ProtoSparseList* tree = build(ctx, keys);
    std::vector<proto::proto_ulong> gone;
    for (proto::proto_ulong k = 2001; k <= 18000; ++k) gone.push_back(k);

    ProtoContext one(&space, ctx, nullptr, nullptr, nullptr, nullptr);
    const proto::proto_ulong before1 = one.allocatedCellsCount;
    const ProtoSparseList* expected = removeOneByOne(&one, tree, gone);
    const proto::proto_ulong seqCells = one.allocatedCellsCount - before1;

    ProtoContext bulk(&space, ctx, nullptr, nullptr, nullptr, nullptr);
    const proto::proto_ulong before2 = bulk.allocatedCellsCount;
    const ProtoSparseList* actual = sparseListRemoveSorted(&bulk, tree, gone.data(), gone.size());
    const proto::proto_ulong bulkCells = bulk.allocatedCellsCount - before2;

    std::printf("[ BULK     ] removing 16000 of 20000 keys: %lu cells one by one, %lu in bulk\n",
                seqCells, bulkCells);
    ASSERT_EQ(entries(ctx, actual), entries(ctx, expected));
    EXPECT_LT(bulkCells * 20, seqCells)
        << "bulk removal allocated " << bulkCells << " cells, one-by-one " << seqCells;
}
