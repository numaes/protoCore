// ListStructuralOpsTests.cpp — ProtoList slicing, splitting, slice removal
// and concatenation through the persistent-AVL join/split algebra
// (core/ProtoList.cpp, "Structural operations on the AVL form").
//
// Every result is checked three ways:
//   * CONTENT — element by element (pointer identity) against a naive model
//     computed on a std::vector;
//   * SHAPE — a walk over ProtoListImplementation verifying, at every node,
//     size = size(left) + size(right) + 1, height = 1 + max(heights),
//     |balance| <= 1, and that only a root may be an isEmpty cell;
//   * FORM — results of at most MAX_INLINE elements are SmallLists, longer
//     ones are AVL lists (the output-form policy of every list mutator).
//
// Also covered: nullptr elements (copied as stored by every modifier),
// tree-form inputs of at most MAX_INLINE elements, the output form of the
// single-element modifiers, collections forced between operations, and the
// cell cost of extending by a few elements.
//
// Nothing here measures time.  The cost claim (getSlice allocates O(log n)
// cells, not O(slice length)) is checked with the context's allocation
// counter, which counts cells, not nanoseconds.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace proto;

namespace {

constexpr unsigned MAX_INLINE = ProtoListSmallImplementation::MAX_INLINE;

bool isSmall(const ProtoList* l) {
    ProtoObjectPointer pa{}; pa.oid = reinterpret_cast<const ProtoObject*>(l);
    return pa.op.pointer_tag == POINTER_TAG_LIST_SMALL;
}

bool isAvl(const ProtoList* l) {
    ProtoObjectPointer pa{}; pa.oid = reinterpret_cast<const ProtoObject*>(l);
    return pa.op.pointer_tag == POINTER_TAG_LIST;
}

// Walks one AVL subtree; returns its height, or -1 after recording a failure.
int checkNode(const ProtoListImplementation* n, std::string& why) {
    if (!n) return 0;
    if (n->isEmpty) { why = "isEmpty cell below the root"; return -1; }
    const int hl = checkNode(n->previousNode, why);
    if (hl < 0) return -1;
    const int hr = checkNode(n->nextNode, why);
    if (hr < 0) return -1;
    const proto_ulong sl = n->previousNode ? n->previousNode->size : 0;
    const proto_ulong sr = n->nextNode ? n->nextNode->size : 0;
    if (n->size != sl + sr + 1) { why = "size field is wrong"; return -1; }
    if (n->height != 1 + std::max(hl, hr)) { why = "height field is wrong"; return -1; }
    if (hl - hr > 1 || hr - hl > 1) {
        why = "AVL balance violated (" + std::to_string(hl) + " vs " + std::to_string(hr) + ")";
        return -1;
    }
    return n->height;
}

// Full structural check of a list handle; empty string means OK.
std::string checkList(const ProtoList* l) {
    if (isSmall(l)) {
        const auto* s = toImpl<const ProtoListSmallImplementation>(l);
        if (s->size > MAX_INLINE) return "SmallList larger than MAX_INLINE";
        return "";
    }
    if (!isAvl(l)) return "not a list handle";
    const auto* root = toImpl<const ProtoListImplementation>(l);
    if (root->isEmpty) return root->size == 0 ? "" : "empty root with a size";
    std::string why;
    checkNode(root, why);
    return why;
}

// Canonical output form: SmallList up to MAX_INLINE elements, AVL beyond.
bool canonicalForm(ProtoContext* c, const ProtoList* l) {
    return l->getSize(c) <= MAX_INLINE ? isSmall(l) : isAvl(l);
}

// Elements as stored, through the iterator (getAt maps nullptr to PROTO_NONE).
std::vector<const ProtoObject*> rawContents(ProtoContext* c, const ProtoList* l) {
    std::vector<const ProtoObject*> out;
    for (const ProtoListIterator* it = l->getIterator(c); it->hasNext(c); it = it->advance(c))
        out.push_back(it->next(c));
    return out;
}

// A balanced tree-form list built node by node, whatever its size: the shape
// a tree mutator produced before 3.5.1 for lists of at most MAX_INLINE.
const ProtoListImplementation* treeNodes(ProtoContext* c, const ProtoObject* const* items, int n) {
    if (n <= 0) return nullptr;
    const int mid = n / 2;
    return new (c) ProtoListImplementation(c, items[mid], false, treeNodes(c, items, mid),
                                           treeNodes(c, items + mid + 1, n - mid - 1));
}
const ProtoList* treeFormList(ProtoContext* c, const std::vector<const ProtoObject*>& items) {
    if (items.empty())
        return (new (c) ProtoListImplementation(c, nullptr, true, nullptr, nullptr))->asProtoList(c);
    return treeNodes(c, items.data(), static_cast<int>(items.size()))->asProtoList(c);
}

std::vector<const ProtoObject*> contents(ProtoContext* c, const ProtoList* l) {
    std::vector<const ProtoObject*> out;
    const proto_ulong n = l->getSize(c);
    out.reserve(n);
    for (proto_ulong i = 0; i < n; ++i) out.push_back(l->getAt(c, static_cast<int>(i)));
    return out;
}

}  // namespace

class ListStructuralOpsTest : public ::testing::Test {
protected:
    ProtoSpace* space = nullptr;
    ProtoContext* c = nullptr;

    void SetUp() override {
        space = new ProtoSpace();
        c = space->rootContext;
    }
    void TearDown() override { delete space; }

    // Runs the enclosing block on a child context.  Its young generation is
    // handed to the collector when the block ends, so a loop that makes
    // hundreds of thousands of throw-away lists does not keep all of them
    // rooted on the space's root context until the test ends.
    struct Scoped {
        ListStructuralOpsTest* t;
        ProtoContext* saved;
        ProtoContext sub;
        explicit Scoped(ListStructuralOpsTest* test)
            : t(test), saved(test->c),
              sub(test->space, test->space->rootContext, nullptr, nullptr, nullptr, nullptr) {
            t->c = &sub;
        }
        ~Scoped() { t->c = saved; }
    };

    std::vector<const ProtoObject*> ints(int base, int n) {
        std::vector<const ProtoObject*> v;
        v.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) v.push_back(c->fromInteger(base + i));
        return v;
    }

    // The canonical list of `items`, through the public bulk builder.
    const ProtoList* listOf(const std::vector<const ProtoObject*>& items) {
        return c->newList(static_cast<unsigned>(items.size()), items.data());
    }

    // A large list of base, base+1, ... built by divide and conquer over
    // extend: O(n) cells in total, where newList(n, items) would take
    // O(n log n).
    const ProtoList* bigList(int base, int n) {
        if (n <= static_cast<int>(MAX_INLINE)) return listOf(ints(base, n));
        const int half = n / 2;
        return bigList(base, half)->extend(c, bigList(base + half, n - half));
    }

    // Asserts content, shape and form of `got` against `expected`.
    void expectList(const ProtoList* got, const std::vector<const ProtoObject*>& expected,
                    const std::string& what) {
        ASSERT_EQ(got->getSize(c), expected.size()) << what;
        const std::string shape = checkList(got);
        ASSERT_TRUE(shape.empty()) << what << ": " << shape;
        ASSERT_TRUE(canonicalForm(c, got)) << what << ": non-canonical output form";
        for (size_t i = 0; i < expected.size(); ++i) {
            ASSERT_EQ(got->getAt(c, static_cast<int>(i)), expected[i]) << what << " at " << i;
        }
    }

    static std::vector<const ProtoObject*> naiveSlice(const std::vector<const ProtoObject*>& v,
                                                      int from, int to) {
        const int n = static_cast<int>(v.size());
        from = std::clamp(from, 0, n);
        to = std::clamp(to, 0, n);
        if (from >= to) return {};
        return {v.begin() + from, v.begin() + to};
    }

    static std::vector<const ProtoObject*> naiveRemove(const std::vector<const ProtoObject*>& v,
                                                       int from, int to) {
        const int n = static_cast<int>(v.size());
        from = std::clamp(from, 0, n);
        to = std::clamp(to, 0, n);
        std::vector<const ProtoObject*> out(v.begin(), v.begin() + from);
        if (to > from) out.insert(out.end(), v.begin() + to, v.end());
        else out.insert(out.end(), v.begin() + from, v.end());
        return out;
    }
};

TEST_F(ListStructuralOpsTest, BuildersProduceValidTrees) {
    for (int n : {0, 1, 5, 6, 7, 64, 300, 1000, 100000}) {
        Scoped scope(this);
        const ProtoList* l = bigList(0, n);
        expectList(l, ints(0, n), "bigList n=" + std::to_string(n));
    }
}

TEST_F(ListStructuralOpsTest, EverySplitPointUpTo300) {
    for (int n = 0; n <= 300; ++n) {
        Scoped scope(this);
        const auto items = ints(0, n);
        const ProtoList* l = listOf(items);
        for (int k = 0; k <= n; ++k) {
            const std::string tag = "n=" + std::to_string(n) + " k=" + std::to_string(k);
            expectList(l->splitFirst(c, k), naiveSlice(items, 0, k), "splitFirst " + tag);
            expectList(l->splitLast(c, k), naiveSlice(items, n - k, n), "splitLast " + tag);
            expectList(l->getSlice(c, 0, k), naiveSlice(items, 0, k), "getSlice prefix " + tag);
            expectList(l->getSlice(c, k, n), naiveSlice(items, k, n), "getSlice suffix " + tag);
            expectList(l->removeSlice(c, 0, k), naiveRemove(items, 0, k), "removeSlice prefix " + tag);
            expectList(l->removeSlice(c, k, n), naiveRemove(items, k, n), "removeSlice suffix " + tag);
        }
    }
}

TEST_F(ListStructuralOpsTest, EveryRangeOnSmallSizes) {
    for (int n = 0; n <= 40; ++n) {
        Scoped scope(this);
        const auto items = ints(0, n);
        const ProtoList* l = listOf(items);
        for (int from = -2; from <= n + 2; ++from) {
            for (int to = -2; to <= n + 2; ++to) {
                const std::string tag = "n=" + std::to_string(n) + " [" + std::to_string(from) +
                                        "," + std::to_string(to) + ")";
                expectList(l->getSlice(c, from, to), naiveSlice(items, from, to), "getSlice " + tag);
                expectList(l->removeSlice(c, from, to), naiveRemove(items, from, to),
                           "removeSlice " + tag);
            }
        }
    }
}

TEST_F(ListStructuralOpsTest, RandomRangesUpTo300) {
    std::mt19937 rng(20261110u);
    for (int n = 0; n <= 300; ++n) {
        Scoped scope(this);
        const auto items = ints(0, n);
        const ProtoList* l = listOf(items);
        std::uniform_int_distribution<int> pick(-3, n + 3);
        for (int trial = 0; trial < 40; ++trial) {
            const int from = pick(rng);
            const int to = pick(rng);
            const std::string tag = "n=" + std::to_string(n) + " [" + std::to_string(from) +
                                    "," + std::to_string(to) + ")";
            expectList(l->getSlice(c, from, to), naiveSlice(items, from, to), "getSlice " + tag);
            expectList(l->removeSlice(c, from, to), naiveRemove(items, from, to),
                       "removeSlice " + tag);
        }
    }
}

TEST_F(ListStructuralOpsTest, RandomRangesOnLargeList) {
    constexpr int N = 100000;
    const auto items = ints(0, N);
    const ProtoList* l = bigList(0, N);
    std::mt19937 rng(42u);
    std::uniform_int_distribution<int> pick(0, N);
    for (int trial = 0; trial < 30; ++trial) {
        int from = pick(rng);
        int to = pick(rng);
        if (from > to) std::swap(from, to);
        const std::string tag = "[" + std::to_string(from) + "," + std::to_string(to) + ")";
        expectList(l->getSlice(c, from, to), naiveSlice(items, from, to), "getSlice " + tag);
        expectList(l->removeSlice(c, from, to), naiveRemove(items, from, to), "removeSlice " + tag);
    }
    // The source list is untouched by all of the above.
    expectList(l, items, "source after slicing");
}

TEST_F(ListStructuralOpsTest, ExtendAcrossSizesAndHeights) {
    const int sizes[] = {0, 1, 2, 4, 5, 6, 7, 31, 64, 300, 5000};
    for (int a : sizes) {
        for (int b : sizes) {
            Scoped scope(this);
            const auto left = ints(0, a);
            const auto right = ints(1000000, b);
            std::vector<const ProtoObject*> both(left);
            both.insert(both.end(), right.begin(), right.end());
            const ProtoList* la = a > 300 ? bigList(0, a) : listOf(left);
            const ProtoList* lb = b > 300 ? bigList(1000000, b) : listOf(right);
            expectList(la->extend(c, lb), both,
                       "extend " + std::to_string(a) + "+" + std::to_string(b));
            // Operands unchanged.
            expectList(la, left, "left operand");
            expectList(lb, right, "right operand");
        }
    }
}

TEST_F(ListStructuralOpsTest, ExtendVeryDifferentHeights) {
    constexpr int N = 100000;
    const ProtoList* big = bigList(0, N);
    for (int small : {1, 6, 17, 1000}) {
        const ProtoList* s = bigList(-small, small);
        std::vector<const ProtoObject*> expectedA = ints(0, N);
        const auto tail = ints(-small, small);
        expectedA.insert(expectedA.end(), tail.begin(), tail.end());
        expectList(big->extend(c, s), expectedA, "big+small " + std::to_string(small));

        std::vector<const ProtoObject*> expectedB = ints(-small, small);
        const auto body = ints(0, N);
        expectedB.insert(expectedB.end(), body.begin(), body.end());
        expectList(s->extend(c, big), expectedB, "small+big " + std::to_string(small));
    }
}

TEST_F(ListStructuralOpsTest, RemoveSliceThenExtendRestoresContent) {
    constexpr int N = 5000;
    const auto items = ints(0, N);
    const ProtoList* l = bigList(0, N);
    for (int from : {0, 1, 7, 2500, 4999}) {
        for (int len : {1, 5, 6, 100, 2500}) {
            const int to = std::min(N, from + len);
            const ProtoList* rebuilt = l->splitFirst(c, from)
                                           ->extend(c, l->getSlice(c, from, to))
                                           ->extend(c, l->getSlice(c, to, N));
            expectList(rebuilt, items, "rebuilt " + std::to_string(from) + "+" + std::to_string(len));
        }
    }
}

TEST_F(ListStructuralOpsTest, NullAndEmptyArguments) {
    const ProtoList* l = listOf(ints(0, 10));
    EXPECT_EQ(l->extend(c, nullptr), l);
    EXPECT_EQ(l->extend(c, c->newList()), l);
    expectList(c->newList()->extend(c, l), ints(0, 10), "empty+l");
    EXPECT_EQ(l->getSlice(c, 0, 10), l);   // full range: the list itself
    EXPECT_EQ(l->removeSlice(c, 4, 4), l); // empty range: the list itself
    EXPECT_EQ(l->removeSlice(c, 9, 2), l);
    expectList(l->getSlice(c, 7, 3), {}, "reversed range");
}

// getSlice of a large list costs O(log n) cells, not O(slice length).  The
// allocation counter is read inside one call that holds a critical section
// and never reaches safepoint(), the only thing that resets the counter.
TEST_F(ListStructuralOpsTest, GetSliceAllocatesLogarithmicCells) {
    constexpr int N = 1 << 17;
    const ProtoList* l = bigList(0, N);
    const proto_ulong before = c->allocatedCellsCount;
    const ProtoList* mid = l->getSlice(c, N / 4, 3 * N / 4);
    const proto_ulong after = c->allocatedCellsCount;
    ASSERT_GE(after, before) << "allocation counter was reset during the call";
    // log2(N) = 17; join/split path copies a bounded number of nodes per
    // tree level (at most six, in a double rotation).  The old
    // element-by-element build took about N/2 * 17 cells, over a million.
    EXPECT_LT(after - before, 1000u) << "getSlice allocated " << (after - before) << " cells";
    std::printf("[ListStructuralOps] getSlice of %d of %d elements: %llu cells\n", N / 2, N,
                static_cast<unsigned long long>(after - before));
    EXPECT_EQ(mid->getSize(c), static_cast<proto_ulong>(N / 2));

    const proto_ulong before2 = c->allocatedCellsCount;
    const ProtoList* rest = l->removeSlice(c, N / 3, 2 * N / 3);
    const proto_ulong after2 = c->allocatedCellsCount;
    ASSERT_GE(after2, before2);
    EXPECT_LT(after2 - before2, 1500u) << "removeSlice allocated " << (after2 - before2) << " cells";
    std::printf("[ListStructuralOps] removeSlice of %d of %d elements: %llu cells\n",
                2 * N / 3 - N / 3, N, static_cast<unsigned long long>(after2 - before2));
    EXPECT_EQ(rest->getSize(c), static_cast<proto_ulong>(N - (2 * N / 3 - N / 3)));
}

// Sanity at a million elements: slicing the middle half completes and yields
// the right elements.  No timing assertion; at O(k log n) this test used to
// build a half-million-element list one append at a time.
TEST_F(ListStructuralOpsTest, MillionElementMiddleHalf) {
    constexpr int N = 1000000;
    const ProtoList* l = bigList(0, N);
    ASSERT_TRUE(checkList(l).empty());
    const ProtoList* mid = l->getSlice(c, N / 4, 3 * N / 4);
    ASSERT_EQ(mid->getSize(c), static_cast<proto_ulong>(N / 2));
    ASSERT_TRUE(checkList(mid).empty()) << checkList(mid);
    for (int i : {0, 1, 12345, N / 4 - 1, N / 2 - 1}) {
        EXPECT_EQ(mid->getAt(c, i), c->fromInteger(N / 4 + i)) << "at " << i;
    }
}

// Every modifier copies stored elements as they are: a nullptr element stays
// nullptr on the inline path and on the tree path alike.
TEST_F(ListStructuralOpsTest, NullElementsAreCopiedAsStored) {
    for (int n : {2, 3, 5, 6, 9, 40}) {
        Scoped scope(this);
        std::vector<const ProtoObject*> items = ints(0, n);
        items[0] = nullptr;
        items[static_cast<size_t>(n) / 2] = nullptr;
        const ProtoList* l = listOf(items);
        const std::string tag = "n=" + std::to_string(n);

        std::vector<const ProtoObject*> twice(items);
        twice.insert(twice.end(), items.begin(), items.end());
        EXPECT_EQ(rawContents(c, l->extend(c, l)), twice) << "extend " << tag;
        EXPECT_EQ(rawContents(c, l->getSlice(c, 0, n - 1)), naiveSlice(items, 0, n - 1)) << "getSlice " << tag;
        EXPECT_EQ(rawContents(c, l->removeSlice(c, 1, 2)), naiveRemove(items, 1, 2)) << "removeSlice " << tag;
        EXPECT_EQ(rawContents(c, l->splitLast(c, n - 1)), naiveSlice(items, 1, n)) << "splitLast " << tag;
        // A one-element list holding nullptr, on either side of a tree.
        const ProtoObject* none[1] = {nullptr};
        const ProtoList* one = c->newList(1, none);
        std::vector<const ProtoObject*> withTail(items);
        withTail.push_back(nullptr);
        EXPECT_EQ(rawContents(c, l->extend(c, one)), withTail) << "extend(one) " << tag;
        std::vector<const ProtoObject*> withHead{nullptr};
        withHead.insert(withHead.end(), items.begin(), items.end());
        EXPECT_EQ(rawContents(c, one->extend(c, l)), withHead) << "one->extend " << tag;
    }
}

// Tree-form lists of at most MAX_INLINE elements (what removeAt produced
// before 3.5.1) are valid inputs to every operation, and every modifier
// returns the canonical form of its result.
TEST_F(ListStructuralOpsTest, TreeFormInputsOfAtMostFiveElements) {
    for (int n = 0; n <= static_cast<int>(MAX_INLINE); ++n) {
        Scoped scope(this);
        const auto items = ints(0, n);
        const ProtoList* t = treeFormList(c, items);
        ASSERT_TRUE(isAvl(t));
        const std::string tag = "n=" + std::to_string(n);
        for (int from = -1; from <= n + 1; ++from) {
            for (int to = -1; to <= n + 1; ++to) {
                const std::string r = tag + " [" + std::to_string(from) + "," + std::to_string(to) + ")";
                const ProtoList* sl = t->getSlice(c, from, to);
                const auto expectedSlice = naiveSlice(items, from, to);
                if (static_cast<int>(expectedSlice.size()) == n) {
                    EXPECT_EQ(rawContents(c, sl), items) << "getSlice full " << r;   // the receiver
                } else {
                    expectList(sl, expectedSlice, "getSlice " + r);
                }
                const auto expectedRemove = naiveRemove(items, from, to);
                const ProtoList* rm = t->removeSlice(c, from, to);
                if (static_cast<int>(expectedRemove.size()) == n) {
                    EXPECT_EQ(rm, t) << "removeSlice empty range " << r;
                } else {
                    expectList(rm, expectedRemove, "removeSlice " + r);
                }
            }
        }
        const auto more = ints(100, 3);
        std::vector<const ProtoObject*> joined(items);
        joined.insert(joined.end(), more.begin(), more.end());
        expectList(t->extend(c, listOf(more)), joined, "extend " + tag);
        std::vector<const ProtoObject*> joined2(more);
        joined2.insert(joined2.end(), items.begin(), items.end());
        expectList(listOf(more)->extend(c, t), joined2, "small->extend(tree) " + tag);
        if (n == 0) {
            EXPECT_EQ(t->extend(c, t), t);   // extending by nothing returns the receiver
        } else {
            expectList(t->extend(c, t),
                       [&] { auto v = items; v.insert(v.end(), items.begin(), items.end()); return v; }(),
                       "self extend " + tag);
        }
    }
}

// The single-element modifiers pick the output form by the result's size,
// like every other modifier; out-of-range indices return the receiver.
TEST_F(ListStructuralOpsTest, SingleElementModifiersPickTheFormBySize) {
    const auto six = ints(0, 6);
    const ProtoList* l6 = listOf(six);
    ASSERT_TRUE(isAvl(l6));
    for (int i = 0; i < 6; ++i) {
        auto expected = six;
        expected.erase(expected.begin() + i);
        expectList(l6->removeAt(c, i), expected, "removeAt " + std::to_string(i));
    }
    expectList(l6->removeFirst(c), naiveSlice(six, 1, 6), "removeFirst");
    expectList(l6->removeLast(c), naiveSlice(six, 0, 5), "removeLast");
    EXPECT_EQ(l6->removeAt(c, 6), l6);
    EXPECT_EQ(l6->removeAt(c, -1), l6);
    EXPECT_EQ(l6->setAt(c, 6, c->fromInteger(9)), l6);
    EXPECT_EQ(l6->setAt(c, -1, c->fromInteger(9)), l6);

    for (int n = 0; n <= static_cast<int>(MAX_INLINE); ++n) {
        const auto items = ints(0, n);
        const ProtoList* t = treeFormList(c, items);
        const std::string tag = "tree n=" + std::to_string(n);
        if (n > 0) {
            auto expected = items;
            expected[0] = c->fromInteger(-7);
            expectList(t->setAt(c, 0, c->fromInteger(-7)), expected, "setAt " + tag);
            expectList(t->removeAt(c, n - 1), naiveSlice(items, 0, n - 1), "removeAt " + tag);
        }
        auto inserted = items;
        inserted.insert(inserted.begin(), c->fromInteger(-1));
        expectList(t->insertAt(c, 0, c->fromInteger(-1)), inserted, "insertAt " + tag);
        auto appended = items;
        appended.push_back(c->fromInteger(-2));
        expectList(t->appendLast(c, c->fromInteger(-2)), appended, "appendLast " + tag);
    }
    // Growing one element at a time crosses MAX_INLINE in canonical form.
    const ProtoList* g = c->newList();
    std::vector<const ProtoObject*> model;
    for (int i = 0; i < 20; ++i) {
        g = g->appendLast(c, c->fromInteger(i));
        model.push_back(c->fromInteger(i));
        expectList(g, model, "append " + std::to_string(i));
    }
    for (int i = 19; i >= 0; --i) {
        g = g->removeAt(c, 0);
        model.erase(model.begin());
        expectList(g, model, "shrink " + std::to_string(i));
    }
}

TEST_F(ListStructuralOpsTest, ExtremeIndicesClampWithoutOverflow) {
    const auto items = ints(0, 50);
    const ProtoList* l = listOf(items);
    EXPECT_EQ(l->getSlice(c, INT_MIN, INT_MAX), l);
    expectList(l->getSlice(c, INT_MIN, 10), naiveSlice(items, 0, 10), "INT_MIN..10");
    expectList(l->getSlice(c, 40, INT_MAX), naiveSlice(items, 40, 50), "40..INT_MAX");
    expectList(l->getSlice(c, INT_MAX, INT_MIN), {}, "reversed extremes");
    expectList(l->removeSlice(c, INT_MIN, INT_MAX), {}, "remove everything");
    EXPECT_EQ(l->removeSlice(c, INT_MAX, INT_MAX), l);
    EXPECT_EQ(l->splitFirst(c, INT_MAX), l);
    EXPECT_EQ(l->splitLast(c, INT_MAX), l);
    expectList(l->splitFirst(c, INT_MIN), {}, "splitFirst INT_MIN");
}

// Extending by a few elements costs about what appending them costs: the
// middle element of the join comes from the shorter tree.
TEST_F(ListStructuralOpsTest, ExtendByFewElementsCostsAboutAnAppend) {
    for (int N : {1536, 1 << 17}) {
        Scoped scope(this);
        const ProtoList* big = bigList(0, N);
        const ProtoObject* one[1] = {c->fromInteger(-1)};
        const ProtoObject* three[3] = {c->fromInteger(-1), c->fromInteger(-2), c->fromInteger(-3)};
        const ProtoList* s1 = c->newList(1, one);
        const ProtoList* s3 = c->newList(3, three);
        // Every call holds a critical section and never reaches safepoint().
        auto cells = [&](auto&& op) {
            const proto_ulong before = c->allocatedCellsCount;
            op();
            const proto_ulong after = c->allocatedCellsCount;
            EXPECT_GE(after, before) << "allocation counter was reset";
            return after - before;
        };
        const proto_ulong append = cells([&] { big->appendLast(c, one[0]); });
        const proto_ulong prepend = cells([&] { big->appendFirst(c, one[0]); });
        const proto_ulong ext1 = cells([&] { big->extend(c, s1); });
        const proto_ulong ext3 = cells([&] { big->extend(c, s3); });
        const proto_ulong pre1 = cells([&] { s1->extend(c, big); });
        const proto_ulong pre3 = cells([&] { s3->extend(c, big); });
        std::printf("[ListStructuralOps] N=%d appendLast=%llu appendFirst=%llu extend(1)=%llu "
                    "extend(3)=%llu 1+big=%llu 3+big=%llu cells\n", N, (unsigned long long)append,
                    (unsigned long long)prepend,
                    (unsigned long long)ext1, (unsigned long long)ext3,
                    (unsigned long long)pre1, (unsigned long long)pre3);
        // Appending at the end compares with appendLast, prepending with
        // appendFirst; the slack covers a rotation and the small operand's
        // own nodes.
        EXPECT_LE(ext1, append + 4) << "N=" << N;
        EXPECT_LE(ext3, append + 6) << "N=" << N;
        EXPECT_LE(pre1, prepend + 4) << "N=" << N;
        EXPECT_LE(pre3, prepend + 6) << "N=" << N;
    }
}

// Lists kept only in a root set survive collections forced between
// structural operations (each iteration runs on a child context, whose young
// generation is handed to the collector when it ends).
TEST_F(ListStructuralOpsTest, StructuralOperationsSurviveForcedCollections) {
    ProtoRootSet* roots = space->createRootSet("list-structural-ops");
    std::vector<long long> model(3000);
    for (int i = 0; i < 3000; ++i) model[static_cast<size_t>(i)] = i;
    auto handle = [&] {
        ProtoContext sub(space, space->rootContext, nullptr, nullptr, nullptr, nullptr);
        c = &sub;
        const ProtoList* l = bigList(0, 3000);
        auto h = roots->add(l->asObject(&sub));
        c = space->rootContext;
        return h;
    }();
    std::mt19937 rng(11u);
    for (int it = 0; it < 200; ++it) {
        {
            ProtoContext sub(space, space->rootContext, nullptr, nullptr, nullptr, nullptr);
            const ProtoList* cur = roots->resolve(handle)->asList(&sub);
            const int n = static_cast<int>(model.size());
            int a = static_cast<int>(rng() % static_cast<unsigned>(n + 1));
            int b = static_cast<int>(rng() % static_cast<unsigned>(n + 1));
            if (a > b) std::swap(a, b);
            const ProtoList* mid = cur->getSlice(&sub, a, b);
            for (int g = 0; g < 2000; ++g) (void) sub.newList()->appendLast(&sub, sub.fromInteger(g));
            const ProtoList* next = cur->removeSlice(&sub, a, b)->extend(&sub, mid);
            std::vector<long long> m(model.begin(), model.begin() + a);
            m.insert(m.end(), model.begin() + b, model.end());
            m.insert(m.end(), model.begin() + a, model.begin() + b);
            model = m;
            auto h2 = roots->add(next->asObject(&sub));
            roots->remove(handle);
            handle = h2;
        }
        space->triggerGC();
        ProtoContext sub(space, space->rootContext, nullptr, nullptr, nullptr, nullptr);
        const ProtoList* cur = roots->resolve(handle)->asList(&sub);
        ASSERT_EQ(cur->getSize(&sub), model.size());
        ASSERT_TRUE(checkList(cur).empty()) << checkList(cur);
        if (it % 20 == 0) {
            for (size_t i = 0; i < model.size(); ++i)
                ASSERT_EQ(cur->getAt(&sub, static_cast<int>(i))->asLong(&sub), model[i]) << i;
        }
    }
    roots->remove(handle);
}

// A random walk mixing every modifier: trees shaped by insertAt / removeAt
// rebalancing as well as by join, sizes crossing MAX_INLINE both ways,
// self-concatenation and rotation by splitFirst / splitLast + extend.
TEST_F(ListStructuralOpsTest, RandomWalkOverEveryModifier) {
    std::mt19937 rng(7u);
    for (int round = 0; round < 12; ++round) {
        Scoped scope(this);
        std::vector<const ProtoObject*> model;
        const ProtoList* l = c->newList();
        for (int step = 0; step < 1000; ++step) {
            const int op = static_cast<int>(rng() % 7);
            const int n = static_cast<int>(model.size());
            auto pick = [&](int lo, int hi) {
                return lo + static_cast<int>(rng() % static_cast<unsigned>(hi - lo + 1));
            };
            if (op <= 1 || n < 3) {
                const int k = pick(1, 40);
                for (int i = 0; i < k; ++i) {
                    const int at = pick(0, static_cast<int>(model.size()));
                    const ProtoObject* v = c->fromInteger(round * 100000 + step * 50 + i);
                    l = l->insertAt(c, at, v);
                    model.insert(model.begin() + at, v);
                }
            } else if (op == 2) {
                const int at = pick(0, n - 1);
                l = l->removeAt(c, at);
                model.erase(model.begin() + at);
            } else if (op == 3) {
                const int a = pick(-2, n + 2), b = pick(-2, n + 2);
                l = l->getSlice(c, a, b);
                model = naiveSlice(model, a, b);
            } else if (op == 4) {
                const int a = pick(-2, n + 2), b = pick(-2, n + 2);
                l = l->removeSlice(c, a, b);
                model = naiveRemove(model, a, b);
            } else if (op == 5) {
                l = l->extend(c, l);
                const auto copy = model;
                model.insert(model.end(), copy.begin(), copy.end());
                if (model.size() > 20000) {
                    l = l->splitFirst(c, 100);
                    model.resize(100);
                }
            } else {
                const int k = pick(0, n);
                l = l->splitLast(c, n - k)->extend(c, l->splitFirst(c, k));
                std::rotate(model.begin(), model.begin() + k, model.end());
            }
            ASSERT_EQ(l->getSize(c), model.size()) << "round " << round << " step " << step << " op " << op;
            ASSERT_TRUE(checkList(l).empty()) << checkList(l) << " op " << op;
            ASSERT_TRUE(canonicalForm(c, l)) << "op " << op;
            if (step % 25 == 0) ASSERT_EQ(rawContents(c, l), model) << "op " << op;
        }
    }
}
