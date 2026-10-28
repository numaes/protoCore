// Attribute read/write microbenchmark, public API only (protoCore.h is the
// same on master and on explore/attr-list, so ONE binary runs against either
// library through LD_LIBRARY_PATH).
//
// For each n (own attributes) it measures, per operation: wall ns (median of
// 5 repetitions), user instructions and user cycles of the calling thread
// only (perf_event_open; the collector thread is excluded), and cells
// allocated (ProtoContext::allocatedCellsCount; run with a large
// PROTOCORE_GC_CONTEXT_THRESHOLD so the counter is not reset mid-run).
//
// Positions are by insertion order: keys k0..k(n-1) are written in order, so
// "new" = k(n-1) (the list's head), "mid" = k(n/2), "old" = k0 (the list's
// tail).  The AVL places keys by address, so for it the names only identify
// the same keys.
//
// Every loop accumulates a checksum that is printed and verified.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "protoCore.h"
#include "perfcount.h"
using namespace proto;
using clk = std::chrono::steady_clock;

static ThreadCounters* g_pc;
// The context every operation runs in.  Each measured repetition runs in a
// fresh child context, destroyed afterwards, so the cells it allocated
// become collectable (a live context's young chain is traced).
static ProtoContext* g_cur = nullptr;
struct ChildScope {
    ProtoContext child;
    ProtoContext* saved;
    explicit ChildScope(ProtoContext* parent)
        : child(parent->space, parent, nullptr, nullptr, nullptr, nullptr), saved(g_cur) { g_cur = &child; }
    ~ChildScope() { g_cur = saved; }
};
static int g_fail = 0;

struct R { double ns, in, cy, cells; };

// `body(rep)` runs `ops` operations; returns a checksum.
// Cells per operation, counted one operation at a time: the counter is
// reset when the context submits its young chain, so a step whose delta is
// negative is skipped.
template <class G> static double cellsPerOp(ProtoContext* c, G one) {
    double sum = 0; long cnt = 0;
    for (long i = 0; i < 4000; ++i) {
        proto_ulong a0 = c->allocatedCellsCount;
        one(i);
        proto_ulong a1 = c->allocatedCellsCount;
        if (a1 >= a0) { sum += double(a1 - a0); ++cnt; }
    }
    return cnt ? sum / cnt : -1;
}

template <class F, class G> static R measure(ProtoContext* c, long ops, long long expect, const char* what, F body, G one) {
    std::vector<double> t, in, cy;
    double cells;
    { ChildScope cs(c); cells = cellsPerOp(g_cur, one); }
    for (int k = 0; k < 5; ++k) {
        uint64_t i0, c0;
        ChildScope cs(c);
        g_pc->start();
        auto a = clk::now();
        long long got = body(k);
        auto b = clk::now();
        g_pc->stop(i0, c0);
        if (got != expect) {
            std::printf("CHECK FAILED %s: got %lld expected %lld\n", what, got, expect);
            ++g_fail;
        }
        t.push_back(std::chrono::duration<double, std::nano>(b - a).count() / ops);
        in.push_back(double(i0) / ops);
        cy.push_back(double(c0) / ops);
    }
    std::sort(t.begin(), t.end()); std::sort(in.begin(), in.end()); std::sort(cy.begin(), cy.end());
    return {t[2], in[2], cy[2], cells};
}

static void row(int n, const char* op, R r) {
    std::printf("%4d %-22s %9.1f ns %9.0f in %9.0f cy %7.2f cells\n", n, op, r.ns, r.in, r.cy, r.cells);
    std::fflush(stdout);
}

int main(int argc, char** argv) {
    std::vector<int> ns = {1, 2, 4, 8, 12, 16, 24, 32, 64, 100};
    if (argc > 1) { ns.clear(); for (int i = 1; i < argc; ++i) ns.push_back(std::atoi(argv[i])); }
    ThreadCounters pc; g_pc = &pc;
    ProtoSpace space;
    g_cur = space.rootContext;
    ProtoContext*& c = g_cur;
    ProtoRootSet* roots = space.createRootSet("attrbench");
    std::vector<const ProtoString*> keys;
    for (int i = 0; i < 128; ++i)
        keys.push_back(ProtoString::fromUTF8String(c, ("key" + std::to_string(i)).c_str())->intern(c));
    const ProtoString* missKey = ProtoString::fromUTF8String(c, "inherited")->intern(c);
    std::printf("PROTOCORE_ATTR_LIST_MAX=%s PROTOCORE_ATTR_LIST_PACK=%s ADAPTIVE=%s\n",
                std::getenv("PROTOCORE_ATTR_LIST_MAX") ? std::getenv("PROTOCORE_ATTR_LIST_MAX") : "(unset)",
                std::getenv("PROTOCORE_ATTR_LIST_PACK") ? std::getenv("PROTOCORE_ATTR_LIST_PACK") : "(unset)",
                std::getenv("PROTOCORE_ADAPTIVE_HEAP") ? std::getenv("PROTOCORE_ADAPTIVE_HEAP") : "(unset)");

    const ProtoObject* parent = c->newObject(false)->setAttribute(c, missKey, c->fromInteger(7));
    roots->add(parent);

    for (int n : ns) {
        // The base object: n attributes written in key order, values 0..n-1.
        const ProtoObject* base = parent->newChild(c, false);
        for (int i = 0; i < n; ++i) base = base->setAttribute(c, keys[i], c->fromInteger(i));
        auto hb = roots->add(base);
        const int pos[3] = {n - 1, n / 2, 0};
        const char* pname[3] = {"new", "mid", "old"};

        // Cells to build the object (n successive writes on a fresh child).
        {
            proto_ulong a0 = c->allocatedCellsCount;
            const ProtoObject* o = parent->newChild(c, false);
            for (int i = 0; i < n; ++i) o = o->setAttribute(c, keys[i], c->fromInteger(i));
            proto_ulong a1 = c->allocatedCellsCount;
            std::printf("%4d %-22s %9.2f cells (incl. object + empty tree)\n", n, "build_total", double(a1 - a0));
        }

        // Reads, attribute cache hot: one object, one key, repeated.
        const long RH = 2000000;
        for (int p = 0; p < 3; ++p) {
            const ProtoString* k = keys[pos[p]];
            R r = measure(c, RH, (long long)pos[p] * RH, "read_hot", [&](int) {
                long long s = 0;
                for (long i = 0; i < RH; ++i) s += base->getAttribute(c, k)->asLong(c);
                return s;
            }, [&](long) { (void) base->getAttribute(c, k); });
            char name[32]; std::snprintf(name, sizeof name, "read_hot_%s", pname[p]);
            row(n, name, r);
        }

        // Reads, attribute cache cold: 8192 objects of the same shape read
        // round-robin (the cache has 1024 entries).
        const int NO = 8192;
        std::vector<const ProtoObject*> objs(NO);
        const ProtoList* holder = c->newList();
        for (int j = 0; j < NO; ++j) {
            const ProtoObject* o = parent->newChild(c, false);
            for (int i = 0; i < n; ++i) o = o->setAttribute(c, keys[i], c->fromInteger(i));
            objs[j] = o;
            holder = holder->appendLast(c, o);
        }
        auto hh = roots->add(holder->asObject(c));
        const long RC = 2000000;
        for (int p = 0; p < 3; ++p) {
            const ProtoString* k = keys[pos[p]];
            R r = measure(c, RC, (long long)pos[p] * RC, "read_cold", [&](int) {
                long long s = 0;
                for (long i = 0; i < RC; ++i) s += objs[(i * 4099) & (NO - 1)]->getAttribute(c, k)->asLong(c);
                return s;
            }, [&](long i) { (void) objs[(i * 4099) & (NO - 1)]->getAttribute(c, k); });
            char name[32]; std::snprintf(name, sizeof name, "read_cold_%s", pname[p]);
            row(n, name, r);
        }
        // Miss on the own attributes, found in the parent (one chain step).
        {
            R r = measure(c, RC, 7LL * RC, "miss_cold", [&](int) {
                long long s = 0;
                for (long i = 0; i < RC; ++i) s += objs[(i * 4099) & (NO - 1)]->getAttribute(c, missKey)->asLong(c);
                return s;
            }, [&](long i) { (void) objs[(i * 4099) & (NO - 1)]->getAttribute(c, missKey); });
            row(n, "miss_chain_cold", r);
            R h = measure(c, RH, 7LL * RH, "miss_hot", [&](int) {
                long long s = 0;
                for (long i = 0; i < RH; ++i) s += base->getAttribute(c, missKey)->asLong(c);
                return s;
            }, [&](long) { (void) base->getAttribute(c, missKey); });
            row(n, "miss_chain_hot", h);
        }
        roots->remove(hh);
        objs.clear();

        // Writes on an immutable object, each from `base` (positions stay put).
        const long W = 400000;
        {
            const ProtoString* k = keys[n];   // a new name
            R r = measure(c, W, (long long)(n + 1) * W, "imm_new", [&](int) {
                long long s = 0;
                for (long i = 0; i < W; ++i) {
                    const ProtoObject* o = base->setAttribute(c, k, c->fromInteger(i));
                    s += (o != base) ? n + 1 : 0;
                }
                return s;
            }, [&](long i) { (void) base->setAttribute(c, k, c->fromInteger(i)); });
            row(n, "imm_write_new", r);
        }
        for (int p = 0; p < 3; ++p) {
            const ProtoString* k = keys[pos[p]];
            R r = measure(c, W, (long long)W, "imm_over", [&](int) {
                long long s = 0;
                for (long i = 0; i < W; ++i) {
                    const ProtoObject* o = base->setAttribute(c, k, c->fromInteger(1000 + (i & 1)));
                    s += (o != base);
                }
                return s;
            }, [&](long i) { (void) base->setAttribute(c, k, c->fromInteger(1000 + (i & 1))); });
            char name[32]; std::snprintf(name, sizeof name, "imm_over_%s", pname[p]);
            row(n, name, r);
        }

        // The floor's pattern (measured before any clone exists, so the mutable
        // table holds only this object's entry): one mutable object, keys written round-robin
        // (each write hits the least recently written key: the list's tail).
        {
            const ProtoObject* mo = base->clone(c, true);
            auto hm = roots->add(mo);
            R r = measure(c, W, (long long)(W - 1), "mut_cycle", [&](int) {
                for (long i = 0; i < W; ++i) mo->setAttribute(c, keys[i % n], c->fromInteger(i));
                return (long long)mo->getAttribute(c, keys[(W - 1) % n])->asLong(c);
            }, [&](long i) { mo->setAttribute(c, keys[i % n], c->fromInteger(i)); });
            row(n, "mut_write_cycle", r);
            // The same key over and over (a hot field: the list's head after
            // the first write).
            R r2 = measure(c, W, (long long)(W - 1), "mut_same", [&](int) {
                for (long i = 0; i < W; ++i) mo->setAttribute(c, keys[0], c->fromInteger(i));
                return (long long)mo->getAttribute(c, keys[0])->asLong(c);
            }, [&](long i) { mo->setAttribute(c, keys[0], c->fromInteger(i)); });
            row(n, "mut_write_samekey", r2);
            roots->remove(hm);
        }
        // Writes on mutable objects: fresh mutable clones of `base` (built
        // outside the timed loop, 1 cell each), one write per clone, so the
        // position is that of `base`.  Each clone is a new entry of the
        // mutable table (20,000 live ones per repetition), whose path copy
        // is part of every one of these writes: the table, not the
        // attributes, dominates their cells.
        const int MB = 20000;
        std::vector<const ProtoObject*> clones(MB);
        auto mutWrite = [&](const char* name, const ProtoString* k, long long expectPer) {
            std::vector<double> t, in, cy;
            double cells = -1;
            for (int rep = 0; rep < 5; ++rep) {
                ChildScope scope(c);
                const ProtoList* hold = c->newList();
                for (int j = 0; j < MB; ++j) { clones[j] = base->clone(c, true); hold = hold->appendLast(c, clones[j]); }
                auto h = roots->add(hold->asObject(c));
                uint64_t i0, c0;
                if (rep == 0) {
                    // Cells: the first 4,000 writes of this repetition one
                    // at a time (outside the timed loop: they also count).
                    double sum = 0; long cnt = 0;
                    for (int j = 0; j < 4000; ++j) {
                        proto_ulong x0 = c->allocatedCellsCount;
                        clones[j]->setAttribute(c, k, c->fromInteger(5000 + j));
                        proto_ulong x1 = c->allocatedCellsCount;
                        if (x1 >= x0) { sum += double(x1 - x0); ++cnt; }
                    }
                    cells = cnt ? sum / cnt : -1;
                    // Fresh clones for the timed loop.
                    for (int j = 0; j < 4000; ++j) { clones[j] = base->clone(c, true); hold = hold->appendLast(c, clones[j]); }
                    roots->remove(h);
                    h = roots->add(hold->asObject(c));
                }
                g_pc->start();
                auto a = clk::now();
                for (int j = 0; j < MB; ++j) clones[j]->setAttribute(c, k, c->fromInteger(5000 + j));
                auto b = clk::now();
                g_pc->stop(i0, c0);
                long long s = 0;
                for (int j = 0; j < MB; ++j) s += clones[j]->getAttribute(c, k)->asLong(c) - 5000 - j + expectPer;
                if (s != expectPer * MB) { std::printf("CHECK FAILED %s\n", name); ++g_fail; }
                t.push_back(std::chrono::duration<double, std::nano>(b - a).count() / MB);
                in.push_back(double(i0) / MB); cy.push_back(double(c0) / MB);
                roots->remove(h);
            }
            std::sort(t.begin(), t.end()); std::sort(in.begin(), in.end()); std::sort(cy.begin(), cy.end());
            row(n, name, {t[2], in[2], cy[2], cells});
        };
        mutWrite("mut_write_new", keys[n], 1);
        for (int p = 0; p < 3; ++p) {
            char name[32]; std::snprintf(name, sizeof name, "mut_over_%s", pname[p]);
            mutWrite(name, keys[pos[p]], 1);
        }

        roots->remove(hb);
    }
    std::printf("RESULT %s\n", g_fail ? "FAILED" : "OK");
    return g_fail ? 1 : 0;
}
