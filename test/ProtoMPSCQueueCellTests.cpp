// ProtoMPSCQueueCellTests.cpp - the three cells of ProtoMPSCQueue:
// tag discipline, CellType, 64-byte budget, and what each one reports
// to the collector.  PMQ-SPEC section 3, section 4.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace proto;

namespace {

// Collects every cell a processReferences call reports, in order.
struct Collector {
    std::vector<const Cell*> refs;
};

void collect(ProtoContext*, void* self, const Cell* ref) {
    static_cast<Collector*>(self)->refs.push_back(ref);
}

unsigned long tagOf(const void* handle) {
    ProtoObjectPointer p{};
    p.oid = reinterpret_cast<const ProtoObject*>(handle);
    return p.op.pointer_tag;
}

}  // namespace

TEST(MPSCQueueCell, HandleCarriesTag28AndCellTypeMPSCQueue) {
    ProtoSpace space;
    ProtoContext ctx(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);

    const ProtoMPSCQueue* q = ctx.newMPSCQueue();
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(tagOf(q), static_cast<unsigned long>(POINTER_TAG_MPSC_QUEUE));
    EXPECT_EQ(POINTER_TAG_MPSC_QUEUE, 28);

    const Cell* cell = toImpl<const ProtoMPSCQueueImplementation>(q);
    EXPECT_EQ(cell->getType(), CellType::MPSCQueue);
}

TEST(MPSCQueueCell, EveryCellFitsInSixtyFourBytes) {
    EXPECT_LE(sizeof(ProtoMPSCQueueImplementation), 64u);
    EXPECT_LE(sizeof(ProtoMPSCQueueNodeImplementation), 64u);
    EXPECT_LE(sizeof(ProtoMPSCQueueRetainImplementation), 64u);
    EXPECT_LE(sizeof(BigCell), 64u);
}

// An empty queue reports nothing: both mutable words are null.
TEST(MPSCQueueCell, EmptyQueueReportsNoReference) {
    ProtoSpace space;
    ProtoContext ctx(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    const auto* impl = toImpl<const ProtoMPSCQueueImplementation>(ctx.newMPSCQueue());

    Collector c;
    impl->processReferences(&ctx, &c, collect);
    EXPECT_TRUE(c.refs.empty());
}

// The order is load-bearing (see the proof in core/ProtoMPSCQueue.cpp):
// `head` must be reported before `retained`.
TEST(MPSCQueueCell, ReportsHeadBeforeRetained) {
    ProtoSpace space;
    ProtoContext ctx(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    const ProtoMPSCQueue* q = ctx.newMPSCQueue();
    const auto* impl = toImpl<const ProtoMPSCQueueImplementation>(q);

    q->push(&ctx, ctx.fromInteger(1));
    (void) q->takeAll(&ctx);            // fills `retained`
    q->push(&ctx, ctx.fromInteger(2));  // refills `head`

    const Cell* head = reinterpret_cast<const Cell*>(impl->head.load());
    const Cell* retained = reinterpret_cast<const Cell*>(impl->retained.load());
    ASSERT_NE(head, nullptr);
    ASSERT_NE(retained, nullptr);

    Collector c;
    impl->processReferences(&ctx, &c, collect);
    ASSERT_EQ(c.refs.size(), 2u);
    EXPECT_EQ(c.refs[0], head);
    EXPECT_EQ(c.refs[1], retained);
}

// A node reports its item (when the item carries a cell) and its next.
TEST(MPSCQueueCell, NodeReportsItemAndNext) {
    ProtoSpace space;
    ProtoContext ctx(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    const ProtoMPSCQueue* q = ctx.newMPSCQueue();
    const auto* impl = toImpl<const ProtoMPSCQueueImplementation>(q);

    const ProtoObject* a = ctx.newList()->appendLast(&ctx, ctx.fromInteger(7))->asObject(&ctx);
    q->push(&ctx, a);
    const ProtoObject* b = ctx.newList()->appendLast(&ctx, ctx.fromInteger(8))->asObject(&ctx);
    q->push(&ctx, b);

    const ProtoMPSCQueueNodeImplementation* top = impl->head.load();
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(top->getType(), CellType::MPSCQueueNode);

    Collector c;
    top->processReferences(&ctx, &c, collect);
    ASSERT_EQ(c.refs.size(), 2u);
    EXPECT_EQ(c.refs[0], ProtoObject::asCellPointer(b));   // item (pushed last)
    EXPECT_EQ(c.refs[1], reinterpret_cast<const Cell*>(top->next.load()));
}

// An embedded item (SmallInteger, boolean, None, inline string) carries no
// cell and must not be reported: pushReportedReference aborts on a tagged
// pointer in instrumented and debug builds.
TEST(MPSCQueueCell, NodeDoesNotReportEmbeddedItems) {
    ProtoSpace space;
    ProtoContext ctx(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    const ProtoMPSCQueue* q = ctx.newMPSCQueue();
    const auto* impl = toImpl<const ProtoMPSCQueueImplementation>(q);

    q->push(&ctx, ctx.fromInteger(42));
    const ProtoMPSCQueueNodeImplementation* top = impl->head.load();
    ASSERT_NE(top, nullptr);

    Collector c;
    top->processReferences(&ctx, &c, collect);
    EXPECT_TRUE(c.refs.empty()) << "an embedded item must not be reported as a cell";
}

// Every reported reference must be non-null and untagged, or the GC's
// pushReportedReference aborts.
TEST(MPSCQueueCell, EveryReportedReferenceIsAnAlignedCell) {
    ProtoSpace space;
    ProtoContext ctx(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    const ProtoMPSCQueue* q = ctx.newMPSCQueue();
    const auto* impl = toImpl<const ProtoMPSCQueueImplementation>(q);

    for (int i = 0; i < 40; ++i)
        q->push(&ctx, ctx.newList()->appendLast(&ctx, ctx.fromInteger(i))->asObject(&ctx));
    (void) q->takeAll(&ctx);
    for (int i = 0; i < 40; ++i)
        q->push(&ctx, ctx.newList()->appendLast(&ctx, ctx.fromInteger(i))->asObject(&ctx));

    Collector c;
    impl->processReferences(&ctx, &c, collect);
    std::vector<const Cell*> work = c.refs;
    size_t seen = 0;
    while (!work.empty()) {
        const Cell* cell = work.back();
        work.pop_back();
        ASSERT_NE(cell, nullptr);
        EXPECT_EQ(reinterpret_cast<uintptr_t>(cell) & 0x3FUL, 0UL);
        ++seen;
        if (seen > 500) break;    // the chains are finite; guard a cycle bug
        Collector inner;
        cell->processReferences(&ctx, &inner, collect);
        work.insert(work.end(), inner.refs.begin(), inner.refs.end());
    }
    EXPECT_GT(seen, 80u);  // 80 nodes + 1 retain cell + the item lists
}

// D6: the internal cells are never exposed.  isObjectFast must reject
// them even though their implAsObject word carries tag 0.
TEST(MPSCQueueCell, InternalCellsAreNotObjectCells) {
    ProtoSpace space;
    ProtoContext ctx(&space, space.rootContext, nullptr, nullptr, nullptr, nullptr);
    const ProtoMPSCQueue* q = ctx.newMPSCQueue();
    const auto* impl = toImpl<const ProtoMPSCQueueImplementation>(q);
    q->push(&ctx, ctx.fromInteger(1));

    const ProtoMPSCQueueNodeImplementation* node = impl->head.load();
    ASSERT_NE(node, nullptr);
    const ProtoObject* word = node->implAsObject(&ctx);
    EXPECT_EQ(tagOf(word), static_cast<unsigned long>(POINTER_TAG_OBJECT));
    EXPECT_FALSE(isObjectFast(word));

    (void) q->takeAll(&ctx);
    const ProtoMPSCQueueRetainImplementation* retain = impl->retained.load();
    ASSERT_NE(retain, nullptr);
    const ProtoObject* rword = retain->implAsObject(&ctx);
    EXPECT_EQ(tagOf(rword), static_cast<unsigned long>(POINTER_TAG_OBJECT));
    EXPECT_FALSE(isObjectFast(rword));
}
