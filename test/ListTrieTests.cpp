// ListTrieTests.cpp — the trie form of ProtoList (core/ProtoList.cpp,
// "ProtoListTrieNode / ProtoListTrieImplementation").
//
// A list of more than MAX_INLINE elements built by appends is a persistent
// trie with six slots per node; it shares POINTER_TAG_LIST with the AVL form.
// Appends, indexed reads, setAt and removeLast run on the trie; structural
// operations move the list to the AVL form.  Every check below compares with
// a std::vector model, element by element as stored (nullptr included), and
// checks the structure of every list it sees (ListTrieCheck.h for tries,
// size and form for the others).

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"
#include "ListTrieCheck.h"

#include <atomic>
#include <chrono>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace proto;
using proto_test::checkTrie;
using proto_test::isTrie;
using proto_test::trieOf;

namespace {

constexpr unsigned MAX_INLINE = ProtoListSmallImplementation::MAX_INLINE;
using Model = std::vector<const ProtoObject*>;

bool isSmall(const ProtoList* l) {
    ProtoObjectPointer pa{}; pa.oid = reinterpret_cast<const ProtoObject*>(l);
    return pa.op.pointer_tag == POINTER_TAG_LIST_SMALL;
}

bool isTag2(const ProtoList* l) {
    ProtoObjectPointer pa{}; pa.oid = reinterpret_cast<const ProtoObject*>(l);
    return pa.op.pointer_tag == POINTER_TAG_LIST;
}

// Elements as stored, through the iterator (getAt maps nullptr to PROTO_NONE).
Model rawContents(ProtoContext* c, const ProtoList* l) {
    Model out;
    for (const ProtoListIterator* it = l->getIterator(c); it->hasNext(c); it = it->advance(c))
        out.push_back(it->next(c));
    return out;
}

// Form and structure: SmallList up to MAX_INLINE, a well-formed trie or a
// tag-2 (AVL) list beyond.  Empty string means OK.
std::string checkForm(ProtoContext* c, const ProtoList* l) {
    const proto_ulong n = l->getSize(c);
    if (n <= MAX_INLINE) return isSmall(l) ? "" : "list of at most MAX_INLINE elements is not a SmallList";
    if (!isTag2(l)) return "list larger than MAX_INLINE is not tag 2";
    if (const auto* t = trieOf(l)) return checkTrie(t);
    return "";
}

// Compares a list with the model three ways: size, getAt (nullptr read as
// PROTO_NONE) and the iterator (as stored).
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

// A mix of element kinds: SmallIntegers, nullptr, and cells (strings and objects).
const ProtoObject* elementFor(ProtoContext* c, long long i) {
    switch (i % 7) {
        case 3: return nullptr;
        case 5: return ProtoString::fromUTF8(c, ("s" + std::to_string(i)).c_str())->asObject(c);
        case 6: return c->newObject(false);
        default: return c->fromInteger(i);
    }
}

}  // namespace

class ListTrieTest : public ::testing::Test {
protected:
    ProtoSpace* space = nullptr;
    ProtoContext* c = nullptr;
    void SetUp() override { space = new ProtoSpace(); c = space->rootContext; }
    void TearDown() override { delete space; }
};

// Appends from the empty list across every change of form and of height
// (5 -> 6 -> 7, 36 -> 37, 216 -> 217, 1296 -> 1297), then removeLast all the
// way down; every size is checked.
TEST_F(ListTrieTest, GrowsAcrossEveryBorderAndShrinksBack) {
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
        ASSERT_EQ(isTrie(l), m.size() > MAX_INLINE) << "size " << m.size();
        const std::string form = checkForm(&sub, l);
        ASSERT_TRUE(form.empty()) << "size " << m.size() << ": " << form;
        ASSERT_EQ(rawContents(&sub, l).back(), v);
    }
    expectEqual(&sub, l, m, "built to 1400");
    for (int k : {0, 1, 5, 6, 7, 36, 37, 216, 217, 1296, 1297}) {
        expectEqual(&sub, versions[k], Model(m.begin(), m.begin() + k), "version " + std::to_string(k));
    }
    while (!m.empty()) {
        l = l->removeLast(&sub);
        m.pop_back();
        ASSERT_EQ(l->getSize(&sub), m.size());
        const std::string form = checkForm(&sub, l);
        ASSERT_TRUE(form.empty()) << "size " << m.size() << ": " << form;
        if (m.size() % 37 == 0 || m.size() < 10) expectEqual(&sub, l, m, "shrunk to " + std::to_string(m.size()));
    }
}

// Every version made along the way keeps its contents after all the
// operations that came after it (immutability, structural sharing).
TEST_F(ListTrieTest, OldVersionsStayIntact) {
    ProtoContext sub(space, c, nullptr, nullptr, nullptr, nullptr);
    std::mt19937 rng(11u);
    Model m;
    const ProtoList* l = sub.newList();
    std::vector<std::pair<const ProtoList*, Model>> kept;
    for (int step = 0; step < 3000; ++step) {
        const unsigned op = rng() % 10;
        if (op < 6 || m.size() < 2) {
            const ProtoObject* v = elementFor(&sub, step);
            l = l->appendLast(&sub, v); m.push_back(v);
        } else if (op < 8) {
            const size_t i = rng() % m.size();
            const ProtoObject* v = sub.fromInteger(-step);
            l = l->setAt(&sub, static_cast<int>(i), v); m[i] = v;
        } else {
            l = l->removeLast(&sub); m.pop_back();
        }
        if (step % 50 == 0) kept.emplace_back(l, m);
    }
    for (size_t k = 0; k < kept.size(); ++k) {
        expectEqual(&sub, kept[k].first, kept[k].second, "kept version " + std::to_string(k));
    }
}

// Random walk over every list operation against the model, on lists that
// start as tries; structural operations move them to the AVL form, appends
// keep the trie.
TEST_F(ListTrieTest, RandomWalkOverEveryOperation) {
    std::mt19937 rng(5u);
    for (int round = 0; round < 6; ++round) {
        ProtoContext sub(space, c, nullptr, nullptr, nullptr, nullptr);
        Model m;
        for (int i = 0; i < 40 + round * 60; ++i) m.push_back(elementFor(&sub, i));
        const ProtoList* l = listOf(&sub, m);
        for (int step = 0; step < 600; ++step) {
            const int n = static_cast<int>(m.size());
            auto pick = [&](int lo, int hi) { return lo + static_cast<int>(rng() % static_cast<unsigned>(hi - lo + 1)); };
            const int op = pick(0, 13);
            std::string what = "round " + std::to_string(round) + " step " + std::to_string(step) + " op " + std::to_string(op);
            const ProtoObject* v = elementFor(&sub, step * 31 + op);
            if (op <= 3) {
                l = l->appendLast(&sub, v); m.push_back(v);
            } else if (op == 4 && n > 0) {
                const int i = pick(0, n - 1); l = l->setAt(&sub, i, v); m[i] = v;
            } else if (op == 5 && n > 0) {
                l = l->removeLast(&sub); m.pop_back();
            } else if (op == 6) {
                const int i = pick(0, n); l = l->insertAt(&sub, i, v); m.insert(m.begin() + i, v);
            } else if (op == 7 && n > 0) {
                const int i = pick(0, n - 1); l = l->removeAt(&sub, i); m.erase(m.begin() + i);
            } else if (op == 8) {
                l = l->appendFirst(&sub, v); m.insert(m.begin(), v);
            } else if (op == 9 && n > 0) {
                l = l->removeFirst(&sub); m.erase(m.begin());
            } else if (op == 10) {
                const int k = pick(0, 80);
                Model o; for (int i = 0; i < k; ++i) o.push_back(elementFor(&sub, step + i));
                l = l->extend(&sub, listOf(&sub, o)); m.insert(m.end(), o.begin(), o.end());
            } else if (op == 11) {
                int a = pick(-1, n + 1), b = pick(-1, n + 1); if (a > b) std::swap(a, b);
                const int ca = std::clamp(a, 0, n), cb = std::clamp(b, 0, n);
                const ProtoList* s = l->getSlice(&sub, a, b);
                expectEqual(&sub, s, Model(m.begin() + ca, m.begin() + std::max(ca, cb)), what + " slice");
            } else if (op == 12) {
                int a = pick(0, n), b = pick(0, n); if (a > b) std::swap(a, b);
                l = l->removeSlice(&sub, a, b); m.erase(m.begin() + a, m.begin() + b);
            } else if (op == 13 && n > 0 && n < 400) {
                l = l->multiply(&sub, sub.fromInteger(2)); Model mm = m; m.insert(m.end(), mm.begin(), mm.end());
            }
            if (m.size() > 3000) { l = l->splitFirst(&sub, 200); m.resize(200); }
            expectEqual(&sub, l, m, what);
            if (::testing::Test::HasFatalFailure()) return;
        }
    }
}

// The operations native to the trie keep the trie; inserting at the end is
// an append; extending by a short list appends; the other structural
// operations return the AVL form (phase 1: no conversion back).
TEST_F(ListTrieTest, OperationsKeepOrLeaveTheTrieAsDocumented) {
    ProtoContext sub(space, c, nullptr, nullptr, nullptr, nullptr);
    Model m; for (int i = 0; i < 100; ++i) m.push_back(sub.fromInteger(i));
    const ProtoList* t = listOf(&sub, m);
    ASSERT_TRUE(isTrie(t));
    EXPECT_TRUE(isTrie(t->appendLast(&sub, PROTO_NONE)));
    EXPECT_TRUE(isTrie(t->setAt(&sub, 3, PROTO_NONE)));
    EXPECT_TRUE(isTrie(t->removeLast(&sub)));
    EXPECT_TRUE(isTrie(t->insertAt(&sub, 100, PROTO_NONE)));
    EXPECT_TRUE(isTrie(t->extend(&sub, listOf(&sub, Model(64, sub.fromInteger(1))))));
    EXPECT_FALSE(isTrie(t->extend(&sub, listOf(&sub, Model(65, sub.fromInteger(1))))));
    EXPECT_FALSE(isTrie(t->insertAt(&sub, 50, PROTO_NONE)));
    EXPECT_FALSE(isTrie(t->appendFirst(&sub, PROTO_NONE)));
    EXPECT_FALSE(isTrie(t->removeAt(&sub, 10)));
    EXPECT_FALSE(isTrie(t->getSlice(&sub, 10, 90)));
    EXPECT_EQ(t->getSlice(&sub, 0, 100), t);
    EXPECT_EQ(t->setAt(&sub, 100, PROTO_NONE), t);
    EXPECT_EQ(t->setAt(&sub, -1, PROTO_NONE), t);
    EXPECT_EQ(t->removeAt(&sub, 100), t);
    EXPECT_EQ(t->getAt(&sub, 100), PROTO_NONE);
    EXPECT_EQ(t->getAt(&sub, -1), PROTO_NONE);
    // removeLast down to MAX_INLINE elements gives a SmallList.
    const ProtoList* six = listOf(&sub, Model(m.begin(), m.begin() + 6));
    ASSERT_TRUE(isTrie(six));
    EXPECT_TRUE(isSmall(six->removeLast(&sub)));
    expectEqual(&sub, six->removeLast(&sub), Model(m.begin(), m.begin() + 5), "six minus one");
    // has() over trees and tails, by value for integers and strings.
    EXPECT_TRUE(t->has(&sub, sub.fromInteger(0)));
    EXPECT_TRUE(t->has(&sub, sub.fromInteger(99)));
    EXPECT_FALSE(t->has(&sub, sub.fromInteger(100)));
}

// The iterator reads the same elements as getAt on tries of every size up
// to 1300, crossing leaf and tail boundaries; advance past the end stays.
TEST_F(ListTrieTest, IteratorMatchesTheModel) {
    ProtoContext sub(space, c, nullptr, nullptr, nullptr, nullptr);
    Model m;
    const ProtoList* l = sub.newList();
    for (int n = 1; n <= 1300; ++n) {
        const ProtoObject* v = elementFor(&sub, n);
        l = l->appendLast(&sub, v); m.push_back(v);
        if (n <= 50 || n % 13 == 0 || n % 216 <= 1) ASSERT_EQ(rawContents(&sub, l), m) << n;
    }
    const ProtoListIterator* it = l->getIterator(&sub);
    for (size_t i = 0; i < m.size(); ++i) it = it->advance(&sub);
    EXPECT_FALSE(it->hasNext(&sub));
    EXPECT_EQ(it->advance(&sub), it->advance(&sub)->advance(&sub)) << "advance past the end is stable";
}

// Appending costs about three cells per element (tail + root, the path every
// sixth append); reading allocates nothing.  The AVL form needed ~19 at 100 k.
TEST_F(ListTrieTest, AppendCostsAboutThreeCellsAndReadsNone) {
    ProtoContext sub(space, c, nullptr, nullptr, nullptr, nullptr);
    const proto_ulong before = sub.allocatedCellsCount;
    const ProtoList* l = sub.newList();
    const int N = 100000;
    for (int i = 0; i < N; ++i) l = l->appendLast(&sub, sub.fromInteger(i));
    const double perAppend = double(sub.allocatedCellsCount - before) / N;
    EXPECT_LT(perAppend, 3.5) << perAppend;
    EXPECT_GT(perAppend, 2.0) << perAppend;
    const proto_ulong beforeReads = sub.allocatedCellsCount;
    long long sum = 0;
    for (int i = 0; i < N; ++i) sum += l->getAt(&sub, (i * 7919) % N)->asLong(&sub);
    EXPECT_EQ(sub.allocatedCellsCount, beforeReads);
    EXPECT_EQ(sum, static_cast<long long>(N) * (N - 1) / 2);
}

// newList(n, items) and makeOutputList build the same tries appends build.
TEST_F(ListTrieTest, BulkBuildsAndTuplesMatchTheModel) {
    ProtoContext sub(space, c, nullptr, nullptr, nullptr, nullptr);
    for (unsigned n : {0u, 1u, 5u, 6u, 7u, 36u, 37u, 216u, 217u, 1297u}) {
        Model m; for (unsigned i = 0; i < n; ++i) m.push_back(elementFor(&sub, i));
        const ProtoList* l = sub.newList(n, m.data());
        expectEqual(&sub, l, m, "newList n=" + std::to_string(n));
        const ProtoTuple* t = sub.newTupleFromList(l);
        ASSERT_EQ(t->getSize(&sub), n);
        for (unsigned i = 0; i < n; ++i) {
            const ProtoObject* want = m[i] ? m[i] : PROTO_NONE;
            const ProtoObject* got = t->getAt(&sub, static_cast<int>(i));
            ASSERT_EQ(got ? got : PROTO_NONE, want) << "tuple n=" << n << " at " << i;
        }
    }
}

// Tries survive collections between operations: the list is held only by
// a root set, and the work in between makes enough garbage to recycle cells.
TEST_F(ListTrieTest, SurvivesCollectionsBetweenOperations) {
    ProtoRootSet* roots = space->createRootSet("list-trie");
    std::vector<long long> model;
    ProtoRootSet::Handle handle;
    {
        ProtoContext sub(space, c, nullptr, nullptr, nullptr, nullptr);
        const ProtoList* l = sub.newList();
        for (int i = 0; i < 500; ++i) { l = l->appendLast(&sub, sub.fromInteger(i)); model.push_back(i); }
        handle = roots->add(l->asObject(&sub));
    }
    std::mt19937 rng(3u);
    for (int it = 0; it < 150; ++it) {
        {
            ProtoContext sub(space, c, nullptr, nullptr, nullptr, nullptr);
            const ProtoList* cur = roots->resolve(handle)->asList(&sub);
            for (int g = 0; g < 3000; ++g) (void) sub.newList()->appendLast(&sub, sub.fromInteger(g));
            const ProtoList* next = cur;
            for (int k = 0; k < 40; ++k) { next = next->appendLast(&sub, sub.fromInteger(it * 1000 + k)); model.push_back(it * 1000 + k); }
            const size_t i = rng() % model.size();
            next = next->setAt(&sub, static_cast<int>(i), sub.fromInteger(-it)); model[i] = -it;
            for (int k = 0; k < 13; ++k) { next = next->removeLast(&sub); model.pop_back(); }
            auto h2 = roots->add(next->asObject(&sub));
            roots->remove(handle);
            handle = h2;
        }
        space->triggerGC();
        ProtoContext sub(space, c, nullptr, nullptr, nullptr, nullptr);
        const ProtoList* cur = roots->resolve(handle)->asList(&sub);
        ASSERT_TRUE(isTrie(cur));
        ASSERT_TRUE(checkTrie(trieOf(cur)).empty()) << checkTrie(trieOf(cur));
        ASSERT_EQ(cur->getSize(&sub), model.size());
        if (it % 10 == 0) {
            for (size_t i = 0; i < model.size(); ++i)
                ASSERT_EQ(cur->getAt(&sub, static_cast<int>(i))->asLong(&sub), model[i]) << i;
        }
    }
    roots->remove(handle);
}

namespace {

constexpr int kTrieBase = 1000;
constexpr int kTrieThreads = 4;
constexpr int kTriePerThread = 20000;
std::atomic<unsigned> gTrieErrors{0};
const ProtoList* gTrieBase = nullptr;

// Each worker derives its own versions from the shared base by appending,
// reading the shared base and its own version as it goes.  Runs as a real
// ProtoThread (see the comment on concWorkerMain in ProtoMapGCTests.cpp).
const ProtoObject* trieWorkerMain(ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                                  const ProtoArguments* args, const ProtoKeywordArguments*) {
    const long long t = args->getAt(ctx, 0)->asLong(ctx);
    const ProtoList* mine = gTrieBase;
    for (int i = 0; i < kTriePerThread; ++i) {
        mine = mine->appendLast(ctx, ctx->fromInteger(t * 1000000 + i));
        if (i % 97 == 0) {
            const int b = (i * 7) % kTrieBase;
            if (gTrieBase->getSize(ctx) != static_cast<proto_ulong>(kTrieBase) ||
                gTrieBase->getAt(ctx, b)->asLong(ctx) != b) gTrieErrors.fetch_add(1);
            if (mine->getAt(ctx, b)->asLong(ctx) != b) gTrieErrors.fetch_add(1);
            if (mine->getAt(ctx, kTrieBase + i)->asLong(ctx) != t * 1000000 + i) gTrieErrors.fetch_add(1);
        }
    }
    if (mine->getSize(ctx) != static_cast<proto_ulong>(kTrieBase + kTriePerThread)) gTrieErrors.fetch_add(1);
    long long sum = 0;
    for (const ProtoListIterator* it = mine->getIterator(ctx); it->hasNext(ctx); it = it->advance(ctx))
        sum += it->next(ctx)->asLong(ctx);
    long long want = static_cast<long long>(kTrieBase) * (kTrieBase - 1) / 2;
    for (int i = 0; i < kTriePerThread; ++i) want += t * 1000000 + i;
    if (sum != want) gTrieErrors.fetch_add(1);
    return PROTO_NONE;
}

}  // namespace

// Several threads append to their own versions of one shared trie while a
// kicker keeps requesting collections: the base never changes and every
// version holds exactly base + its own elements.
TEST(ListTrieConcurrency, VersionsFromASharedBaseWhileTheGcRuns) {
    ProtoSpace space;
    ProtoContext* root = space.rootContext;
    const ProtoList* base = root->newList();
    for (int i = 0; i < kTrieBase; ++i) base = base->appendLast(root, root->fromInteger(i));
    ASSERT_TRUE(isTrie(base));
    gTrieErrors = 0;
    gTrieBase = base;

    std::atomic<bool> stopGc{false};
    std::thread gcKicker([&]() {
        while (!stopGc.load(std::memory_order_relaxed)) {
            space.triggerGC();
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });
    std::vector<const ProtoThread*> workers;
    for (int t = 0; t < kTrieThreads; ++t) {
        const ProtoList* args = root->newList()->appendLast(root, root->fromInteger(t));
        workers.push_back(space.newThread(root, ProtoString::createSymbol(root, "list-trie-worker"),
                                          trieWorkerMain, args, nullptr));
    }
    {
        ProtoContext::UnmanagedScope parked(root);
        for (const ProtoThread* w : workers) const_cast<ProtoThread*>(w)->join(root);
    }
    stopGc.store(true, std::memory_order_relaxed);
    gcKicker.join();

    EXPECT_EQ(gTrieErrors.load(), 0u);
    ASSERT_EQ(base->getSize(root), static_cast<proto_ulong>(kTrieBase));
    for (int i = 0; i < kTrieBase; ++i) ASSERT_EQ(base->getAt(root, i)->asLong(root), i);
}
