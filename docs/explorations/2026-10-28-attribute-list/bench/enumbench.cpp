// Cost of one getOwnAttributes() call (public API) on an object with n own
// attributes: instructions and cycles of the calling thread, cells allocated.
// Self-check: the returned sparse list has n entries.
#include <cstdio>
#include <string>
#include <vector>
#include "protoCore.h"
#include "perfcount.h"
using namespace proto;
int main() {
    ThreadCounters pc;
    ProtoSpace space;
    ProtoContext* root = space.rootContext;
    std::vector<const ProtoString*> keys;
    for (int i = 0; i < 128; ++i)
        keys.push_back(ProtoString::fromUTF8String(root, ("key" + std::to_string(i)).c_str())->intern(root));
    int fail = 0;
    for (int n : {1, 2, 4, 8, 16, 32}) {
        ProtoContext c(&space, root, nullptr, nullptr, nullptr, nullptr);
        const ProtoObject* o = c.newObject(false);
        for (int i = 0; i < n; ++i) o = o->setAttribute(&c, keys[i], c.fromInteger(i));
        const long OPS = 200000;
        double best_in = 1e18, best_cy = 1e18, cells = 0;
        for (int rep = 0; rep < 5; ++rep) {
            ProtoContext r(&space, &c, nullptr, nullptr, nullptr, nullptr);
            long long sum = 0; uint64_t i0, c0;
            proto_ulong a0 = r.allocatedCellsCount;
            pc.start();
            for (long i = 0; i < OPS; ++i) sum += (long long) o->getOwnAttributes(&r)->getSize(&r);
            pc.stop(i0, c0);
            proto_ulong a1 = r.allocatedCellsCount;
            if (sum != (long long) n * OPS) { std::printf("CHECK FAILED n=%d\n", n); ++fail; }
            if (a1 >= a0 && rep == 0) cells = double(a1 - a0) / OPS;
            best_in = std::min(best_in, double(i0) / OPS); best_cy = std::min(best_cy, double(c0) / OPS);
        }
        std::printf("%4d getOwnAttributes %8.0f in %8.0f cy %6.2f cells\n", n, best_in, best_cy, cells);
    }
    std::printf("RESULT %s\n", fail ? "FAILED" : "OK");
    return fail;
}
