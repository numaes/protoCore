// ListRrbTests.cpp — the RRB form of ProtoList (core/ProtoListRrb.cpp,
// docs/LIST_RRB_DESIGN.md).
//
// Every list of more than MAX_INLINE elements that no AVL operand produced is
// an RRB tree.  Every check compares with a std::vector model, element by
// element as stored (nullptr included), and checks the structure of every
// list it sees (ListRrbCheck.h).  Besides correctness, two cost properties are
// guarded here because the first attempt (a trie that converted to the AVL
// form for structural operations) broke them without failing any
// correctness test: deriving many versions from ONE list costs a bounded
// number of cells per version, and adversarial build orders keep the height
// near the optimum.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"
#include "ListRrbCheck.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace proto;
using proto_test::checkRrb;
using proto_test::isRrb;
using proto_test::rrbOf;
using proto_test::RrbStats;

namespace {

constexpr unsigned MAX_INLINE = ProtoListSmallImplementation::MAX_INLINE;
using Model = std::vector<const ProtoObject*>;

bool isSmall(const ProtoList* l) {
    ProtoObjectPointer pa{}; pa.oid = reinterpret_cast<const ProtoObject*>(l);
    return pa.op.pointer_tag == POINTER_TAG_LIST_SMALL;
}

// Elements as stored, through the iterator (getAt maps nullptr to PROTO_NONE).
Model rawContents(ProtoContext* c, const ProtoList* l) {
    Model out;
    for (const ProtoListIterator* it = l->getIterator(c); it->hasNext(c); it = it->advance(c))
        out.push_back(it->next(c));
    return out;
}

// Form and structure: SmallList up to MAX_INLINE, a well-formed RRB beyond.
std::string checkForm(ProtoContext* c, const ProtoList* l) {
    const proto_ulong n = l->getSize(c);
    if (n <= MAX_INLINE) return isSmall(l) ? "" : "list of at most MAX_INLINE elements is not a SmallList";
    const auto* r = rrbOf(l);
    if (!r) return "list larger than MAX_INLINE is not RRB";
    return checkRrb(r);
}

void expectEqual(ProtoContext* c, const ProtoList* l, const Model& m, const std::string& what) {
    ASSERT_EQ(l->getSize(c), m.size()) << what;
    const std::string form = checkForm(c, l);
    ASSERT_TRUE(form.empty()) << what << ": " << form;
    for (size_t i = 0; i < m.size(); ++i) {
        const ProtoObject* want = m[i] ? m[i] : PROTO_NONE;
        ASSERT_EQ(l->getAt(c, static_cast<int>(i)), want) << what << " at " << i;
    }
    ASSERT_EQ(rawContents(c, l), m) << what << " (iterator)";
}

const ProtoList* listOf(ProtoContext* c, const Model& m) {
    const ProtoList* l = c->newList();
    for (const ProtoObject* v : m) l = l->appendLast(c, v);
    return l;
}

// A list of the model's elements built by concatenating pieces of 1..40
// elements: relaxed nodes everywhere.
const ProtoList* relaxedListOf(ProtoContext* c, const Model& m, std::mt19937& rng) {
    const ProtoList* l = c->newList();
    size_t at = 0;
    while (at < m.size()) {
        const size_t k = std::min<size_t>(m.size() - at, 1 + rng() % 40);
        l = l->extend(c, listOf(c, Model(m.begin() + at, m.begin() + at + k)));
        at += k;
    }
    return l;
}

// A mix of element kinds: SmallIntegers, nullptr, and cells (strings and objects).
const ProtoObject* elementFor(ProtoContext* c, long long i) {
    switch (i % 7) {
        case 3: return nullptr;
        case 5: return ProtoString::fromUTF8(c, ("s" + std::to_string(i)).c_str())->asObject(c);
        case 6: return c->newObject(false);
        default: return c->fromInteger(i);
    }
}

unsigned optimalHeight(proto_ulong treeElements) {
    unsigned h = 0; proto_ulong cap = 6;
    while (cap < treeElements) { cap *= 6; ++h; }
    return h;
}

}  // namespace

class ListRrbTest : public ::testing::Test {
protected:
    ProtoSpace* space = nullptr;
    ProtoContext* c = nullptr;
    void SetUp() override { space = new ProtoSpace(); c = space->rootContext; }
    void TearDown() override { delete space; }
};

// Appends from the empty list across every change of form and of height
// (5 -> 6 -> 7, 36 -> 37, 216 -> 217, 1296 -> 1297), then removeLast all the
// way down; every size is checked and every version stays intact.
TEST_F(ListRrbTest, GrowsAcrossEveryBorderAndShrinksBack) {
    ProtoContext sub(space, c, nullptr, nullptr, nullptr, nullptr);
    const int N = 1400;
    Model m;
    std::vector<const ProtoList*> versions;
    const ProtoList* l = sub.newList();
    versions.push_back(l);
    for (int i = 0; i < N; ++i) {
        const ProtoObject* v = elementFor(&sub, i);
        l = l->appendLast(&sub, v);
        m.push_back(v);
        versions.push_back(l);
        ASSERT_EQ(isRrb(l), m.size() > MAX_INLINE) << "size " << m.size();
        const std::string form = checkForm(&sub, l);
        ASSERT_TRUE(form.empty()) << "size " << m.size() << ": " << form;
        if (m.size() > MAX_INLINE) {
            RrbStats st; checkRrb(rrbOf(l), &st);
            ASSERT_EQ(st.relaxedNodes, 0u) << "a list built by appends has only strict nodes";
        }
    }
    expectEqual(&sub, l, m, "built to 1400");
    for (int k : {0, 1, 5, 6, 7, 36, 37, 216, 217, 1296, 1297}) {
        expectEqual(&sub, versions[k], Model(m.begin(), m.begin() + k), "version " + std::to_string(k));
    }
    while (!m.empty()) {
        l = l->removeLast(&sub);
        m.pop_back();
        const std::string form = checkForm(&sub, l);
        ASSERT_TRUE(form.empty()) << "size " << m.size() << ": " << form;
        if (m.size() % 37 == 0 || m.size() < 10) expectEqual(&sub, l, m, "shrunk to " + std::to_string(m.size()));
    }
}

// Random walk over every list operation against the model, from lists built
// by appends and by concatenation; every version kept along the way is
// checked again at the end.
TEST_F(ListRrbTest, RandomWalkOverEveryOperation) {
    for (unsigned seed : {1u, 2u, 3u, 4u}) {
        std::mt19937 rng(seed);
        ProtoContext sub(space, c, nullptr, nullptr, nullptr, nullptr);
        long long next = 1;
        auto fresh = [&]() { return elementFor(&sub, next++); };
        auto build = [&](size_t n, Model& m) {
            m.clear(); for (size_t i = 0; i < n; ++i) m.push_back(fresh());
            return rng() % 2 ? listOf(&sub, m) : relaxedListOf(&sub, m, rng);
        };
        std::vector<std::pair<const ProtoList*, Model>> kept;
        Model m;
        const ProtoList* l = build(rng() % 2000, m);
        for (int step = 0; step < 900; ++step) {
            const int n = static_cast<int>(m.size());
            auto pick = [&](int lo, int hi) { return lo + static_cast<int>(rng() % static_cast<unsigned>(hi - lo + 1)); };
            const int op = pick(0, 15);
            const std::string what = "seed " + std::to_string(seed) + " step " + std::to_string(step)
                                   + " op " + std::to_string(op) + " n " + std::to_string(n);
            const ProtoObject* v = fresh();
            switch (op) {
                case 0: case 1: l = l->appendLast(&sub, v); m.push_back(v); break;
                case 2: if (n) { l = l->removeLast(&sub); m.pop_back(); } break;
                case 3: if (n) { int i = pick(0, n - 1); l = l->setAt(&sub, i, v); m[i] = v; } break;
                case 4: { int i = pick(0, n); l = l->insertAt(&sub, i, v); m.insert(m.begin() + i, v); } break;
                case 5: if (n) { int i = pick(0, n - 1); l = l->removeAt(&sub, i); m.erase(m.begin() + i); } break;
                case 6: l = l->appendFirst(&sub, v); m.insert(m.begin(), v); break;
                case 7: if (n) { l = l->removeFirst(&sub); m.erase(m.begin()); } break;
                case 8: { Model o; const ProtoList* p = build(rng() % 4 ? pick(0, 60) : pick(0, 1500), o);
                          if (rng() % 2) { l = l->extend(&sub, p); m.insert(m.end(), o.begin(), o.end()); }
                          else { l = p->extend(&sub, l); m.insert(m.begin(), o.begin(), o.end()); } } break;
                case 9: { int a = pick(-2, n + 2), b = pick(-2, n + 2); if (a > b) std::swap(a, b);
                          const int ca = std::clamp(a, 0, n), cb = std::clamp(b, 0, n);
                          expectEqual(&sub, l->getSlice(&sub, a, b), Model(m.begin() + ca, m.begin() + std::max(ca, cb)), what + " slice"); } break;
                case 10: { int a = pick(0, n), b = pick(0, n); if (a > b) std::swap(a, b);
                           l = l->removeSlice(&sub, a, b); m.erase(m.begin() + a, m.begin() + b); } break;
                case 11: { int k = pick(0, n);
                           if (rng() % 2) { l = l->splitFirst(&sub, k); m.resize(k); }
                           else { l = l->splitLast(&sub, k); m.erase(m.begin(), m.begin() + (n - k)); } } break;
                case 12: for (int q = 0; q < 30; ++q) { const ProtoObject* w = fresh(); int i = pick(0, static_cast<int>(m.size()));
                             l = l->insertAt(&sub, i, w); m.insert(m.begin() + i, w); } break;
                case 13: for (int q = 0; q < 30 && !m.empty(); ++q) { int i = pick(0, static_cast<int>(m.size()) - 1);
                             l = l->removeAt(&sub, i); m.erase(m.begin() + i); } break;
                case 14: if (n > 0 && n < 400) { int k = pick(0, 4); l = l->multiply(&sub, sub.fromInteger(k));
                             Model mm; for (int r = 0; r < k; ++r) mm.insert(mm.end(), m.begin(), m.end()); m = mm; } break;
                case 15: for (int q = 0; q < 20; ++q) { const ProtoObject* w = fresh(); l = l->appendFirst(&sub, w); m.insert(m.begin(), w); } break;
            }
            if (m.size() > 5000) { l = l->splitFirst(&sub, 2000); m.resize(2000); }
            if (step % 5 == 0 || m.size() < 40) {
                expectEqual(&sub, l, m, what);
            } else {
                const std::string form = checkForm(&sub, l);
                ASSERT_TRUE(form.empty()) << what << ": " << form;
                ASSERT_EQ(l->getSize(&sub), m.size()) << what;
            }
            if (::testing::Test::HasFatalFailure()) return;
            if (step % 60 == 0) kept.emplace_back(l, m);
        }
        for (size_t k = 0; k < kept.size(); ++k) expectEqual(&sub, kept[k].first, kept[k].second, "kept version " + std::to_string(k));
    }
}

// Results pick their form by size: SmallList up to MAX_INLINE, RRB beyond;
// out-of-range indices return the receiver.
TEST_F(ListRrbTest, OutputFormsBySize) {
    ProtoContext sub(space, c, nullptr, nullptr, nullptr, nullptr);
    Model m; for (int i = 0; i < 100; ++i) m.push_back(sub.fromInteger(i));
    const ProtoList* t = listOf(&sub, m);
    ASSERT_TRUE(isRrb(t));
    for (const ProtoList* r : {t->appendLast(&sub, PROTO_NONE), t->setAt(&sub, 3, PROTO_NONE), t->removeLast(&sub),
                               t->insertAt(&sub, 50, PROTO_NONE), t->appendFirst(&sub, PROTO_NONE), t->removeAt(&sub, 10),
                               t->removeFirst(&sub), t->getSlice(&sub, 10, 90), t->extend(&sub, t), t->removeSlice(&sub, 3, 50),
                               t->multiply(&sub, sub.fromInteger(3))})
        EXPECT_TRUE(isRrb(r));
    EXPECT_TRUE(isSmall(t->getSlice(&sub, 10, 15)));
    EXPECT_TRUE(isSmall(t->removeSlice(&sub, 2, 97)));
    EXPECT_TRUE(isSmall(t->splitLast(&sub, 5)));
    EXPECT_EQ(t->getSlice(&sub, 0, 100), t);
    EXPECT_EQ(t->setAt(&sub, 100, PROTO_NONE), t);
    EXPECT_EQ(t->setAt(&sub, -1, PROTO_NONE), t);
    EXPECT_EQ(t->removeAt(&sub, 100), t);
    EXPECT_EQ(t->getAt(&sub, 100), PROTO_NONE);
    EXPECT_EQ(t->getAt(&sub, -1), PROTO_NONE);
    const ProtoList* six = listOf(&sub, Model(m.begin(), m.begin() + 6));
    ASSERT_TRUE(isRrb(six));
    expectEqual(&sub, six->removeLast(&sub), Model(m.begin(), m.begin() + 5), "six minus last");
    expectEqual(&sub, six->removeFirst(&sub), Model(m.begin() + 1, m.begin() + 6), "six minus first");
    EXPECT_TRUE(t->has(&sub, sub.fromInteger(0)));
    EXPECT_TRUE(t->has(&sub, sub.fromInteger(99)));
    EXPECT_FALSE(t->has(&sub, sub.fromInteger(100)));
}

// The iterator reads the same elements as getAt on strict and relaxed lists.
TEST_F(ListRrbTest, IteratorMatchesTheModel) {
    ProtoContext sub(space, c, nullptr, nullptr, nullptr, nullptr);
    std::mt19937 rng(8u);
    Model m;
    const ProtoList* l = sub.newList();
    for (int n = 1; n <= 1300; ++n) {
        const ProtoObject* v = elementFor(&sub, n);
        l = l->appendLast(&sub, v); m.push_back(v);
        if (n <= 50 || n % 13 == 0) ASSERT_EQ(rawContents(&sub, l), m) << n;
    }
    for (int n : {6, 7, 40, 300, 1300}) {
        Model mm(m.begin(), m.begin() + n);
        const ProtoList* r = relaxedListOf(&sub, mm, rng);
        ASSERT_EQ(rawContents(&sub, r), mm) << "relaxed " << n;
    }
    const ProtoListIterator* it = l->getIterator(&sub);
    for (size_t i = 0; i < m.size(); ++i) it = it->advance(&sub);
    EXPECT_FALSE(it->hasNext(&sub));
    EXPECT_EQ(it->advance(&sub), it->advance(&sub)->advance(&sub)) << "advance past the end is stable";
}

// Appending costs about three cells per element; reading allocates nothing.
TEST_F(ListRrbTest, AppendCostsAboutThreeCellsAndReadsNone) {
    ProtoContext sub(space, c, nullptr, nullptr, nullptr, nullptr);
    const proto_ulong before = sub.allocatedCellsCount;
    const ProtoList* l = sub.newList();
    const int N = 100000;
    for (int i = 0; i < N; ++i) l = l->appendLast(&sub, sub.fromInteger(i));
    const double perAppend = double(sub.allocatedCellsCount - before) / N;
    EXPECT_LT(perAppend, 3.5) << perAppend;
    const proto_ulong beforeReads = sub.allocatedCellsCount;
    long long sum = 0;
    for (int i = 0; i < N; ++i) sum += l->getAt(&sub, (i * 7919) % N)->asLong(&sub);
    for (const ProtoListIterator* it = l->getIterator(&sub); it->hasNext(&sub); it = it->advance(&sub)) (void) 0;
    EXPECT_EQ(sum, static_cast<long long>(N) * (N - 1) / 2);
    const proto_ulong readsCells = sub.allocatedCellsCount - beforeReads;
    EXPECT_LE(readsCells, static_cast<proto_ulong>(N) + 1) << "only the iterator cells (one per advance)";
}

// Deriving many versions from ONE list costs a bounded number of cells per
// version, whatever its size: the regression that phase 1 (convert to AVL
// per structural operation, O(n) per version) introduced.
TEST_F(ListRrbTest, DerivingFromOneBaseCostsBoundedCells) {
    std::mt19937 rng(4u);
    for (int shape = 0; shape < 2; ++shape) {
        ProtoContext sub(space, c, nullptr, nullptr, nullptr, nullptr);
        const int N = 100000;
        Model m; for (int i = 0; i < N; ++i) m.push_back(sub.fromInteger(i));
        const ProtoList* base = shape ? relaxedListOf(&sub, m, rng) : listOf(&sub, m);
        const ProtoList* half = listOf(&sub, Model(m.begin(), m.begin() + N / 2));
        const ProtoObject* x = sub.fromInteger(-1);
        struct Op { const char* name; std::function<const ProtoList*(ProtoContext*)> run; double maxCells; };
        const std::vector<Op> ops = {
            {"setAt",      [&](ProtoContext* k) { return base->setAt(k, N / 3, x); }, 20},
            {"appendLast", [&](ProtoContext* k) { return base->appendLast(k, x); }, 20},
            {"removeLast", [&](ProtoContext* k) { return base->removeLast(k); }, 20},
            {"appendFirst",[&](ProtoContext* k) { return base->appendFirst(k, x); }, 40},
            {"insertAt",   [&](ProtoContext* k) { return base->insertAt(k, N / 2, x); }, 40},
            {"removeAt",   [&](ProtoContext* k) { return base->removeAt(k, N / 2); }, 40},
            {"removeFirst",[&](ProtoContext* k) { return base->removeFirst(k); }, 40},
            {"getSlice",   [&](ProtoContext* k) { return base->getSlice(k, N / 4, 3 * N / 4); }, 60},
            {"removeSlice",[&](ProtoContext* k) { return base->removeSlice(k, N / 4, 3 * N / 4); }, 80},
            {"extend",     [&](ProtoContext* k) { return half->extend(k, half); }, 80},
        };
        for (const Op& op : ops) {
            ProtoContext k(space, &sub, nullptr, nullptr, nullptr, nullptr);
            const proto_ulong before = k.allocatedCellsCount;
            const ProtoList* r = nullptr;
            for (int i = 0; i < 200; ++i) r = op.run(&k);
            const double perCall = double(k.allocatedCellsCount - before) / 200.0;
            EXPECT_LE(perCall, op.maxCells) << (shape ? "relaxed " : "strict ") << op.name << ": " << perCall << " cells per call";
            const std::string form = checkForm(&k, r);
            EXPECT_TRUE(form.empty()) << op.name << ": " << form;
        }
    }
}

// Adversarial build orders keep the tree near its optimal height (and
// getAt fast): prepends, inserts at the middle, random inserts, random
// removals, repeated concatenation of small pieces.
TEST_F(ListRrbTest, BuildOrdersKeepTheHeightNearTheOptimum) {
    ProtoContext sub(space, c, nullptr, nullptr, nullptr, nullptr);
    const int N = 20000;
    std::mt19937 rng(6u);
    auto heightOk = [&](const ProtoList* l, const char* what) {
        const auto* r = rrbOf(l);
        ASSERT_NE(r, nullptr) << what;
        ASSERT_TRUE(checkRrb(r).empty()) << what << ": " << checkRrb(r);
        EXPECT_LE(r->height, optimalHeight(r->treeSize) + 1u) << what << " height " << unsigned(r->height)
            << " for " << r->treeSize << " elements in the tree";
    };
    Model mp; const ProtoList* pre = sub.newList();
    for (int i = 0; i < N; ++i) { pre = pre->appendFirst(&sub, sub.fromInteger(i)); mp.insert(mp.begin(), sub.fromInteger(i)); }
    heightOk(pre, "prepends"); expectEqual(&sub, pre, mp, "prepends");
    Model mm; const ProtoList* mid = sub.newList();
    for (int i = 0; i < N; ++i) { mid = mid->insertAt(&sub, i / 2, sub.fromInteger(i)); mm.insert(mm.begin() + i / 2, sub.fromInteger(i)); }
    heightOk(mid, "middle inserts"); expectEqual(&sub, mid, mm, "middle inserts");
    Model mr; const ProtoList* ran = sub.newList();
    for (int i = 0; i < N; ++i) { int at = static_cast<int>(rng() % (mr.size() + 1)); ran = ran->insertAt(&sub, at, sub.fromInteger(i)); mr.insert(mr.begin() + at, sub.fromInteger(i)); }
    heightOk(ran, "random inserts"); expectEqual(&sub, ran, mr, "random inserts");
    while (mr.size() > N / 4) { int at = static_cast<int>(rng() % mr.size()); ran = ran->removeAt(&sub, at); mr.erase(mr.begin() + at); }
    heightOk(ran, "random removals"); expectEqual(&sub, ran, mr, "random removals");
    Model mc; for (int i = 0; i < N; ++i) mc.push_back(sub.fromInteger(i));
    const ProtoList* cat = relaxedListOf(&sub, mc, rng);
    heightOk(cat, "concatenated pieces"); expectEqual(&sub, cat, mc, "concatenated pieces");
}

// newList(n, items) and the tuple conversion read every form.
TEST_F(ListRrbTest, BulkBuildsAndTuplesMatchTheModel) {
    ProtoContext sub(space, c, nullptr, nullptr, nullptr, nullptr);
    std::mt19937 rng(2u);
    for (unsigned n : {0u, 1u, 5u, 6u, 7u, 36u, 37u, 216u, 217u, 1297u}) {
        Model m; for (unsigned i = 0; i < n; ++i) m.push_back(elementFor(&sub, i));
        for (const ProtoList* l : {sub.newList(n, m.data()), relaxedListOf(&sub, m, rng)}) {
            expectEqual(&sub, l, m, "n=" + std::to_string(n));
            const ProtoTuple* t = sub.newTupleFromList(l);
            ASSERT_EQ(t->getSize(&sub), n);
            for (unsigned i = 0; i < n; ++i) {
                const ProtoObject* want = m[i] ? m[i] : PROTO_NONE;
                const ProtoObject* got = t->getAt(&sub, static_cast<int>(i));
                ASSERT_EQ(got ? got : PROTO_NONE, want) << "tuple n=" << n << " at " << i;
            }
        }
    }
}

// AVL lists (built cell by cell, as older code paths may) are still read and
// accepted by every operation, alone and mixed with RRB operands.  This also
// exercises the layout contract that tells the forms apart without a
// virtual call: a misread AVL list would return wrong elements here.
TEST_F(ListRrbTest, AvlListsStillWorkAloneAndMixed) {
    ProtoContext sub(space, c, nullptr, nullptr, nullptr, nullptr);
    std::function<const ProtoListImplementation*(const ProtoObject* const*, int)> nodes =
        [&](const ProtoObject* const* items, int n) -> const ProtoListImplementation* {
            if (n <= 0) return nullptr;
            const int mid = n / 2;
            return new (&sub) ProtoListImplementation(&sub, items[mid], false, nodes(items, mid),
                                                      nodes(items + mid + 1, n - mid - 1));
        };
    for (int n : {1, 2, 6, 7, 40, 300}) {
        Model m; for (int i = 0; i < n; ++i) m.push_back(sub.fromInteger(1000 + i));
        const ProtoList* avl = nodes(m.data(), n)->asProtoList(&sub);
        ASSERT_FALSE(isRrb(avl));
        ASSERT_EQ(avl->getSize(&sub), static_cast<proto_ulong>(n));
        for (int i = 0; i < n; ++i) ASSERT_EQ(avl->getAt(&sub, i), m[i]) << "avl n=" << n << " at " << i;
        ASSERT_EQ(rawContents(&sub, avl), m);
        Model other; for (int i = 0; i < 50; ++i) other.push_back(sub.fromInteger(i));
        const ProtoList* rr = listOf(&sub, other);
        Model both = m; both.insert(both.end(), other.begin(), other.end());
        Model both2 = other; both2.insert(both2.end(), m.begin(), m.end());
        const ProtoList* e1 = avl->extend(&sub, rr);
        const ProtoList* e2 = rr->extend(&sub, avl);
        ASSERT_EQ(rawContents(&sub, e1), both);
        ASSERT_EQ(rawContents(&sub, e2), both2);
        Model ins = m; ins.insert(ins.begin() + n / 2, PROTO_NONE);
        ASSERT_EQ(rawContents(&sub, avl->insertAt(&sub, n / 2, PROTO_NONE)), ins);
    }
}

// Lists that would pass the 48-bit size tables raise instead of corrupting them.
TEST_F(ListRrbTest, SizeLimitRaises) {
    ProtoContext sub(space, c, nullptr, nullptr, nullptr, nullptr);
    Model m; for (int i = 0; i < 64; ++i) m.push_back(sub.fromInteger(i));
    const ProtoList* l = listOf(&sub, m);
    EXPECT_THROW((void) l->multiply(&sub, sub.fromLong(static_cast<long long>(1) << 43)), ProtoException);
}

// RRB lists survive collections between operations: the list is held only
// by a root set, and the work in between makes enough garbage to recycle cells.
TEST_F(ListRrbTest, SurvivesCollectionsBetweenOperations) {
    ProtoRootSet* roots = space->createRootSet("list-rrb");
    Model model;
    ProtoRootSet::Handle handle;
    {
        ProtoContext sub(space, c, nullptr, nullptr, nullptr, nullptr);
        for (int i = 0; i < 500; ++i) model.push_back(sub.fromInteger(i));
        handle = roots->add(listOf(&sub, model)->asObject(&sub));
    }
    std::mt19937 rng(3u);
    for (int it = 0; it < 150; ++it) {
        {
            ProtoContext sub(space, c, nullptr, nullptr, nullptr, nullptr);
            const ProtoList* cur = roots->resolve(handle)->asList(&sub);
            for (int g = 0; g < 3000; ++g) (void) sub.newList()->appendLast(&sub, sub.fromInteger(g));
            const ProtoList* next = cur;
            for (int k = 0; k < 20; ++k) {
                const ProtoObject* v = sub.fromInteger(it * 1000 + k);
                const size_t at = rng() % (model.size() + 1);
                next = next->insertAt(&sub, static_cast<int>(at), v); model.insert(model.begin() + at, v);
            }
            const size_t a = rng() % model.size(), b = std::min(model.size(), a + rng() % 30);
            next = next->removeSlice(&sub, static_cast<int>(a), static_cast<int>(b)); model.erase(model.begin() + a, model.begin() + b);
            const Model head(model.begin(), model.begin() + 10);
            next = next->extend(&sub, listOf(&sub, head));
            model.insert(model.end(), head.begin(), head.end());
            while (model.size() > 800) { next = next->removeFirst(&sub); model.erase(model.begin()); }
            auto h2 = roots->add(next->asObject(&sub));
            roots->remove(handle);
            handle = h2;
        }
        space->triggerGC();
        ProtoContext sub(space, c, nullptr, nullptr, nullptr, nullptr);
        const ProtoList* cur = roots->resolve(handle)->asList(&sub);
        ASSERT_TRUE(isRrb(cur));
        ASSERT_TRUE(checkRrb(rrbOf(cur)).empty()) << checkRrb(rrbOf(cur));
        ASSERT_EQ(cur->getSize(&sub), model.size());
        if (it % 10 == 0) ASSERT_EQ(rawContents(&sub, cur), model);
    }
    roots->remove(handle);
}

namespace {

constexpr int kRrbBase = 1000;
constexpr int kRrbThreads = 4;
constexpr int kRrbPerThread = 6000;
std::atomic<unsigned> gRrbErrors{0};
const ProtoList* gRrbBase = nullptr;

// Each worker derives its own versions from the shared base with every kind
// of operation, reading the shared base and its own version as it goes.
// Runs as a real ProtoThread (see concWorkerMain in ProtoMapGCTests.cpp).
const ProtoObject* rrbWorkerMain(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                                 const ProtoArguments* args, const ProtoKeywordArguments*) {
    const long long t = args->getAt(ctx, 0)->asLong(ctx);
    std::mt19937 rng(static_cast<unsigned>(t) + 11u);
    std::vector<long long> model; for (int i = 0; i < kRrbBase; ++i) model.push_back(i);
    const ProtoList* mine = gRrbBase;
    for (int i = 0; i < kRrbPerThread; ++i) {
        const long long v = t * 1000000 + i;
        switch (rng() % 5) {
            case 0: mine = mine->appendLast(ctx, ctx->fromLong(v)); model.push_back(v); break;
            case 1: { size_t at = rng() % (model.size() + 1); mine = mine->insertAt(ctx, static_cast<int>(at), ctx->fromLong(v));
                      model.insert(model.begin() + at, v); } break;
            case 2: if (model.size() > 10) { size_t at = rng() % model.size(); mine = mine->removeAt(ctx, static_cast<int>(at));
                      model.erase(model.begin() + at); } break;
            case 3: mine = mine->appendFirst(ctx, ctx->fromLong(v)); model.insert(model.begin(), v); break;
            case 4: if (model.size() > 2000) { mine = mine->getSlice(ctx, 100, static_cast<int>(model.size()) - 100);
                      model = std::vector<long long>(model.begin() + 100, model.end() - 100); } break;
        }
        if (i % 97 == 0) {
            const int b = (i * 7) % kRrbBase;
            if (gRrbBase->getSize(ctx) != static_cast<proto_ulong>(kRrbBase) ||
                gRrbBase->getAt(ctx, b)->asLong(ctx) != b) gRrbErrors.fetch_add(1);
            const size_t k = rng() % model.size();
            if (mine->getAt(ctx, static_cast<int>(k))->asLong(ctx) != model[k]) gRrbErrors.fetch_add(1);
        }
    }
    if (mine->getSize(ctx) != model.size()) gRrbErrors.fetch_add(1);
    size_t i = 0;
    for (const ProtoListIterator* it = mine->getIterator(ctx); it->hasNext(ctx); it = it->advance(ctx), ++i)
        if (i >= model.size() || it->next(ctx)->asLong(ctx) != model[i]) { gRrbErrors.fetch_add(1); break; }
    return PROTO_NONE;
}

}  // namespace

// Several threads derive their own versions of one shared RRB list with every
// kind of operation while a kicker keeps requesting collections: the base
// never changes and every version matches its thread's model.
TEST(ListRrbConcurrency, VersionsFromASharedBaseWhileTheGcRuns) {
    ProtoSpace space;
    ProtoContext* root = space.rootContext;
    const ProtoList* base = root->newList();
    for (int i = 0; i < kRrbBase; ++i) base = base->appendLast(root, root->fromInteger(i));
    ASSERT_TRUE(isRrb(base));
    gRrbErrors = 0;
    gRrbBase = base;

    std::atomic<bool> stopGc{false};
    std::thread gcKicker([&]() {
        while (!stopGc.load(std::memory_order_relaxed)) {
            space.triggerGC();
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });
    std::vector<const ProtoThread*> workers;
    for (int t = 0; t < kRrbThreads; ++t) {
        const ProtoList* args = root->newList()->appendLast(root, root->fromInteger(t));
        workers.push_back(space.newThread(root, ProtoString::createSymbol(root, "list-rrb-worker"),
                                          rrbWorkerMain, args, nullptr));
    }
    {
        ProtoContext::UnmanagedScope parked(root);
        for (const ProtoThread* w : workers) const_cast<ProtoThread*>(w)->join(root);
    }
    stopGc.store(true, std::memory_order_relaxed);
    gcKicker.join();

    EXPECT_EQ(gRrbErrors.load(), 0u);
    ASSERT_EQ(base->getSize(root), static_cast<proto_ulong>(kRrbBase));
    for (int i = 0; i < kRrbBase; ++i) ASSERT_EQ(base->getAt(root, i)->asLong(root), i);
}
