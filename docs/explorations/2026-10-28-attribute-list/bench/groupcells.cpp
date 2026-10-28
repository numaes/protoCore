// Cells per construction of a 5-field object: five setAttribute calls versus
// one setAttributes(5), immutable and mutable.  Self-check: field values.
#include <cstdio>
#include <string>
#include "protoCore.h"
using namespace proto;
int main() {
    ProtoSpace space; ProtoContext* root = space.rootContext;
    const ProtoString* k[5];
    for (int i = 0; i < 5; ++i) k[i] = ProtoString::fromUTF8String(root, ("f" + std::to_string(i)).c_str())->intern(root);
    int fail = 0;
    for (int mut = 0; mut < 2; ++mut) for (int grouped = 0; grouped < 2; ++grouped) {
        ProtoContext c(&space, root, nullptr, nullptr, nullptr, nullptr);
        const long N = 2000; double sum = 0; long cnt = 0;
        for (long j = 0; j < N; ++j) {
            proto_ulong a0 = c.allocatedCellsCount;
            const ProtoObject* o = c.newObject(mut);
            const ProtoObject* v[5]; for (int i = 0; i < 5; ++i) v[i] = c.fromInteger(j + i);
            if (grouped) o = o->setAttributes(&c, 5, k, v);
            else for (int i = 0; i < 5; ++i) o = o->setAttribute(&c, k[i], v[i]);
            proto_ulong a1 = c.allocatedCellsCount;
            if (a1 >= a0) { sum += a1 - a0; ++cnt; }
            if (o->getAttribute(&c, k[4])->asLong(&c) != j + 4) ++fail;
        }
        std::printf("%s %s: %.2f cells per construction (incl. newObject)\n", mut ? "mutable  " : "immutable", grouped ? "setAttributes(5)" : "5 x setAttribute", sum / cnt);
    }
    std::printf("RESULT %s\n", fail ? "FAILED" : "OK");
    return fail;
}
