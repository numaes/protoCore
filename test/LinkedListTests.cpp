// LinkedListTests.cpp — ProtoLinkedList (3.3.0).
//
// Specification: docs/specs/2026-10-25-linked-list-design.md, section 8.
// A list is a chain of cells of four elements, every cell full except the
// head (the list is aligned at its end).  These tests pin the canonical
// form, the cells each operation allocates, the sharing of suffixes, the
// conversions, hashing and equality, collection, long chains, concurrent
// readers, the object-model integration and the unchanged ProtoSpace
// layout.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>
#include <vector>

using namespace proto;

namespace {

const ProtoLinkedListNode* nodeOf(const ProtoLinkedList* list) {
    return reinterpret_cast<const ProtoLinkedListNode*>(
        reinterpret_cast<uintptr_t>(list) & ~static_cast<uintptr_t>(0x3F));
}

proto_ulong cellsFor(proto_ulong n) { return (n + 3) / 4; }

// Fails the current test unless `list` is in canonical form: the head holds
// ((n - 1) mod 4) + 1 elements, every other cell 4, and every cell's total
// is the count of its suffix.
void expectCanonical(ProtoContext* ctx, const ProtoLinkedList* list) {
    const proto_ulong n = list->getSize(ctx);
    const ProtoLinkedListNode* node = nodeOf(list);
    ASSERT_NE(node, nullptr);
    if (n == 0) {
        EXPECT_EQ(node->nodeCount(), 0u);
        EXPECT_EQ(node->next, nullptr);
        return;
    }
    EXPECT_EQ(node->nodeCount(), ((n - 1) % 4) + 1) << "head of a list of " << n;
    proto_ulong remaining = n;
    for (const ProtoLinkedListNode* c = node; c; c = c->next) {
        EXPECT_EQ(c->totalCount(), remaining);
        if (c != node) EXPECT_EQ(c->nodeCount(), 4u);
        remaining -= c->nodeCount();
    }
    EXPECT_EQ(remaining, 0u);
}

std::vector<const ProtoObject*> ints(ProtoContext* ctx, int from, int count) {
    std::vector<const ProtoObject*> v;
    for (int i = 0; i < count; ++i) v.push_back(ctx->fromInteger(from + i));
    return v;
}

std::vector<long long> contents(ProtoContext* ctx, const ProtoLinkedList* list) {
    std::vector<long long> out;
    ProtoLinkedListCursor cursor(list);
    while (cursor.hasNext()) out.push_back(cursor.next()->asLong(ctx));
    return out;
}

std::vector<long long> range(int from, int count) {
    std::vector<long long> v;
    for (int i = 0; i < count; ++i) v.push_back(from + i);
    return v;
}

void requestCycle(ProtoSpace& space) {
    std::lock_guard<std::recursive_mutex> lock(ProtoSpace::globalMutex);
    space.gcStarted = true;
    space.gcCV.notify_all();
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. Canonical form
// ---------------------------------------------------------------------------

TEST(LinkedList, EveryOperationKeepsTheCanonicalForm) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    const ProtoLinkedList* list = ctx->newLinkedList();
    expectCanonical(ctx, list);
    for (int i = 0; i < 37; ++i) {
        list = list->prepend(ctx, ctx->fromInteger(i));
        expectCanonical(ctx, list);
    }
    for (const ProtoLinkedList* l = list; !l->isEmpty(ctx); l = l->getRest(ctx)) {
        expectCanonical(ctx, l);
    }
    for (proto_ulong k = 0; k <= 40; ++k) {
        expectCanonical(ctx, list->removeFirst(ctx, k));
        expectCanonical(ctx, list->takeFirst(ctx, k));
    }
    for (int n = 0; n <= 13; ++n) {
        const auto values = ints(ctx, 100, n);
        const ProtoLinkedList* built = ctx->newLinkedListFromArray(values.size(), values.data());
        expectCanonical(ctx, built);
        expectCanonical(ctx, built->reverse(ctx));
        for (int m = 0; m <= 9; ++m) {
            const auto more = ints(ctx, 0, m);
            const ProtoLinkedList* tail = ctx->newLinkedListFromArray(more.size(), more.data());
            const ProtoLinkedList* joined = built->prependAll(ctx, tail);
            expectCanonical(ctx, joined);
            std::vector<long long> expected = range(100, n);
            for (long long x : range(0, m)) expected.push_back(x);
            EXPECT_EQ(contents(ctx, joined), expected);
        }
    }
}

TEST(LinkedList, ElementsReadBackInOrder) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    const auto values = ints(ctx, 0, 23);
    const ProtoLinkedList* list = ctx->newLinkedListFromArray(values.size(), values.data());
    EXPECT_EQ(list->getSize(ctx), 23u);
    EXPECT_EQ(contents(ctx, list), range(0, 23));
    for (int i = 0; i < 23; ++i) EXPECT_EQ(list->getAt(ctx, i)->asLong(ctx), i);
    EXPECT_EQ(list->getAt(ctx, 23), nullptr);
    EXPECT_EQ(list->getFirst(ctx)->asLong(ctx), 0);
    EXPECT_EQ(contents(ctx, list->getRest(ctx)), range(1, 22));
    EXPECT_EQ(contents(ctx, list->removeFirst(ctx, 6)), range(6, 17));
    EXPECT_EQ(contents(ctx, list->takeFirst(ctx, 6)), range(0, 6));
    std::vector<long long> reversed = range(0, 23);
    std::reverse(reversed.begin(), reversed.end());
    EXPECT_EQ(contents(ctx, list->reverse(ctx)), reversed);

    // prepend is cons: the new element comes first.
    const ProtoLinkedList* consed = list->prepend(ctx, ctx->fromInteger(-1));
    EXPECT_EQ(consed->getFirst(ctx)->asLong(ctx), -1);
    EXPECT_EQ(consed->getSize(ctx), 24u);
    EXPECT_TRUE(consed->getRest(ctx)->isEqual(ctx, list));
}

// ---------------------------------------------------------------------------
// 2. Cells per operation (spec section 5)
// ---------------------------------------------------------------------------

TEST(LinkedList, CellsPerOperation) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;
    auto cells = [&](auto&& op) {
        const proto_ulong before = ctx->allocatedCellsCount;
        op();
        return ctx->allocatedCellsCount - before;
    };

    for (int n = 1; n <= 41; ++n) {
        const auto values = ints(ctx, 0, n);
        const ProtoLinkedList* list = nullptr;
        EXPECT_EQ(cells([&] { list = ctx->newLinkedListFromArray(values.size(), values.data()); }),
                  cellsFor(n)) << "build of " << n;
        const unsigned head = nodeOf(list)->nodeCount();

        EXPECT_EQ(cells([&] { (void) list->prepend(ctx, ctx->fromInteger(7)); }), 1u);
        EXPECT_EQ(cells([&] { (void) list->getRest(ctx); }), head == 1 ? 0u : 1u);
        EXPECT_EQ(cells([&] {
            (void) list->getFirst(ctx);
            (void) list->getSize(ctx);
            (void) list->isEmpty(ctx);
            (void) list->getAt(ctx, n - 1);
            (void) list->isEqual(ctx, list->asObject(ctx)->asLinkedList(ctx));
            (void) list->getHash(ctx);
            ProtoLinkedListCursor cursor(list);
            while (cursor.hasNext()) (void) cursor.next();
        }), 0u);
        EXPECT_EQ(cells([&] { (void) list->reverse(ctx); }), n == 1 ? 0u : cellsFor(n));
        for (int k = 1; k < n; ++k) {
            EXPECT_EQ(cells([&] { (void) list->takeFirst(ctx, k); }), cellsFor(k));
            // Dropping k elements copies at most the new head.
            const bool cellBoundary = (k == static_cast<int>(head)) || (k > static_cast<int>(head) && (k - head) % 4 == 0);
            EXPECT_EQ(cells([&] { (void) list->removeFirst(ctx, k); }), cellBoundary ? 0u : 1u)
                << "removeFirst(" << k << ") of " << n;
        }
    }
    EXPECT_EQ(cells([&] { (void) ctx->newLinkedList(); }), 0u);
    EXPECT_EQ(cells([&] { (void) ctx->newLinkedListFromArray(0, nullptr); }), 0u);
}

// ---------------------------------------------------------------------------
// 3. Structural sharing
// ---------------------------------------------------------------------------

TEST(LinkedList, SuffixesAreShared) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    const auto values = ints(ctx, 0, 9);   // head of 1, then two full cells
    const ProtoLinkedList* list = ctx->newLinkedListFromArray(values.size(), values.data());
    ASSERT_EQ(nodeOf(list)->nodeCount(), 1u);
    EXPECT_EQ(nodeOf(list->getRest(ctx)), nodeOf(list)->next);
    EXPECT_EQ(nodeOf(list->removeFirst(ctx, 5)), nodeOf(list)->next->next);

    // prependAll shares every full cell of its tail.
    const auto front = ints(ctx, 100, 6);
    const ProtoLinkedList* a = ctx->newLinkedListFromArray(front.size(), front.data());
    const ProtoLinkedList* fullTail = list->getRest(ctx);   // 8 elements, head full
    const ProtoLinkedList* joined = a->prependAll(ctx, fullTail);
    EXPECT_EQ(nodeOf(joined->removeFirst(ctx, 6)), nodeOf(fullTail));
    // A partial head of the tail is merged; the cells after it are shared.
    const ProtoLinkedList* joined2 = a->prependAll(ctx, list);
    EXPECT_EQ(nodeOf(joined2->removeFirst(ctx, 7)), nodeOf(list)->next);
    EXPECT_EQ(contents(ctx, joined2->removeFirst(ctx, 6)), range(0, 9));
}

// ---------------------------------------------------------------------------
// 4. The empty list
// ---------------------------------------------------------------------------

TEST(LinkedList, OneEmptyListPerProcess) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    const ProtoLinkedList* empty = ctx->newLinkedList();
    EXPECT_EQ(ctx->newLinkedList(), empty);
    EXPECT_TRUE(empty->isEmpty(ctx));
    EXPECT_EQ(empty->getSize(ctx), 0u);
    EXPECT_EQ(empty->getFirst(ctx), nullptr);
    EXPECT_EQ(empty->getRest(ctx), empty);
    EXPECT_EQ(empty->getAt(ctx, 0), nullptr);
    EXPECT_EQ(empty->reverse(ctx), empty);
    EXPECT_FALSE(ProtoLinkedListCursor(empty).hasNext());

    const ProtoLinkedList* one = empty->prepend(ctx, ctx->fromInteger(1));
    EXPECT_EQ(one->getRest(ctx), empty);
    EXPECT_EQ(one->removeFirst(ctx, 5), empty);
    EXPECT_EQ(one->takeFirst(ctx, 0), empty);
    EXPECT_EQ(empty->prependAll(ctx, one), one);
    EXPECT_EQ(one->prependAll(ctx, empty), one);
    EXPECT_EQ(ctx->newLinkedListFromArray(0, nullptr), empty);

    // nullptr is not an element.
    EXPECT_EQ(one->prepend(ctx, nullptr), one);
    const ProtoObject* withNulls[] = {ctx->fromInteger(1), nullptr, ctx->fromInteger(2)};
    EXPECT_EQ(contents(ctx, ctx->newLinkedListFromArray(3, withNulls)), (std::vector<long long>{1, 2}));
    // PROTO_NONE is.
    EXPECT_EQ(empty->prepend(ctx, PROTO_NONE)->getSize(ctx), 1u);
}

// ---------------------------------------------------------------------------
// 5. Conversions
// ---------------------------------------------------------------------------

TEST(LinkedList, ConversionsRoundTrip) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    for (int n : {0, 1, 4, 5, 6, 17, 100}) {
        const auto values = ints(ctx, 0, n);
        const ProtoLinkedList* list = ctx->newLinkedListFromArray(values.size(), values.data());

        const ProtoList* asList = list->asList(ctx);
        ASSERT_EQ(asList->getSize(ctx), static_cast<proto_ulong>(n));
        for (int i = 0; i < n; ++i) EXPECT_EQ(asList->getAt(ctx, i)->asLong(ctx), i);
        EXPECT_TRUE(ctx->newLinkedListFromList(asList)->isEqual(ctx, list));

        const ProtoTuple* tuple = list->asTuple(ctx);
        ASSERT_EQ(tuple->getSize(ctx), static_cast<proto_ulong>(n));
        for (int i = 0; i < n; ++i) EXPECT_EQ(tuple->getAt(ctx, i)->asLong(ctx), i);
        EXPECT_TRUE(ctx->newLinkedListFromTuple(tuple)->isEqual(ctx, list));

        // asTuple gives the interned tuple: equal lists, the same tuple.
        const ProtoLinkedList* again = ctx->newLinkedListFromList(asList);
        EXPECT_EQ(again->asTuple(ctx), tuple);
    }
}

// ---------------------------------------------------------------------------
// 6. Hash and equality
// ---------------------------------------------------------------------------

TEST(LinkedList, HashMatchesTupleAndEqualityFollowsElementWords) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    for (int n : {1, 3, 4, 5, 9, 33}) {
        const auto values = ints(ctx, 10, n);
        const ProtoLinkedList* built = ctx->newLinkedListFromArray(values.size(), values.data());
        const ProtoLinkedList* consed = ctx->newLinkedList();
        for (int i = n - 1; i >= 0; --i) consed = consed->prepend(ctx, values[i]);

        EXPECT_TRUE(built->isEqual(ctx, consed));
        EXPECT_EQ(built->getHash(ctx), consed->getHash(ctx));
        EXPECT_EQ(built->getHash(ctx), built->asTuple(ctx)->asObject(ctx)->getHash(ctx));
        EXPECT_EQ(built->asObject(ctx)->getHash(ctx), built->getHash(ctx));

        const ProtoLinkedList* other = consed->getRest(ctx)->prepend(ctx, ctx->fromInteger(-5));
        EXPECT_FALSE(built->isEqual(ctx, other));
        EXPECT_FALSE(built->isEqual(ctx, built->getRest(ctx)));
    }
    EXPECT_TRUE(ctx->newLinkedList()->isEqual(ctx, ctx->newLinkedList()));
    EXPECT_FALSE(ctx->newLinkedList()->isEqual(ctx, nullptr));

    // Equality is by element word: two distinct objects are different
    // elements even when their content is alike.
    const ProtoObject* a = ctx->newObject(false);
    const ProtoObject* b = ctx->newObject(false);
    EXPECT_FALSE(ctx->newLinkedList()->prepend(ctx, a)->isEqual(ctx, ctx->newLinkedList()->prepend(ctx, b)));
}

// ---------------------------------------------------------------------------
// 7. Collection
// ---------------------------------------------------------------------------

// A million lists built and dropped under a hard heap ceiling far below what
// they would take if they were kept: the run completes only if dropped lists
// are reclaimed.
TEST(LinkedList, DroppedListsAreCollected) {
    constexpr int kBatches = 200;
    constexpr int kPerBatch = 5000;           // 5,000 lists of 6 elements: 10,000 cells
    constexpr int kHeadroomCells = 200000;    // the whole run would need 2,000,000

    ProtoSpace space;
    ProtoContext* root = space.rootContext;
    space.setHeapLimits(/*soft=*/0, /*hard=*/space.heapSize + kHeadroomCells);
    const uint64_t cyclesStart = space.getGCCycleCount();

    long long checksum = 0;
    for (int batch = 0; batch < kBatches; ++batch) {
        ProtoContext work(&space, root, nullptr, nullptr, nullptr, nullptr);
        for (int i = 0; i < kPerBatch; ++i) {
            const ProtoLinkedList* list = work.newLinkedList();
            for (int j = 0; j < 6; ++j) list = list->prepend(&work, work.fromInteger(i + j));
            checksum += list->getFirst(&work)->asLong(&work);
        }
    }
    space.setHeapLimits(0, 0);

    long long expected = 0;
    for (int i = 0; i < kPerBatch; ++i) expected += i + 5;
    EXPECT_EQ(checksum, expected * kBatches);
    EXPECT_GE(space.getGCCycleCount() - cyclesStart, 1u)
        << "no collection ran — the ceiling did not exercise the collector";
}

// A list kept by a root survives cycles intact.
TEST(LinkedList, RetainedListSurvivesCollections) {
    ProtoSpace space;
    ProtoContext* root = space.rootContext;

    const ProtoString* key = ProtoString::createSymbol(root, "idx");
    std::vector<const ProtoObject*> values;
    for (int i = 0; i < 1000; ++i) {
        const ProtoObject* obj = root->newObject(false);
        values.push_back(obj->setAttribute(root, key, root->fromInteger(i)));
    }
    const ProtoLinkedList* list = root->newLinkedListFromArray(values.size(), values.data());
    values.clear();
    ProtoRootSet* pins = space.createRootSet("linked-list-test");
    pins->add(list->asObject(root));

    for (int cycle = 0; cycle < 3; ++cycle) {
        const uint64_t before = space.getGCCycleCount();
        requestCycle(space);
        ProtoContext::UnmanagedScope parked(root);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (space.getGCCycleCount() == before && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    ASSERT_EQ(list->getSize(root), 1000u);
    int i = 0;
    ProtoLinkedListCursor cursor(list);
    while (cursor.hasNext()) {
        const ProtoObject* stored = cursor.next()->getAttribute(root, key);
        ASSERT_NE(stored, nullptr);
        EXPECT_EQ(stored->asLong(root), i++);
    }
    EXPECT_EQ(i, 1000);
}

// ---------------------------------------------------------------------------
// 8. Long chains
// ---------------------------------------------------------------------------

TEST(LinkedList, AMillionElementsWithoutRecursion) {
    constexpr int kN = 1000000;
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    const auto values = ints(ctx, 0, kN);
    const ProtoLinkedList* list = ctx->newLinkedListFromArray(values.size(), values.data());
    const ProtoLinkedList* same = ctx->newLinkedListFromArray(values.size(), values.data());
    ASSERT_EQ(list->getSize(ctx), static_cast<proto_ulong>(kN));

    long long sum = 0;
    ProtoLinkedListCursor cursor(list);
    while (cursor.hasNext()) sum += cursor.next()->asLong(ctx);
    EXPECT_EQ(sum, static_cast<long long>(kN) * (kN - 1) / 2);
    EXPECT_TRUE(list->isEqual(ctx, same));
    EXPECT_EQ(list->getHash(ctx), same->getHash(ctx));
    EXPECT_EQ(list->getAt(ctx, kN - 1)->asLong(ctx), kN - 1);
    EXPECT_EQ(list->reverse(ctx)->getFirst(ctx)->asLong(ctx), kN - 1);

    // Marking walks the chain: a cycle with the list pinned keeps it whole.
    ProtoRootSet* pins = space.createRootSet("linked-list-test");
    pins->add(list->asObject(ctx));
    const uint64_t before = space.getGCCycleCount();
    requestCycle(space);
    {
        ProtoContext::UnmanagedScope parked(ctx);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (space.getGCCycleCount() == before && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_GT(space.getGCCycleCount(), before);
    EXPECT_EQ(list->getAt(ctx, kN / 2)->asLong(ctx), kN / 2);
}

// ---------------------------------------------------------------------------
// 9. Concurrent readers
// ---------------------------------------------------------------------------

TEST(LinkedList, ConcurrentReadersSeeTheSameList) {
    constexpr int kThreads = 4;
    constexpr int kN = 10000;

    struct Shared {
        const ProtoLinkedList* list = nullptr;
        std::atomic<int> good{0};
    };
    static Shared* shared = nullptr;
    Shared state;
    shared = &state;

    ProtoSpace space;
    ProtoContext* root = space.rootContext;
    const auto values = ints(root, 0, kN);
    state.list = root->newLinkedListFromArray(values.size(), values.data());
    ProtoRootSet* pins = space.createRootSet("linked-list-test");
    pins->add(state.list->asObject(root));

    auto reader = [](ProtoContext* ctx, const ProtoObject*, const ParentLink*,
                     const ProtoArguments*, const ProtoKeywordArguments*) -> const ProtoObject* {
        for (int rep = 0; rep < 20; ++rep) {
            long long sum = 0;
            const ProtoLinkedList* l = shared->list;
            ProtoLinkedListCursor cursor(l);
            while (cursor.hasNext()) sum += cursor.next()->asLong(ctx);
            // Readers also build from the shared list: rest, prepend.
            const ProtoLinkedList* derived = l->getRest(ctx)->prepend(ctx, ctx->fromInteger(0));
            if (sum == static_cast<long long>(kN) * (kN - 1) / 2 && derived->isEqual(ctx, l))
                shared->good.fetch_add(1, std::memory_order_relaxed);
        }
        return PROTO_NONE;
    };

    std::vector<const ProtoThread*> handles;
    for (int t = 0; t < kThreads; ++t) {
        handles.push_back(space.newThread(root, ProtoString::createSymbol(root, "list-reader"),
                                          reader, nullptr, nullptr));
    }
    {
        ProtoContext::UnmanagedScope parked(root);
        for (const ProtoThread* h : handles) const_cast<ProtoThread*>(h)->join(root);
    }
    shared = nullptr;
    EXPECT_EQ(state.good.load(), kThreads * 20);
}

// ---------------------------------------------------------------------------
// 10. Object model
// ---------------------------------------------------------------------------

TEST(LinkedList, ObjectModelIntegration) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    const ProtoLinkedList* list = ctx->newLinkedList()->prepend(ctx, ctx->fromInteger(3));
    const ProtoObject* obj = list->asObject(ctx);
    EXPECT_TRUE(obj->isLinkedList(ctx));
    EXPECT_EQ(obj->asLinkedList(ctx), list);
    EXPECT_EQ(obj->asList(ctx), nullptr);
    EXPECT_FALSE(obj->isTuple(ctx));
    EXPECT_FALSE(ctx->fromInteger(3)->isLinkedList(ctx));
    EXPECT_EQ(ctx->newList()->asObject(ctx)->asLinkedList(ctx), nullptr);

    // A wrapper object whose __data__ is a list answers as the list.
    const ProtoObject* wrapper = ctx->newObject(true)->setAttribute(ctx, space.literalData, obj);
    EXPECT_TRUE(wrapper->isLinkedList(ctx));
    EXPECT_EQ(wrapper->asLinkedList(ctx), list);

    // The prototype is per space and replaceable.
    ASSERT_NE(space.getLinkedListPrototype(), nullptr);
    EXPECT_EQ(obj->getPrototype(ctx), space.getLinkedListPrototype());
    EXPECT_EQ(ctx->newLinkedList()->asObject(ctx)->getPrototype(ctx), space.getLinkedListPrototype());
    ProtoObject* custom = const_cast<ProtoObject*>(ctx->newObject(false));
    space.setLinkedListPrototype(custom);
    EXPECT_EQ(obj->getPrototype(ctx), custom);
}

// ---------------------------------------------------------------------------
// 11. ABI guard: the public layout of ProtoSpace is that of 3.2.0
// ---------------------------------------------------------------------------

// The values were measured on the 3.2.0 header (master bb2d581a) with
// libstdc++ on x86-64 Linux; other ABIs lay the class out differently.
#if defined(__GLIBCXX__) && defined(__x86_64__) && defined(__linux__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
TEST(LinkedList, ProtoSpaceLayoutIsUnchangedFrom320) {
    EXPECT_EQ(sizeof(ProtoSpace), 19584u);
    EXPECT_EQ(offsetof(ProtoSpace, mpscQueuePrototype), 248u);
    EXPECT_EQ(offsetof(ProtoSpace, literalData), 256u);
    EXPECT_EQ(offsetof(ProtoSpace, nonMethodCallback), 280u);
    EXPECT_EQ(offsetof(ProtoSpace, outOfMemoryCallback), 312u);
    EXPECT_EQ(offsetof(ProtoSpace, impl), 328u);
    EXPECT_EQ(offsetof(ProtoSpace, rootContext), 344u);
    EXPECT_EQ(offsetof(ProtoSpace, mutableRoot), 384u);
}
#pragma GCC diagnostic pop
#endif
