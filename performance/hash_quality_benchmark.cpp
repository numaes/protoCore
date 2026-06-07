// Hash-quality benchmark: working set 500 (obj, attr) pairs vs 1024-slot cache.
// With a well-distributed hash all entries land in distinct slots → 100 % hits.
// With a poorly-distributed hash, collisions cause evictions during the first
// pass and lasting misses on later passes.
#include <iostream>
#include <chrono>
#include <vector>
#include "../headers/protoCore.h"
int main() {
    proto::ProtoSpace space;
    proto::ProtoContext* c = space.rootContext;

    const int num_objects = 500;
    const int num_passes  = 20000;

    const proto::ProtoString* attr = c->fromUTF8String("a")->asString(c);
    std::vector<const proto::ProtoObject*> objs(num_objects);
    for (int i = 0; i < num_objects; ++i) {
        objs[i] = c->newObject()->setAttribute(c, attr, c->fromInteger(i));
    }

    long long checksum = 0;
    auto start = std::chrono::high_resolution_clock::now();
    for (int p = 0; p < num_passes; ++p)
        for (int i = 0; i < num_objects; ++i)
            checksum += objs[i]->getAttribute(c, attr)->asLong(c);
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;

    long long expected = 0;
    for (int i = 0; i < num_objects; ++i) expected += (long long)i;
    expected *= num_passes;
    std::cout << "checksum: " << (checksum == expected ? "OK" : "FAIL") << "\n";
    std::cout << "lookups : " << (long long)num_objects * num_passes << "\n";
    std::cout << "time    : " << diff.count() << " s\n";
    return 0;
}
