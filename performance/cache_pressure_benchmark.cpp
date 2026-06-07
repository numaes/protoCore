// Cache-pressure benchmark: many (object, attr) pairs to drive realistic
// hit-rate measurement of the per-thread attribute cache.
#include <iostream>
#include <chrono>
#include <vector>
#include "../headers/protoCore.h"

int main() {
    proto::ProtoSpace space;
    proto::ProtoContext* c = space.rootContext;

    // Working set significantly larger than the cache (1024 slots) and the
    // attribute count plausible for realistic programs (think class hierarchies
    // with several inherited slots).
    const int num_objects = 250;
    const int num_attrs   = 4;
    const int num_passes  = 200;

    std::vector<const proto::ProtoString*> attrs(num_attrs);
    for (int a = 0; a < num_attrs; ++a) {
        std::string n = "attr_" + std::to_string(a);
        attrs[a] = c->fromUTF8String(n.c_str())->asString(c);
    }

    std::vector<const proto::ProtoObject*> objs(num_objects);
    for (int i = 0; i < num_objects; ++i) {
        objs[i] = c->newObject();
        for (int a = 0; a < num_attrs; ++a) {
            objs[i] = objs[i]->setAttribute(c, attrs[a], c->fromInteger(i * num_attrs + a));
        }
    }

    long long checksum = 0;
    auto start = std::chrono::high_resolution_clock::now();
    for (int pass = 0; pass < num_passes; ++pass) {
        for (int i = 0; i < num_objects; ++i) {
            for (int a = 0; a < num_attrs; ++a) {
                checksum += objs[i]->getAttribute(c, attrs[a])->asLong(c);
            }
        }
    }
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;

    long long expected = 0;
    for (int i = 0; i < num_objects; ++i)
        for (int a = 0; a < num_attrs; ++a)
            expected += (long long)(i * num_attrs + a);
    expected *= num_passes;
    std::cout << "checksum: " << (checksum == expected ? "OK" : "FAIL") << "\n";
    std::cout << "lookups : " << (long long)num_objects * num_attrs * num_passes << "\n";
    std::cout << "time    : " << diff.count() << " s\n";
    return 0;
}
