// Phase 1: decomposition of one attribute write on unmodified protoCore.
//
// The floor scenario (object_write_only): one object with 100 attributes,
// key i % 100 overwritten in a loop.  The full write is measured for a
// mutable and an immutable object, then each component in isolation using
// protoCore's internal API (the same calls setAttribute makes):
//
//   avl_replace      ProtoSparseListImplementation::implSetAt on the 100-key
//                    tree (value replacement: root-to-key path copy)
//   avl_insert       implSetAt of a NEW key into an n-key tree (path copy +
//                    rebalancing), n = 100
//   object_cell      new ProtoObjectCell(parent, attributes, 0)
//   shard_setAt      the mutable table update: setAt(mutable_ref, state) on a
//                    shard sparse list holding M entries
//   cache_inval      the attribute-cache invalidation code of setAttribute
//   bare_alloc       one empty sparse-list cell (allocation + trivial ctor)
//   crit_section     enter/leave a CriticalSection
//
// Every row prints ns/op (wall, median of 5), user instructions/op and user
// cycles/op of the mutator thread only, and cells allocated per op.  Each
// loop's result is checked so a silent failure cannot look like a win.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "proto_internal.h"
#include "perfcount.h"
using namespace proto;
using clk = std::chrono::steady_clock;

static ThreadCounters* g_pc;

struct Result { double ns, instr, cycles, cells; };

static bool g_ran = false;
static const char* g_only = nullptr;
static bool want(const char* tag) { return !g_only || std::strncmp(tag, g_only, std::strlen(g_only)) == 0; }
template <class F> static Result measure(const char* tag, ProtoContext* c, long ops, F f) {
    g_ran = want(tag);
    if (!g_ran) return {0, 0, 0, 0};
    std::vector<double> t, in, cy;
    double cells = 0;
    for (int k = 0; k < 5; ++k) {
        uint64_t i0, c0;
        proto_ulong a0 = c->allocatedCellsCount;
        g_pc->start();
        auto a = clk::now(); f(); auto b = clk::now();
        g_pc->stop(i0, c0);
        proto_ulong a1 = c->allocatedCellsCount;
        if (a1 >= a0 && k == 0) cells = double(a1 - a0) / ops;
        t.push_back(std::chrono::duration<double, std::nano>(b - a).count() / ops);
        in.push_back(double(i0) / ops);
        cy.push_back(double(c0) / ops);
    }
    std::sort(t.begin(), t.end()); std::sort(in.begin(), in.end()); std::sort(cy.begin(), cy.end());
    return {t[2], in[2], cy[2], cells};
}
static void report(const char* name, Result r, long long check) {
    if (!g_ran) return;
    std::printf("%-34s %8.1f ns  %8.0f instr  %8.0f cyc  %6.2f cells  check=%lld\n",
                name, r.ns, r.instr, r.cycles, r.cells, check);
    std::fflush(stdout);
}

int main(int argc, char** argv) {
    const long K = argc > 1 ? std::atol(argv[1]) : 1000000;
    if (argc > 2) g_only = argv[2];
    ThreadCounters pc; g_pc = &pc;
    ProtoSpace space;
    ProtoContext* c = space.rootContext;
    ProtoRootSet* roots = space.createRootSet("decomp");
    std::vector<const ProtoString*> keys;
    for (int i = 0; i < 200; ++i)
        keys.push_back(ProtoString::fromUTF8String(c, ("k" + std::to_string(i)).c_str())->intern(c));
    std::printf("heap mode: PROTOCORE_ADAPTIVE_HEAP=%s  ops/rep=%ld\n",
                std::getenv("PROTOCORE_ADAPTIVE_HEAP") ? std::getenv("PROTOCORE_ADAPTIVE_HEAP") : "(unset)", K);

    long long chk = 0;
    // W1: full mutable write (floor).
    const ProtoObject* mo = c->newObject(true);
    roots->add(mo);
    for (int i = 0; i < 100; ++i) mo->setAttribute(c, keys[i], c->fromInteger(i));
    Result r = measure("W1", c, K, [&] {
        for (long i = 0; i < K; ++i) mo->setAttribute(c, keys[i % 100], c->fromInteger(i));
        chk = mo->getAttribute(c, keys[(K - 1) % 100])->asLong(c);
    });
    report("W1 setAttribute mutable (n=100)", r, chk);
    if (g_ran && chk != K - 1) { std::printf("CHECK FAILED\n"); return 1; }

    // W2: full immutable write.
    const ProtoObject* io = c->newObject(false);
    for (int i = 0; i < 100; ++i) io = io->setAttribute(c, keys[i], c->fromInteger(i));
    auto ioH = roots->add(io);
    r = measure("W2", c, K, [&] {
        const ProtoObject* o = io;
        for (long i = 0; i < K; ++i) o = o->setAttribute(c, keys[i % 100], c->fromInteger(i));
        chk = o->getAttribute(c, keys[(K - 1) % 100])->asLong(c);
    });
    report("W2 setAttribute immutable (n=100)", r, chk);
    if (g_ran && chk != K - 1) { std::printf("CHECK FAILED\n"); return 1; }

    const ProtoObjectCell* ioc = toImpl<const ProtoObjectCell>(io);
    const ProtoSparseListImplementation* attrs = ioc->attributes;

    // C1: AVL value replacement on the 100-key tree.
    r = measure("C1", c, K, [&] {
        long long s = 0;
        for (long i = 0; i < K; ++i) {
            ProtoContext::CriticalSection cs(c);
            const ProtoSparseListImplementation* n =
                attrs->implSetAt(c, reinterpret_cast<uintptr_t>(keys[i % 100]), c->fromInteger(i + 1));
            s += n->size;
        }
        chk = s / K;
    });
    report("C1 AVL replace (n=100)", r, chk);
    if (g_ran && chk != 100) { std::printf("CHECK FAILED\n"); return 1; }

    // C1b: AVL insert of a new key into the 100-key tree.
    r = measure("C1b", c, K, [&] {
        long long s = 0;
        for (long i = 0; i < K; ++i) {
            ProtoContext::CriticalSection cs(c);
            const ProtoSparseListImplementation* n =
                attrs->implSetAt(c, reinterpret_cast<uintptr_t>(keys[100 + i % 100]), c->fromInteger(i));
            s += n->size;
        }
        chk = s / K;
    });
    report("C1b AVL insert (n=100 -> 101)", r, chk);
    if (g_ran && chk != 101) { std::printf("CHECK FAILED\n"); return 1; }

    // C2: the new ProtoObjectCell around an existing attribute tree.
    r = measure("C2", c, K, [&] {
        long long s = 0;
        for (long i = 0; i < K; ++i) {
            ProtoContext::CriticalSection cs(c);
            auto* oc = new(c) ProtoObjectCell(c, ioc->parent, attrs, 0);
            s += (oc->attributes == attrs);
        }
        chk = s;
    });
    report("C2 new ProtoObjectCell", r, chk);
    if (g_ran && chk != K) { std::printf("CHECK FAILED\n"); return 1; }

    // C3: the mutable-table update on a shard with M entries.
    for (int M : {1, 4, 64, 400}) {
        const ProtoSparseList* shard = c->newSparseList();
        for (int j = 0; j < M; ++j) shard = shard->setAt(c, (proto_ulong)(j * 256 + 7), io);
        auto h = roots->add(shard->asObject(c));
        const proto_ulong key = (proto_ulong)((M / 2) * 256 + 7);
        const ProtoObject* states[2] = {io, mo};
        r = measure("C3", c, K, [&] {
            long long s = 0;
            for (long i = 0; i < K; ++i) {
                ProtoContext::CriticalSection cs(c);
                const ProtoSparseList* n = shard->setAt(c, key, states[i & 1]);
                s += n->getSize(c);
            }
            chk = s / K;
        });
        char name[64]; std::snprintf(name, sizeof name, "C3 shard setAt (M=%d)", M);
        report(name, r, chk);
        if (g_ran && chk != M) { std::printf("CHECK FAILED\n"); return 1; }
        roots->remove(h);
    }

    // C4: attribute-cache invalidation, the exact code of setAttribute.
    r = measure("C4", c, K, [&] {
        long long s = 0;
        for (long i = 0; i < K; ++i) {
            const ProtoString* name = keys[i % 100];
            if (c->thread) {
                auto* threadImpl = toImpl<ProtoThreadImplementation>(c->thread);
                if (threadImpl->extension) {
                    proto_ulong hash_idx = ((reinterpret_cast<uintptr_t>(mo) >> 6) ^
                                            (reinterpret_cast<uintptr_t>(name) >> 4)) % THREAD_CACHE_DEPTH;
                    if (threadImpl->extension->attributeCache[hash_idx].object == mo &&
                        threadImpl->extension->attributeCache[hash_idx].name == name) {
                        threadImpl->extension->attributeCache[hash_idx] = {nullptr, nullptr, nullptr, nullptr};
                    }
                    s += 1;
                }
            }
            asm volatile("" ::: "memory");
        }
        chk = s;
    });
    report("C4 cache invalidation", r, chk);

    // C5: one bare cell.
    r = measure("C5", c, K, [&] {
        long long s = 0;
        for (long i = 0; i < K; ++i) {
            ProtoContext::CriticalSection cs(c);
            s += c->newSparseListImpl()->isEmpty;
        }
        chk = s;
    });
    report("C5 bare cell alloc", r, chk);
    if (g_ran && chk != K) { std::printf("CHECK FAILED\n"); return 1; }

    // C6: critical section enter/leave alone.
    r = measure("C6", c, K, [&] {
        long long s = 0;
        for (long i = 0; i < K; ++i) {
            ProtoContext::CriticalSection cs(c);
            s += c->criticalSectionDepth;
            asm volatile("" ::: "memory");
        }
        chk = s;
    });
    report("C6 critical section", r, chk);
    if (g_ran && chk != K) { std::printf("CHECK FAILED\n"); return 1; }

    // C7: the boxing in the loop (fromInteger) plus the loop itself.
    r = measure("C7", c, K, [&] {
        long long s = 0;
        for (long i = 0; i < K; ++i) {
            s += (long long)reinterpret_cast<uintptr_t>(c->fromInteger(i)) & 1;
            asm volatile("" ::: "memory");
        }
        chk = s;
    });
    report("C7 loop + fromInteger", r, chk);
    roots->remove(ioH);
    return 0;
}
