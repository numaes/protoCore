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
// Nothing here measures time.  The cost claim (getSlice allocates O(log n)
// cells, not O(slice length)) is checked with the context's allocation
// counter, which counts cells, not nanoseconds.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <algorithm>
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
