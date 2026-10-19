// Hot copy-update in the shape of a runtime: every batch of BATCH copies runs
// in its own child context (like one method call), so the versions it drops
// become collectable when it returns.  The live version is carried across
// batches in a root set.  Reports ns/op; the caller sets heap limits by env.
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
    const long batch = argc > 3 ? std::atol(argv[3]) : 1000;
    ProtoSpace space;
    ProtoContext* c = space.rootContext;
    ProtoRootSet* roots = space.createRootSet("live");
    std::vector<const ProtoString*> nm;
    for (int a = 0; a < nattr; ++a) nm.push_back(ProtoString::createSymbol(c, ("attr_" + std::to_string(a)).c_str()));
    const ProtoObject* o = c->newObject();
    for (int a = 0; a < nattr; ++a) o = o->setAttribute(c, nm[a], c->fromInteger(a));
    ProtoRootSet::Handle h = roots->add(o);

    for (int rep = 0; rep < 5; ++rep) {
        const uint64_t cycles0 = space.getGCCycleCount();
        auto t0 = std::chrono::steady_clock::now();
        for (long done = 0; done < ops; done += batch) {
            {
                ProtoContext call(&space, c, nullptr, nullptr, nullptr, nullptr);
                for (long i = done; i < done + batch; ++i)
                    o = o->setAttribute(&call, nm[0], call.fromInteger(i & 1023));
                ProtoRootSet::Handle nh = roots->add(o);
                roots->remove(h);
                h = nh;
            }
            c->safepoint();
        }
        const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / ops;
        if (o->getAttribute(c, nm[0])->asLong(c) != ((ops - 1) & 1023)) { std::puts("WRONG"); return 1; }
        std::printf("rep %d: %.1f ns/op  gc cycles %llu  heap cells %d\n", rep, ns,
                    (unsigned long long) (space.getGCCycleCount() - cycles0), space.heapSize);
    }
    return 0;
}
