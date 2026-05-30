// ConcurrentMarkSafetyTests.cpp — verify the mutable-shard snapshot is
// sufficient for safe concurrent mark.
//
// As of the commit that introduces docs/STW_ELIMINATION_RESEARCH.md § 13,
// the GC mark phase runs OUTSIDE the Stop-The-World window.  Workers may
// CAS-swap mutableRoot[] shards while the marker is traversing the heap.
// The marker stays correct by consulting the per-cycle snapshot
// `gcMutableSnapshot[]`, captured atomically under STW at Phase 2.
//
// These tests exercise that invariant: many threads do high-frequency
// setAttribute / getAttribute on mutable objects, with the GC repeatedly
// triggered in parallel.  Failures of the snapshot discipline manifest as
// crashes, lost writes, or use-after-free (visible under AddressSanitizer).
// The tests pass cleanly under both Release and ASan builds.

#include <gtest/gtest.h>
#include "../headers/protoCore.h"
#include "../headers/proto_internal.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace proto;

namespace {

const ProtoString* sym(ProtoContext* ctx, const char* s) {
    return ProtoString::createSymbol(ctx, s);
}

}  // namespace

// Many worker threads each rotate through a shared pool of mutable objects,
// installing a fresh integer value and reading it back immediately.  A
// dedicated GC-kicker thread fires triggerGC() in parallel, so every
// setAttribute is racing the concurrent mark for the corresponding shard.
//
// What this catches: if mark dereferenced live mutableRoot[s] (instead of
// the snapshot), a CAS by a worker between mark's load and mark's traverse
// could leave the new SparseList unmarked while the marker visits the old
// one.  Sweep would then free cells the worker still references, and the
// next read would surface garbage — observed either as a crash, an ASan
// use-after-free, or (in benign cases) an unexpected value on read-back.
//
// With the snapshot discipline in place, every shard the marker visits is
// the STW-time snapshot; concurrent CAS by workers never affects it.
TEST(ConcurrentMarkSafety, MutationDuringMarkPreservesValues) {
    ProtoSpace space;
    ProtoContext* setupCtx = space.rootContext;

    constexpr int kPoolSize       = 64;
    constexpr int kThreads        = 4;
    constexpr int kIterPerThread  = 20000;

    std::vector<ProtoObject*> pool;
    pool.reserve(kPoolSize);
    for (int i = 0; i < kPoolSize; ++i) {
        pool.push_back(const_cast<ProtoObject*>(setupCtx->newObject(true)));
    }
    const ProtoString* slot = sym(setupCtx, "slot");

    std::atomic<bool> stopGc{false};
    std::atomic<unsigned long> mismatchCount{0};

    std::thread gcKicker([&]() {
        while (!stopGc.load(std::memory_order_relaxed)) {
            space.triggerGC();
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t]() {
            ProtoContext threadCtx{&space};
            for (int i = 0; i < kIterPerThread; ++i) {
                ProtoObject* obj = pool[(i + t * 17) % kPoolSize];
                const long long val = static_cast<long long>(t) * 1000000LL + i;
                const ProtoObject* v = threadCtx.fromInteger(val);
                obj->setAttribute(&threadCtx, slot, v);

                // Read it back immediately on the same thread.  Mutable
                // attribute reads go through the per-thread cache which is
                // also refreshed on the same CAS, so we expect the value
                // we just wrote — unless a concurrent CAS by another
                // worker overwrote it between our setAttribute and our
                // getAttribute.  We tolerate that (it is not a safety
                // failure) by ONLY counting mismatches when the read
                // value cannot be parsed as a valid integer, which would
                // indicate the cell was freed and overwritten.
                const ProtoObject* read = obj->getAttribute(&threadCtx, slot);
                if (!read || !read->isInteger(&threadCtx)) {
                    mismatchCount.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& th : workers) th.join();
    stopGc.store(true, std::memory_order_relaxed);
    gcKicker.join();

    // Any non-integer read indicates the SparseList carrying the slot was
    // freed while still reachable — i.e., the snapshot did not protect it.
    EXPECT_EQ(mismatchCount.load(), 0u)
        << "the snapshot must keep every mutable cell reachable from a STW "
           "root alive across a concurrent mark cycle";
}

// A second variant: build a chain of mutable references and verify the
// chain stays intact across many cycles of mark + concurrent mutation.
//
// The chain shape:  root.next == m1, m1.next == m2, m2.next == m3.
// A worker thread continually mutates `m1.next` to alternate between m2
// and a freshly-allocated decoy.  Mark must always see m2 reachable from
// root via the snapshot — never lose the chain.
TEST(ConcurrentMarkSafety, NoLostMutableReferences) {
    ProtoSpace space;
    ProtoContext* setupCtx = space.rootContext;

    auto* root = const_cast<ProtoObject*>(setupCtx->newObject(true));
    auto* m1   = const_cast<ProtoObject*>(setupCtx->newObject(true));
    auto* m2   = const_cast<ProtoObject*>(setupCtx->newObject(true));
    auto* m3   = const_cast<ProtoObject*>(setupCtx->newObject(true));

    const ProtoString* nextKey = sym(setupCtx, "next");
    const ProtoString* tagKey  = sym(setupCtx, "tag");

    m2->setAttribute(setupCtx, nextKey, m3);
    m1->setAttribute(setupCtx, nextKey, m2);
    root->setAttribute(setupCtx, nextKey, m1);
    m3->setAttribute(setupCtx, tagKey, setupCtx->fromInteger(0xCAFE));

    std::atomic<bool> stopGc{false};
    std::atomic<bool> stopFlip{false};
    std::atomic<unsigned long> brokenChain{0};

    std::thread gcKicker([&]() {
        while (!stopGc.load(std::memory_order_relaxed)) {
            space.triggerGC();
            std::this_thread::sleep_for(std::chrono::microseconds(150));
        }
    });

    std::thread flipper([&]() {
        ProtoContext flipCtx{&space};
        bool useM2 = true;
        while (!stopFlip.load(std::memory_order_relaxed)) {
            // Allocate a fresh decoy on every flip — cycles a lot of memory
            // through Phase 5 sweep, exercising the snapshot vs. live race.
            const ProtoObject* decoy = flipCtx.newObject(false);
            m1->setAttribute(&flipCtx, nextKey, useM2 ? m2 : decoy);
            useM2 = !useM2;
        }
        // Leave the chain in the m2 state so the verifier can read m3.
        m1->setAttribute(&flipCtx, nextKey, m2);
    });

    // Verifier thread: chase the chain repeatedly.  Every time the chain
    // points back at m2, m3.tag must be readable and equal to 0xCAFE.
    // If snapshotting is broken, m2 or m3 may be freed mid-mark and the
    // tag read will return garbage / null / crash.
    {
        ProtoContext verifyCtx{&space};
        constexpr int kVerifyRounds = 30000;
        for (int i = 0; i < kVerifyRounds; ++i) {
            const ProtoObject* a = root->getAttribute(&verifyCtx, nextKey);
            if (a != m1) { brokenChain.fetch_add(1); continue; }
            const ProtoObject* b = m1->getAttribute(&verifyCtx, nextKey);
            if (b != m2) {
                // Flipper installed the decoy at this instant; legitimate.
                continue;
            }
            const ProtoObject* c = m2->getAttribute(&verifyCtx, nextKey);
            if (c != m3) { brokenChain.fetch_add(1); continue; }
            const ProtoObject* tag = m3->getAttribute(&verifyCtx, tagKey);
            if (!tag || !tag->isInteger(&verifyCtx)
                     || tag->asLong(&verifyCtx) != 0xCAFE) {
                brokenChain.fetch_add(1);
            }
        }
    }

    stopFlip.store(true, std::memory_order_relaxed);
    flipper.join();
    stopGc.store(true, std::memory_order_relaxed);
    gcKicker.join();

    EXPECT_EQ(brokenChain.load(), 0u)
        << "mutable reference chain must remain readable across concurrent "
           "mark cycles — snapshot must root the m1->m2->m3 graph";
}
