// protoCore floor for the protoJS standard benchmarks: the same work done
// directly through the protoCore API, median of 5 runs, ns per operation.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>
#include "protoCore.h"
using namespace proto;
using clk = std::chrono::steady_clock;

template <class F> static double median_ms(F f) {
    std::vector<double> t;
    for (int k = 0; k < 5; ++k) {
        auto a = clk::now(); f(); auto b = clk::now();
        t.push_back(std::chrono::duration<double, std::milli>(b - a).count());
    }
    std::sort(t.begin(), t.end());
    return t[2];
}
static void report(const char* name, double ms, long ops, long long check) {
    std::printf("%-34s %8.2f ms  %8.1f ns/op  check=%lld\n", name, ms, ms * 1e6 / ops, check);
}

int main() {
    ProtoSpace space;
    ProtoContext* c = space.rootContext;
    std::vector<const ProtoString*> keys;
    for (int i = 0; i < 100; ++i)
        keys.push_back(ProtoString::fromUTF8String(c, ("k" + std::to_string(i)).c_str())->intern(c));

    // object_write_only: 1,000,000 writes over 100 keys into one mutable object.
    long long chk = 0;
    double ms = median_ms([&] {
        const ProtoObject* o = c->newObject(true);
        for (long i = 0; i < 1000000; ++i) o->setAttribute(c, keys[i % 100], c->fromInteger(i));
        chk = o->getAttribute(c, keys[99])->asLong(c);
    });
    report("object_write_only (mutable)", ms, 1000000, chk);

    // Same writes on an immutable object (each write returns a new version).
    ms = median_ms([&] {
        const ProtoObject* o = c->newObject(false);
        for (long i = 0; i < 1000000; ++i) o = o->setAttribute(c, keys[i % 100], c->fromInteger(i));
        chk = o->getAttribute(c, keys[99])->asLong(c);
    });
    report("object_write_only (immutable)", ms, 1000000, chk);

    // object_read_only: 1,000,000 reads over 100 keys.
    const ProtoObject* ro = c->newObject(true);
    for (int i = 0; i < 100; ++i) ro->setAttribute(c, keys[i], c->fromInteger(i));
    ms = median_ms([&] {
        long long s = 0;
        for (long i = 0; i < 1000000; ++i) s += ro->getAttribute(c, keys[i % 100])->asLong(c);
        chk = s;
    });
    report("object_read_only (1M reads)", ms, 1000000, chk);

    // array_literal: 100,000 pushes; the list lives in a mutable object as an
    // array would, plus the bare immutable list for reference.
    const ProtoString* items = ProtoString::fromUTF8String(c, "items")->intern(c);
    ms = median_ms([&] {
        const ProtoObject* arr = c->newObject(true);
        const ProtoList* l = c->newList();
        arr->setAttribute(c, items, l->asObject(c));
        for (long i = 0; i < 100000; ++i) {
            const ProtoList* cur = arr->getAttribute(c, items)->asList(c);
            arr->setAttribute(c, items, cur->appendLast(c, c->fromInteger(i))->asObject(c));
        }
        chk = arr->getAttribute(c, items)->asList(c)->getSize(c);
    });
    report("array push (list in mutable obj)", ms, 100000, chk);
    ms = median_ms([&] {
        const ProtoList* l = c->newList();
        for (long i = 0; i < 100000; ++i) l = l->appendLast(c, c->fromInteger(i));
        chk = l->getSize(c);
    });
    report("array push (bare list)", ms, 100000, chk);

    // string_concat: s += 'x', 50,000 times.
    const ProtoString* x = ProtoString::fromUTF8String(c, "x");
    ms = median_ms([&] {
        const ProtoString* s = ProtoString::fromUTF8String(c, "");
        for (long i = 0; i < 50000; ++i) s = s->appendLast(c, x);
        chk = s->getSize(c);
    });
    report("string_concat (50k appends)", ms, 50000, chk);

    // function_calls: 200,000 calls, each a ProtoContext with one argument slot.
    ms = median_ms([&] {
        long long state = 1;
        for (long i = 0; i < 200000; ++i) {
            ProtoContext callee(&space, c, nullptr, nullptr, nullptr, nullptr, 1);
            const ProtoObject* v = callee.fromInteger(state);
            state = (v->asLong(&callee) + 1) | 0;
        }
        chk = state;
    });
    report("function_calls (context per call)", ms, 200000, chk);
    return 0;
}
