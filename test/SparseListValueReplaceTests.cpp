// SparseListValueReplaceTests.cpp - a new value for a key already present.
//
// Giving an existing key a new value does not change the shape of the AVL
// tree, so implSetAt copies only the root-to-key path and every copied node
// keeps the size and height of the node it replaces, without reading the
// siblings and without rebalancing (sparse_avl::replaceValue).  Inserts and
// removals still take the general setAt path.
//
// These tests check that the tree stays a valid AVL tree with correct sizes
// and heights after any sequence of replacements mixed with inserts and
// removals, that earlier versions are untouched, that a replacement shares
// every subtree off the path and allocates one cell per level of the path,
// and that replacing a value with itself answers the tree itself.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <algorithm>
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

using Entries = std::vector<std::pair<proto::proto_ulong, const ProtoObject*>>;

Entries entries(ProtoContext* ctx, const ProtoSparseList* sl) {
    Entries out;
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

Entries modelEntries(const std::map<proto::proto_ulong, const ProtoObject*>& model) {
    return Entries(model.begin(), model.end());
}

const ProtoSparseListImplementation* avl(const ProtoSparseList* sl) {
    return toImpl<const ProtoSparseListImplementation>(sl);
}

} // namespace

class SparseListValueReplace : public ::testing::Test {
protected:
    ProtoSpace space;
    ProtoContext* ctx = nullptr;
    void SetUp() override { ctx = space.rootContext; }
};

// Random replacements, mixed with inserts and removals that leave empty nodes
// inside the tree, checked against a std::map model after every step.
TEST_F(SparseListValueReplace, KeepsTheTreeValidAndMatchesAModel) {
    std::mt19937_64 rng(20261018);
    for (int round = 0; round < 40; ++round) {
        std::map<proto::proto_ulong, const ProtoObject*> model;
        const ProtoSparseList* sl = ctx->newSparseList();
        const int n = 4 + static_cast<int>(rng() % 500);
        for (int i = 0; i < n; ++i) {
            const proto::proto_ulong k = 1 + rng() % 1000;
            const ProtoObject* v = ctx->fromInteger(static_cast<long long>(k));
            sl = sl->setAt(ctx, k, v);
            model[k] = v;
        }
        for (int step = 0; step < 300; ++step) {
            const int op = static_cast<int>(rng() % 10);
            if (op < 7 && !model.empty()) {          // replace a present key
                auto it = model.begin();
                std::advance(it, rng() % model.size());
                const ProtoObject* v = ctx->fromInteger(static_cast<long long>(rng() % 100000));
                sl = sl->setAt(ctx, it->first, v);
                it->second = v;
            } else if (op < 9) {                     // insert (or replace) any key
                const proto::proto_ulong k = 1 + rng() % 1200;
                const ProtoObject* v = ctx->fromInteger(static_cast<long long>(rng() % 100000));
                sl = sl->setAt(ctx, k, v);
                model[k] = v;
            } else {                                 // remove any key
                const proto::proto_ulong k = 1 + rng() % 1200;
                sl = sl->removeAt(ctx, k);
                model.erase(k);
            }
            ASSERT_EQ(entries(ctx, sl), modelEntries(model)) << "round " << round << " step " << step;
            ASSERT_EQ(sl->getSize(ctx), model.size()) << "round " << round << " step " << step;
        }
    }
}

// A replacement returns a new version; the version it was made from keeps its
// value and its entries.
TEST_F(SparseListValueReplace, LeavesTheEarlierVersionUntouched) {
    const ProtoSparseList* sl = ctx->newSparseList();
    for (proto::proto_ulong k = 1; k <= 200; ++k) sl = sl->setAt(ctx, k, ctx->fromInteger(static_cast<long long>(k)));
    const Entries before = entries(ctx, sl);

    const ProtoSparseList* replaced = sl->setAt(ctx, 137, ctx->fromInteger(-1));

    EXPECT_EQ(entries(ctx, sl), before);
    EXPECT_EQ(sl->getAt(ctx, 137)->asLong(ctx), 137);
    EXPECT_EQ(replaced->getAt(ctx, 137)->asLong(ctx), -1);
    EXPECT_EQ(replaced->getSize(ctx), sl->getSize(ctx));
}

// Every subtree off the root-to-key path is shared with the earlier version,
// and the replacement allocates exactly one cell per node on the path.
TEST_F(SparseListValueReplace, CopiesOnlyTheRootToKeyPath) {
    const ProtoSparseList* sl = ctx->newSparseList();
    for (proto::proto_ulong k = 1; k <= 1000; ++k) sl = sl->setAt(ctx, k, ctx->fromInteger(static_cast<long long>(k)));
    const proto::proto_ulong key = 777;

    proto::proto_ulong pathLength = 0;
    for (const ProtoSparseListImplementation* n = avl(sl); n && !n->isEmpty;
         n = key < n->key ? n->previous : n->next) {
        ++pathLength;
        if (n->key == key) break;
    }

    ProtoContext sub(&space, ctx, nullptr, nullptr, nullptr, nullptr);
    const ProtoObject* v = sub.fromInteger(-7);   // tagged SmallInteger: no cell
    const proto::proto_ulong before = sub.allocatedCellsCount;
    const ProtoSparseList* replaced = sl->setAt(&sub, key, v);
    const proto::proto_ulong cells = sub.allocatedCellsCount - before;
    EXPECT_EQ(cells, pathLength);

    const ProtoSparseListImplementation* o = avl(sl);
    const ProtoSparseListImplementation* r = avl(replaced);
    while (o && !o->isEmpty) {
        ASSERT_NE(o, r) << "a node on the path must be a copy";
        EXPECT_EQ(r->key, o->key);
        EXPECT_EQ(r->size, o->size);
        EXPECT_EQ(r->height, o->height);
        if (o->key == key) {
            EXPECT_EQ(r->value, v);
            EXPECT_EQ(r->previous, o->previous);
            EXPECT_EQ(r->next, o->next);
            break;
        }
        EXPECT_EQ(r->value, o->value);
        if (key < o->key) {
            EXPECT_EQ(r->next, o->next) << "subtree off the path not shared at key " << o->key;
            o = o->previous; r = r->previous;
        } else {
            EXPECT_EQ(r->previous, o->previous) << "subtree off the path not shared at key " << o->key;
            o = o->next; r = r->next;
        }
    }
}

// Replacing a value with the very same value answers the tree itself and
// allocates nothing.
TEST_F(SparseListValueReplace, TheSameValueAnswersTheTreeItself) {
    const ProtoSparseList* sl = ctx->newSparseList();
    for (proto::proto_ulong k = 1; k <= 100; ++k) sl = sl->setAt(ctx, k, ctx->fromInteger(static_cast<long long>(k)));

    ProtoContext sub(&space, ctx, nullptr, nullptr, nullptr, nullptr);
    const proto::proto_ulong before = sub.allocatedCellsCount;
    const ProtoSparseList* same = sl->setAt(&sub, 42, sl->getAt(&sub, 42));
    EXPECT_EQ(same, sl);
    EXPECT_EQ(sub.allocatedCellsCount - before, 0u);
}

// Attribute updates on objects go through the same path: a new value for an
// existing attribute keeps every other attribute and the attribute count.
TEST_F(SparseListValueReplace, ObjectAttributeUpdatesKeepTheOtherAttributes) {
    std::vector<const ProtoString*> names;
    for (int i = 0; i < 16; ++i)
        names.push_back(ProtoString::createSymbol(ctx, ("attr_" + std::to_string(i)).c_str()));
    const ProtoObject* o = ctx->newObject();
    for (int i = 0; i < 16; ++i) o = o->setAttribute(ctx, names[i], ctx->fromInteger(i));

    const ProtoObject* updated = o;
    for (int round = 0; round < 50; ++round)
        updated = updated->setAttribute(ctx, names[round % 16], ctx->fromInteger(1000 + round));

    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(o->getAttribute(ctx, names[i])->asLong(ctx), i);
        const int lastRound = 48 + i < 50 ? 48 + i : 32 + i;
        EXPECT_EQ(updated->getAttribute(ctx, names[i])->asLong(ctx), 1000 + lastRound) << "attribute " << i;
    }
    EXPECT_EQ(updated->getOwnAttributes(ctx)->getSize(ctx), o->getOwnAttributes(ctx)->getSize(ctx));
}
