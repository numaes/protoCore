// Cost of one mutable attribute write as a function of how many mutable
// objects are alive in the process.  Each write path-copies the object's
// attribute tree, allocates a new ProtoObjectCell, and path-copies the
// global mutable table's shard (MUTABLE_ROOT_SHARDS = 256), whose depth is
// about log2(live mutables / 256).  Reports ns and cells per write; the
// written object's final value is checked.
//
// usage: mutable_population [population ...]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "protoCore.h"
using namespace proto;

int main(int argc, char** argv) {
    std::vector<long> pops;
    for (int i = 1; i < argc; ++i) pops.push_back(std::atol(argv[i]));
    if (pops.empty()) pops = {1, 1000, 10000, 100000, 1000000};
    const long ops = 200000;

    ProtoSpace space;
    ProtoContext* c = space.rootContext;
    ProtoRootSet* roots = space.createRootSet("population");
    const ProtoString* kx = ProtoString::createSymbol(c, "x");
    const ProtoString* ky = ProtoString::createSymbol(c, "y");

    long alive = 0;
    const ProtoObject* target = nullptr;
    std::printf("# live_mutables  ns/write  cells/write\n");
    for (long pop : pops) {
        // Grow the population of live mutable objects (pinned in one list).
        if (pop > alive) {
            ProtoContext build(&space, c, nullptr, nullptr, nullptr, nullptr);
            const ProtoList* l = build.newList();
            for (long i = alive; i < pop; ++i) {
                const ProtoObject* o = build.newObject(true);
                o->setAttribute(&build, kx, build.fromInteger(i));
                o->setAttribute(&build, ky, build.fromInteger(1));
                l = l->appendLast(&build, o);
                if (!target) target = o;
            }
            roots->add(reinterpret_cast<const ProtoObject*>(l));
            alive = pop;
        }
        c->safepoint();
        double best = 1e30, cells = 0;
        for (int rep = 0; rep < 3; ++rep) {
            ProtoContext sub(&space, c, nullptr, nullptr, nullptr, nullptr);
            const proto_ulong before = sub.allocatedCellsCount;
            auto t0 = std::chrono::steady_clock::now();
            for (long i = 0; i < ops; ++i) target->setAttribute(&sub, kx, sub.fromInteger(i & 1023));
            const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / ops;
            cells = double(sub.allocatedCellsCount - before) / ops;
            if (target->getAttribute(&sub, kx)->asLong(&sub) != ((ops - 1) & 1023)) { std::puts("WRONG"); return 1; }
            if (ns < best) best = ns;
        }
        std::printf("%14ld  %8.1f  %11.2f\n", alive, best, cells);
    }
    return 0;
}
