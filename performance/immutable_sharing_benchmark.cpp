/*
 * immutable_sharing_benchmark.cpp
 *
 *  Created on: 2024-01-15
 *      Author: gamarino
 */

#include <iostream>
#include <vector>
#include <chrono>
#include "../headers/protoCore.h"

using namespace proto;

#define INITIAL_SIZE 10000
#define NUM_VERSIONS 1000

// Corrected: Signature now uses const pointer
long long checksum_proto_list(proto::ProtoContext* c, const proto::ProtoList* list) {
    long long checksum = 0;
    // Corrected: Iterator is a const pointer
    const proto::ProtoListIterator* iter = list->getIterator(c);
    while (iter->hasNext(c)) {
        checksum += iter->next(c)->asLong(c);
        iter = iter->advance(c);
    }
    return checksum;
}

const ProtoObject* benchmarks(
    ProtoContext* c,
    const ProtoObject* self,
    const ParentLink* parentLink,
    const ProtoList* positionalParameters,
    const ProtoSparseList* keywordParametersDict
) {
    std::cout << "--- Immutable Sharing Benchmark ---" << std::endl;
    std::cout << "Initial size: " << INITIAL_SIZE << ", Versions: " << NUM_VERSIONS << std::endl;

    // --- Create Base List ---
    auto start_create = std::chrono::high_resolution_clock::now();
    // Corrected: base_list is a const pointer
    const proto::ProtoList* base_list = c->newList();
    for (int i = 0; i < INITIAL_SIZE; ++i) {
        // Corrected: re-assign to the const pointer
        base_list = base_list->appendLast(c, c->fromInteger(i));
    }
    auto end_create = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff_create = end_create - start_create;
    std::cout << "Creation time: " << diff_create.count() << " s" << std::endl;

    // --- Create New Versions ---
    auto start_versions = std::chrono::high_resolution_clock::now();
    // Corrected: Vector holds const pointers
    std::vector<const proto::ProtoList*> proto_versions(NUM_VERSIONS);
    for (int i = 0; i < NUM_VERSIONS; ++i) {
        // Corrected: Assignment is to a vector of const pointers
        proto_versions[i] = base_list->appendLast(c, c->fromInteger(INITIAL_SIZE + i));
    }
    auto end_versions = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff_versions = end_versions - start_versions;
    std::cout << "Versioning time: " << diff_versions.count() << " s" << std::endl;

    // --- Verify Checksums ---
    auto start_checksum = std::chrono::high_resolution_clock::now();
    long long base_checksum = checksum_proto_list(c, base_list);
    for (int i = 0; i < NUM_VERSIONS; ++i) {
        long long version_checksum = checksum_proto_list(c, proto_versions[i]);
        if (version_checksum != base_checksum + INITIAL_SIZE + i) {
            std::cerr << "Checksum mismatch!" << std::endl;
        }
    }
    auto end_checksum = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff_checksum = end_checksum - start_checksum;
    std::cout << "Checksum time: " << diff_checksum.count() << " s" << std::endl;
    std::cout << "------------------------------------" << std::endl;

    return PROTO_NONE;
}

int main(int argc, char* argv[]) {
    // Corrected: ProtoSpace constructor takes no arguments
    proto::ProtoSpace space;
    benchmarks(space.rootContext, nullptr, nullptr, nullptr, nullptr);
    return 0;
}
