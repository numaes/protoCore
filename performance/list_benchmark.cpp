/*
 * list_benchmark.cpp
 *
 *  Created on: 2024-01-15
 *      Author: gamarino
 */

#include <iostream>
#include <chrono>
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
    const int num_iterations = 100000;
    std::cout << "--- List Append Benchmark ---" << std::endl;
    std::cout << "Iterations: " << num_iterations << std::endl;

    // --- Proto List ---
    auto start_proto = std::chrono::high_resolution_clock::now();
    const ProtoList* proto_list = c->newList();
    for (int i = 0; i < num_iterations; ++i) {
        proto_list = proto_list->appendLast(c, c->fromInteger(i));
    }
    auto end_proto = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff_proto = end_proto - start_proto;
    std::cout << "Proto list time: " << diff_proto.count() << " s" << std::endl;

    // --- Verification ---
    long long checksum = 0;
    const ProtoListIterator* iter = proto_list->getIterator(c);
    while (iter->hasNext(c)) {
        checksum += iter->next(c)->asInteger(c);
        iter = iter->advance(c);
    }
    long long expected_checksum = (long long)(num_iterations - 1) * num_iterations / 2;
    if (checksum != expected_checksum) {
        std::cerr << "Checksum mismatch! Got " << checksum << ", expected " << expected_checksum << std::endl;
    } else {
        std::cout << "Checksum verified." << std::endl;
    }

    std::cout << "--------------------------" << std::endl;
    return PROTO_NONE;
}

int main(int argc, char* argv[]) {
    // Corrected: ProtoSpace constructor takes no arguments
    proto::ProtoSpace space;
    benchmarks(space.rootContext, nullptr, nullptr, nullptr, nullptr);
    return 0;
}
