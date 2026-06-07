// Mutable-heavy benchmark: 1000 mutable objects * 10000 attribute reads
// each, so the MutableValueCacheEntry cache is exercised on every read.
#include <iostream>
#include <chrono>
#include <vector>
#include "../headers/protoCore.h"

int main() {
    proto::ProtoSpace space;
    proto::ProtoContext* c = space.rootContext;

    const int num_objects = 1000;
    const int num_accesses = 100000;

    const proto::ProtoString* attr = c->fromUTF8String("attr")->asString(c);

    // Build mutable objects, each with one attribute.
    std::vector<const proto::ProtoObject*> objs(num_objects);
    for (int i = 0; i < num_objects; ++i) {
        // newObject(true) → mutable.
        objs[i] = c->newObject(true);
        const_cast<proto::ProtoObject*>(objs[i])->setAttribute(c, attr, c->fromInteger(i));
    }

    // Hot loop.
    auto start = std::chrono::high_resolution_clock::now();
    long long checksum = 0;
    for (int i = 0; i < num_objects; ++i) {
        for (int j = 0; j < num_accesses; ++j) {
            checksum += objs[i]->getAttribute(c, attr)->asLong(c);
        }
    }
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;

    long long expected = 0;
    for (int i = 0; i < num_objects; ++i) expected += (long long)i * num_accesses;
    std::cout << "checksum: " << (checksum == expected ? "OK" : "FAIL") << "\n";
    std::cout << "time: " << diff.count() << " s\n";
    return 0;
}
