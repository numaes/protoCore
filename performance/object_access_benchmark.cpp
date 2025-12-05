/*
 * object_access_benchmark.cpp
 *
 *  Created on: 2024-01-15
 *      Author: gamarino
 */

#include <iostream>
#include <chrono>
#include <vector>
#include "../headers/protoCore.h"

using namespace proto;

// Corrected: Function signature syntax
const ProtoObject* benchmarks(
    ProtoContext* c,
    const ProtoObject* self,
    const ParentLink* parentLink,
    const ProtoList* args,
    const ProtoSparseList* kwargs
) {
    const int num_objects = 1000;
    const int num_accesses = 10000;
    std::cout << "--- Object Access Benchmark ---" << std::endl;
    std::cout << "Objects: " << num_objects << ", Accesses per object: " << num_accesses << std::endl;

    // --- Create Objects ---
    std::vector<const ProtoObject*> objects(num_objects);
    const ProtoString* attr_name = c->fromUTF8String("my_attribute")->asString(c);

    for (int i = 0; i < num_objects; ++i) {
        objects[i] = c->newObject()->setAttribute(c, const_cast<ProtoString*>(attr_name), c->fromInteger(i));
    }

    // --- Access Attributes ---
    auto start_access = std::chrono::high_resolution_clock::now();
    long long checksum = 0;
    for (int i = 0; i < num_objects; ++i) {
        for (int j = 0; j < num_accesses; ++j) {
            checksum += objects[i]->getAttribute(c, const_cast<ProtoString*>(attr_name))->asInteger(c);
        }
    }
    auto end_access = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff_access = end_access - start_access;

    // --- Verification ---
    long long expected_checksum = 0;
    for (int i = 0; i < num_objects; ++i) {
        expected_checksum += (long long)i * num_accesses;
    }
    if (checksum != expected_checksum) {
        std::cerr << "Checksum mismatch! Got " << checksum << ", expected " << expected_checksum << std::endl;
    } else {
        std::cout << "Checksum verified." << std::endl;
    }

    std::cout << "Total access time: " << diff_access.count() << " s" << std::endl;
    std::cout << "--------------------------" << std::endl;

    return PROTO_NONE;
}

int main(int argc, char* argv[]) {
    // Corrected: ProtoSpace constructor takes no arguments
    proto::ProtoSpace space;
    benchmarks(space.rootContext, nullptr, nullptr, nullptr, nullptr);
    return 0;
}
