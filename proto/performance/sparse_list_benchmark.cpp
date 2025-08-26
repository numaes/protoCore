//
// Created by gamarino on 26/8/25.
//

#include <iostream>
#include <vector>
#include <chrono>
#include <random>
#include "../headers/proto.h"

const int NUM_OPERATIONS = 100000;

int main() {
    // --- STD::VECTOR BENCHMARKS ---
    std::cout << "--- Benchmarking std::vector ---" << std::endl;

    // 1. Append Benchmark
    auto start = std::chrono::high_resolution_clock::now();
    std::vector<int> std_vector;
    for (int i = 0; i < NUM_OPERATIONS; ++i) {
        std_vector.push_back(i);
    }
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> std_append_time = end - start;
    std::cout << "std::vector append time: " << std_append_time.count() << "s" << std::endl;

    // 2. Random Access Benchmark
    std::vector<int> random_indices;
    std::mt19937 rng(std::chrono::steady_clock::now().time_since_epoch().count());
    std::uniform_int_distribution<int> dist(0, NUM_OPERATIONS - 1);
    for (int i = 0; i < NUM_OPERATIONS; ++i) {
        random_indices.push_back(dist(rng));
    }

    start = std::chrono::high_resolution_clock::now();
    long long std_sum = 0;
    for (int index : random_indices) {
        std_sum += std_vector[index];
    }
    end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> std_access_time = end - start;
    std::cout << "std::vector random access time: " << std_access_time.count() << "s (checksum: " << std_sum << ")" << std::endl;

    // 3. Iteration Benchmark
    start = std::chrono::high_resolution_clock::now();
    std_sum = 0;
    for (int val : std_vector) {
        std_sum += val;
    }
    end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> std_iteration_time = end - start;
    std::cout << "std::vector iteration time: " << std_iteration_time.count() << "s (checksum: " << std_sum << ")" << std::endl;


    // --- PROTOLIST BENCHMARKS ---
    std::cout << "\n--- Benchmarking ProtoList ---" << std::endl;

    proto::ProtoContext c;

    // 1. Append Benchmark
    start = std::chrono::high_resolution_clock::now();
    proto::ProtoList* proto_list = c.newList();
    for (int i = 0; i < NUM_OPERATIONS; ++i) {
        proto_list = proto_list->appendLast(&c, c.fromInteger(i));
    }
    end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> proto_append_time = end - start;
    std::cout << "ProtoList append time: " << proto_append_time.count() << "s" << std::endl;

    // 2. Random Access Benchmark
    start = std::chrono::high_resolution_clock::now();
    long long proto_sum = 0;
    for (int index : random_indices) {
        proto_sum += proto_list->getAt(&c, index)->asInteger(&c);
    }
    end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> proto_access_time = end - start;
    std::cout << "ProtoList random access time: " << proto_access_time.count() << "s (checksum: " << proto_sum << ")" << std::endl;

    // 3. Iteration Benchmark
    start = std::chrono::high_resolution_clock::now();
    proto_sum = 0;
    proto::ProtoListIterator* iter = proto_list->getIterator(&c);
    while (iter->hasNext(&c)) {
        proto_sum += iter->next(&c)->asInteger(&c);
        iter = iter->advance(&c);
    }
    end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> proto_iteration_time = end - start;
    std::cout << "ProtoList iteration time: " << proto_iteration_time.count() << "s (checksum: " << proto_sum << ")" << std::endl;

    return 0;
}
