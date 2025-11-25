/*
 * sparse_list_benchmark.cpp
 *
 *  Created on: 2024-01-15
 *      Author: gamarino
 */

#include <iostream>
#include <chrono>
#include <vector>
#include <random>
#include "../headers/proto.h"

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
    const int list_size = 1000;
    std::cout << "--- Sparse List Benchmark ---" << std::endl;
    std::cout << "Iterations: " << num_iterations << ", List size: " << list_size << std::endl;

    // --- Setup ---
    std::vector<unsigned long> keys(num_iterations);
    std::vector<const ProtoObject*> values(num_iterations);
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<unsigned long> distrib(0, list_size * 10);

    for (int i = 0; i < num_iterations; ++i) {
        keys[i] = distrib(gen);
        values[i] = c->fromInteger(i);
    }

    // --- Proto Sparse List ---
    auto start_proto = std::chrono::high_resolution_clock::now();
    const ProtoSparseList* proto_list = c->newSparseList();
    for (int i = 0; i < num_iterations; ++i) {
        proto_list = proto_list->setAt(c, keys[i], values[i]);
    }
    auto end_proto = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff_proto = end_proto - start_proto;
    std::cout << "Proto sparse list insertion time: " << diff_proto.count() << " s" << std::endl;

    // --- Access ---
    auto start_access = std::chrono::high_resolution_clock::now();
    long long checksum = 0;
    for (int i = 0; i < num_iterations; ++i) {
        const ProtoObject* val = proto_list->getAt(c, keys[i]);
        if (val != PROTO_NONE) {
            checksum += val->asInteger(c);
        }
    }
    auto end_access = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff_access = end_access - start_access;
    std::cout << "Proto sparse list access time: " << diff_access.count() << " s" << std::endl;

    std::cout << "--------------------------" << std::endl;
    return PROTO_NONE;
}

int main(int argc, char* argv[]) {
    // Corrected: ProtoSpace constructor takes no arguments
    proto::ProtoSpace space;
    benchmarks(space.rootContext, nullptr, nullptr, nullptr, nullptr);
    return 0;
}
