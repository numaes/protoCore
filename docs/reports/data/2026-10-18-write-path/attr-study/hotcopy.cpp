// Hot copy-update profile: one immutable object, repeated setAttribute of an
// existing attribute.  Reports cells allocated per op and ns/op.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "protoCore.h"
using namespace proto;

int main(int argc, char** argv) {
    const int nattr = argc > 1 ? std::atoi(argv[1]) : 4;
    const long ops = argc > 2 ? std::atol(argv[2]) : 2000000;
    ProtoSpace space;
    ProtoContext* c = space.rootContext;
    std::vector<const ProtoString*> nm;
    for (int a = 0; a < nattr; ++a) nm.push_back(ProtoString::createSymbol(c, ("attr_" + std::to_string(a)).c_str()));
    const ProtoObject* base = c->newObject();
    for (int a = 0; a < nattr; ++a) base = base->setAttribute(c, nm[a], c->fromInteger(a));
    space.createRootSet("hot")->add(base);

    {   // cells per op
        ProtoContext sub(&space, c, nullptr, nullptr, nullptr, nullptr);
        const proto_ulong before = sub.allocatedCellsCount;
        const ProtoObject* o = base;
        for (int i = 0; i < 1000; ++i) o = o->setAttribute(&sub, nm[0], sub.fromInteger(i + 1));
        std::printf("nattr=%d cells/op=%.2f\n", nattr, (sub.allocatedCellsCount - before) / 1000.0);
    }
    for (int rep = 0; rep < 5; ++rep) {
        long s = 0;
        double ns;
        {
            ProtoContext sub(&space, c, nullptr, nullptr, nullptr, nullptr);
            const ProtoObject* o = base;
            auto t0 = std::chrono::steady_clock::now();
            for (long i = 0; i < ops; ++i) {
                o = o->setAttribute(&sub, nm[0], sub.fromInteger(i & 1023));
                s += reinterpret_cast<uintptr_t>(o) & 1;
            }
            ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / ops;
            if (o->getAttribute(&sub, nm[0])->asLong(&sub) != ((ops - 1) & 1023)) { std::puts("WRONG"); return 1; }
        }
        c->safepoint();
        if (argc > 3) {   // run two full cycles so the first one has completed
            for (int k = 0; k < 2; ++k) {
                const uint64_t n = space.getGCCycleCount();
                space.triggerGC();
                while (space.getGCCycleCount() <= n) c->safepoint();
            }
            c->safepoint();
        }
        std::printf("rep %d: %.1f ns/op\n", rep, ns);
    }
    return 0;
}
