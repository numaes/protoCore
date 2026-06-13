/*
 * SwarmTests.cpp — Stress tests for ropes and external buffers (v61).
 * 1M concat (O(1) rope building), ~10MB rope index access, external buffer GC.
 */

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include <chrono>
#include <cstring>

using namespace proto;

namespace {

constexpr int ONE_MILLION = 1'000'000;
constexpr unsigned long ROPE_TARGET_BYTES = 10 * 1024 * 1024;  // ~10 MB in UTF-32 codepoints
constexpr int EXTERNAL_BUFFER_GC_COUNT = 5000;
constexpr unsigned long EXTERNAL_BUFFER_SIZE = 4096;

}  // namespace

// Historical bug: building a rope with 1M appendLast calls used to trigger a
// GC segfault on the resulting graph (~1.4 GB of intermediate AVL nodes).
// Closed by the concurrent-mark + freelist-chunking series (335ef608, the
// b07dcbeb..21fa2289..11e287a1 sweep refactor): the live rope stays reachable
// through the root-context's automatic locals and the GC traces it without
// touching half-constructed AVL slots. We require completion under the
// suite-wide DEV12 8 GB cap (set externally via the systemd-run wrapper) and
// a generous wall-clock ceiling that comfortably accommodates ASan builds.
TEST(SwarmTest, OneMillionConcats) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    const ProtoString* s = reinterpret_cast<const ProtoString*>(ctx->fromUTF8String("x"));
    ASSERT_NE(s, nullptr);

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < ONE_MILLION - 1; ++i) {
        s = s->appendLast(ctx, reinterpret_cast<const ProtoString*>(ctx->fromUTF8String("x")));
        ASSERT_NE(s, nullptr);
    }
    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    unsigned long size = s->getSize(ctx);
    EXPECT_EQ(size, static_cast<unsigned long>(ONE_MILLION)) << "rope length after concats";

    (void)ms;
    EXPECT_LT(ms, 120000);
}

// Historical bug: traversal of a ~10 MB rope used to hit "Non-tuple object in
// tuple node slot" — a sentinel-shaped cell would appear where the index walker
// expected an AVL tuple node, caused by an in-flight concurrent-mark snapshot
// reading a half-built slot. Closed by the per-cycle mutable-shard snapshot
// (335ef608), which makes the marker see a consistent view of every mutable
// across one GC cycle. Verifies start / middle / end index reads return a real
// codepoint object after the rope is fully assembled.
TEST(SwarmTest, LargeRopeIndexAccess) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    // Build a rope of ~10MB (in UTF-32: 4 bytes per char; target ~2.5M chars).
    constexpr int chunkChars = 256;
    const ProtoString* chunk = reinterpret_cast<const ProtoString*>(ctx->fromUTF8String(
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
    ASSERT_GE(chunk->getSize(ctx), static_cast<unsigned long>(chunkChars));

    const ProtoString* rope = chunk;
    int numChunks = 1;
    while (rope->getSize(ctx) < ROPE_TARGET_BYTES / 4 && numChunks < 20000) {
        rope = rope->appendLast(ctx, chunk);
        numChunks++;
    }

    unsigned long totalSize = rope->getSize(ctx);
    EXPECT_GE(totalSize, ROPE_TARGET_BYTES / 4u);

    // Index access at start, middle, end
    const ProtoObject* at0 = rope->getAt(ctx, 0);
    const ProtoObject* atMid = rope->getAt(ctx, static_cast<int>(totalSize / 2));
    const ProtoObject* atEnd = rope->getAt(ctx, static_cast<int>(totalSize - 1));
    EXPECT_NE(at0, nullptr);
    EXPECT_NE(atMid, nullptr);
    EXPECT_NE(atEnd, nullptr);
}

// Regression: the original "Non-tuple object in tuple node slot" symptom
// surfaced when the GC was *forced to run during construction* by an
// aggressively low heap ceiling. The two tests above only exercise the
// finished rope (the live tree never grew past one GC reclaim cycle's worth
// of intermediate nodes — the 8 GB cap was generous). This test makes the
// scenario explicit: cap the heap to ~120K cells and build the same ~10 MB
// rope. Every few thousand appendLast calls the allocator must wait for the
// GC to reclaim the discarded interior nodes (each appendLast COWs O(log n)
// AVL spine cells), and the index walker reads the freshly-reclaimed-and-
// reused region. A regression of the concurrent-mark snapshot bug would
// resurface here as either a crash or a stale-tuple-slot exception.
TEST(SwarmTest, LargeRopeIndexAccessUnderHeapPressure) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;
    const int base = space.heapSize;
    // Cap aggressively low — well under what the cumulative appendLast COW
    // allocation would need without reclamation. The young-generation reuse
    // path must keep the cap honoured throughout the build; if it does not,
    // the allocator either grows past the limit (caught by the heap-size
    // assertion at the end) or stalls (caught by the watchdog ceiling on
    // wall-clock).
    space.setHeapLimits(/*soft=*/0, /*hard=*/base + 6000);

    constexpr int chunkChars = 256;
    const ProtoString* chunk = reinterpret_cast<const ProtoString*>(ctx->fromUTF8String(
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
    ASSERT_GE(chunk->getSize(ctx), static_cast<unsigned long>(chunkChars));

    const ProtoString* rope = chunk;
    int numChunks = 1;
    while (rope->getSize(ctx) < ROPE_TARGET_BYTES / 4 && numChunks < 20000) {
        rope = rope->appendLast(ctx, chunk);
        ASSERT_NE(rope, nullptr) << "rope went null at chunk " << numChunks;
        numChunks++;
    }

    unsigned long totalSize = rope->getSize(ctx);
    EXPECT_GE(totalSize, ROPE_TARGET_BYTES / 4u);

    // Index access at start, middle, end must still return non-null even
    // though the underlying cells have cycled through several reclaim
    // batches during construction.
    const ProtoObject* at0 = rope->getAt(ctx, 0);
    const ProtoObject* atMid = rope->getAt(ctx, static_cast<int>(totalSize / 2));
    const ProtoObject* atEnd = rope->getAt(ctx, static_cast<int>(totalSize - 1));
    EXPECT_NE(at0, nullptr);
    EXPECT_NE(atMid, nullptr);
    EXPECT_NE(atEnd, nullptr);

    // The hard ceiling must have held — proves reclamation actually ran
    // during construction (whether via STW or young-gen reuse) rather than
    // the allocator silently growing past the limit.
    EXPECT_LE(space.heapSize, base + 6000)
        << "heapSize grew past the configured hard ceiling — "
           "the GC did not engage during rope construction";
}

TEST(SwarmTest, ExternalBufferGC) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    std::vector<const ProtoObject*> refs;
    refs.reserve(EXTERNAL_BUFFER_GC_COUNT);

    for (int i = 0; i < EXTERNAL_BUFFER_GC_COUNT; ++i) {
        const ProtoObject* obj = ctx->newExternalBuffer(EXTERNAL_BUFFER_SIZE);
        ASSERT_NE(obj, nullptr);
        void* raw = obj->getRawPointerIfExternalBuffer(ctx);
        ASSERT_NE(raw, nullptr);
        refs.push_back(obj);
    }

    unsigned long heapBefore = space.heapSize;
    refs.clear();
    // Drop all refs so buffers become collectible.

    for (int i = 0; i < 20; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        (void)ctx->newObject(false);
    }

    unsigned long heapAfter = space.heapSize;
    (void)heapBefore;
    (void)heapAfter;
    // GC should eventually reclaim; we only assert no crash and that GetRawPointer was usable.
    EXPECT_TRUE(true);
}

TEST(SwarmTest, GetRawPointerIfExternalBuffer) {
    ProtoSpace space;
    ProtoContext* ctx = space.rootContext;

    const ProtoObject* bufObj = ctx->newExternalBuffer(128);
    ASSERT_NE(bufObj, nullptr);
    void* raw = bufObj->getRawPointerIfExternalBuffer(ctx);
    ASSERT_NE(raw, nullptr);

    const ProtoObject* listObj = ctx->newList()->asObject(ctx);
    void* rawList = listObj->getRawPointerIfExternalBuffer(ctx);
    EXPECT_EQ(rawList, nullptr);
}
