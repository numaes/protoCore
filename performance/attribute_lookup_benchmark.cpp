/*
 * attribute_lookup_benchmark.cpp
 *
 * Attribute-lookup baseline for protoCore vs. native C++ field access.
 * Scenarios: own attributes (cache-hot and over a working set larger than
 * the per-thread AttributeCache), inherited attributes by chain depth, and
 * mutable objects (read-only and write+read).  Every scenario verifies a
 * checksum; a mismatch aborts the run.
 *
 * Usage: attribute_lookup_benchmark [own|inh|mut|sweep|ownhot|owncold|inhhot|inhcold]
 *   (no argument runs own, inh and mut)
 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>
#include <algorithm>
#include "../headers/protoCore.h"

using namespace proto;
using Clock = std::chrono::steady_clock;

static const int REPS = 5;
static const long OPS = 4000000;
static const int COLD_OBJECTS = 16384;   // 16x the 1024-entry attribute cache

static ProtoRootSet* g_roots;

static void pin(ProtoContext* c, const std::vector<const ProtoObject*>& objs) {
    const ProtoList* l = c->newList();
    for (auto* o : objs) l = l->appendLast(c, o);
    g_roots->add(reinterpret_cast<const ProtoObject*>(l));
}

static std::vector<const ProtoString*> names(ProtoContext* c, int n) {
    std::vector<const ProtoString*> v;
    for (int i = 0; i < n; ++i)
        v.push_back(ProtoString::createSymbol(c, ("attr_" + std::to_string(i)).c_str()));
    return v;
}

static std::vector<int> order(int nobj, long ops) {
    std::mt19937 rng(42);
    std::vector<int> o(ops);
    for (long i = 0; i < ops; ++i) o[i] = rng() % nobj;
    return o;
}

template <class F>
static double best_ns(F body, long expected) {
    double best = 1e30;
    for (int r = 0; r < REPS; ++r) {
        auto t0 = Clock::now();
        long sum = body();
        double ns = std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / OPS;
        if (sum != expected) { std::fprintf(stderr, "CHECKSUM MISMATCH %ld != %ld\n", sum, expected); std::exit(1); }
        best = std::min(best, ns);
    }
    return best;
}

static void report(const char* scen, int param, double ns, long ops) {
    std::printf("%-34s %4d  %8.2f ns/op  (ops=%ld, checksum ok)\n", scen, param, ns, ops);
}

// ---------- protoCore: own attributes, immutable ----------
static void own_immutable(ProtoContext* c, int nattr, int nobj, const char* label) {
    auto nm = names(c, nattr);
    std::vector<const ProtoObject*> objs;
    for (int i = 0; i < nobj; ++i) {
        const ProtoObject* o = c->newObject();
        for (int a = 0; a < nattr; ++a) o = o->setAttribute(c, nm[a], c->fromInteger(i + a));
        objs.push_back(o);
    }
    pin(c, objs);
    const ProtoString* key = nm[nattr / 2];
    auto ord = order(nobj, OPS);
    long expected = 0;
    for (long i = 0; i < OPS; ++i) expected += ord[i] + nattr / 2;
    double ns = best_ns([&] {
        long s = 0;
        for (long i = 0; i < OPS; ++i) s += objs[ord[i]]->getAttribute(c, key)->asLong(c);
        return s;
    }, expected);
    report(label, nattr, ns, OPS);
}

// ---------- protoCore: inherited attribute at depth d ----------
static void inherited(ProtoContext* c, int depth, int nobj, int ownAttrs, const char* label) {
    auto nm = names(c, 8);
    const ProtoString* key = ProtoString::createSymbol(c, "method_x");
    const ProtoObject* proto0 = c->newObject()->setAttribute(c, key, c->fromInteger(7));
    for (int a = 0; a < 4; ++a) proto0 = proto0->setAttribute(c, nm[a], c->fromInteger(a));
    const ProtoObject* p = proto0;
    std::vector<const ProtoObject*> keep{proto0};
    for (int d = 1; d < depth; ++d) {
        p = p->newChild(c);
        for (int a = 0; a < 4; ++a) p = p->setAttribute(c, nm[a], c->fromInteger(a));
        keep.push_back(p);
    }
    std::vector<const ProtoObject*> objs;
    for (int i = 0; i < nobj; ++i) {
        const ProtoObject* o = p->newChild(c);
        for (int a = 0; a < ownAttrs; ++a) o = o->setAttribute(c, nm[a], c->fromInteger(a));
        objs.push_back(o);
    }
    pin(c, keep);
    pin(c, objs);
    auto ord = order(nobj, OPS);
    long expected = 7L * OPS;
    double ns = best_ns([&] {
        long s = 0;
        for (long i = 0; i < OPS; ++i) s += objs[ord[i]]->getAttribute(c, key)->asLong(c);
        return s;
    }, expected);
    report(label, depth, ns, OPS);
}

// ---------- protoCore: mutable object ----------
static void mutable_read(ProtoContext* c, int nattr, bool writeBetween, const char* label) {
    auto nm = names(c, nattr);
    const ProtoObject* o = c->newObject(true);
    for (int a = 0; a < nattr; ++a) o->setAttribute(c, nm[a], c->fromInteger(a));
    pin(c, {o});
    const ProtoString* key = nm[nattr / 2];
    const ProtoString* wkey = nm[0];
    const long ops = writeBetween ? OPS / 8 : OPS;
    long expected = (long)(nattr / 2) * ops;
    double best = 1e30;
    for (int r = 0; r < REPS; ++r) {
        auto t0 = Clock::now();
        long s = 0;
        for (long i = 0; i < ops; ++i) {
            if (writeBetween) o->setAttribute(c, wkey, c->fromInteger(i & 1023));
            s += o->getAttribute(c, key)->asLong(c);
        }
        double ns = std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / ops;
        if (s != expected) { std::fprintf(stderr, "CHECKSUM MISMATCH\n"); std::exit(1); }
        best = std::min(best, ns);
        if (writeBetween) c->safepoint();
    }
    report(label, nattr, best, ops);
}

// ---------- native baselines ----------
struct NativeObj { long* fields; int n; };                 // variable-length array, offset known
struct NativeBase { virtual long method() const { return 7; } virtual ~NativeBase() {} };
struct NativeD1 : NativeBase { long f[4]; };

static void native_own(int nattr, int nobj, const char* label) {
    std::vector<NativeObj*> objs;
    for (int i = 0; i < nobj; ++i) {
        auto* o = new NativeObj{new long[nattr], nattr};
        for (int a = 0; a < nattr; ++a) o->fields[a] = i + a;
        objs.push_back(o);
    }
    auto ord = order(nobj, OPS);
    const int idx = nattr / 2;
    long expected = 0;
    for (long i = 0; i < OPS; ++i) expected += ord[i] + idx;
    double ns = best_ns([&] {
        long s = 0;
        for (long i = 0; i < OPS; ++i) { auto* o = objs[ord[i]]; asm volatile("" : "+r"(o)); s += o->fields[idx]; }
        return s;
    }, expected);
    report(label, nattr, ns, OPS);
}

static void native_virtual(int nobj, const char* label) {
    std::vector<NativeBase*> objs;
    for (int i = 0; i < nobj; ++i) objs.push_back(new NativeD1());
    auto ord = order(nobj, OPS);
    double ns = best_ns([&] {
        long s = 0;
        for (long i = 0; i < OPS; ++i) { auto* o = objs[ord[i]]; asm volatile("" : "+r"(o)); s += o->method(); }
        return s;
    }, 7L * OPS);
    report(label, 1, ns, OPS);
}

int main(int argc, char** argv) {
    ProtoSpace space;
    ProtoContext* c = space.rootContext;
    g_roots = space.createRootSet("attr_bench");
    std::string only = argc > 1 ? argv[1] : "";

    std::printf("# scenario                           param  time\n");
    if (only.empty() || only == "own") {
        for (int n : {1, 4, 16, 64}) own_immutable(c, n, 1, "own/immutable/hot(1 obj) nattr");
        for (int n : {1, 4, 16, 64}) own_immutable(c, n, COLD_OBJECTS, "own/immutable/cold(16k obj) nattr");
        for (int n : {1, 4, 16, 64}) native_own(n, 1, "NATIVE own/hot nattr");
        for (int n : {1, 4, 16, 64}) native_own(n, COLD_OBJECTS, "NATIVE own/cold(16k) nattr");
    }
    if (only == "sweep") {
        for (int nobj : {256, 1024, 4096, 16384, 65536}) {
            std::printf("-- nobj=%d\n", nobj);
            for (int n : {4, 16}) own_immutable(c, n, nobj, "own/immutable nattr");
            for (int n : {4, 16}) native_own(n, nobj, "NATIVE own nattr");
            inherited(c, 2, nobj, 4, "inherited depth");
        }
    }
    if (only == "ownhot") for (int i = 0; i < 6; ++i) own_immutable(c, 16, 1, "own/immutable/hot nattr");
    if (only == "owncold") own_immutable(c, 16, COLD_OBJECTS, "own/immutable/cold nattr");
    if (only == "inhhot") for (int i = 0; i < 6; ++i) inherited(c, 4, 1, 4, "inherited/hot depth");
    if (only == "inhcold") inherited(c, 4, COLD_OBJECTS, 4, "inherited/cold depth");
    if (only.empty() || only == "inh") {
        for (int d : {1, 2, 4, 8}) inherited(c, d, 1, 4, "inherited/hot(1 obj) depth");
        for (int d : {1, 2, 4, 8}) inherited(c, d, COLD_OBJECTS, 4, "inherited/cold(16k obj) depth");
        native_virtual(1, "NATIVE virtual call/hot");
        native_virtual(COLD_OBJECTS, "NATIVE virtual call/cold(16k)");
    }
    if (only.empty() || only == "mut") {
        for (int n : {4, 16}) mutable_read(c, n, false, "mutable/read-only nattr");
        for (int n : {4, 16}) mutable_read(c, n, true, "mutable/write+read nattr");
    }
    return 0;
}
