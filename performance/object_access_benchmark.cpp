//
// Created by gamarino on 26/8/25.
//
#include <iostream>
#include <vector>
#include <chrono>
#include <random>

#include "../headers/proto.h"

const int NUM_OPERATIONS = 1000000;

proto::ProtoObject *benchmarks(proto::ProtoContext *c, proto::ProtoObject* self, proto::ParentLink* parentLink, proto::ProtoList* args, proto::ProtoSparseList* kwargs) {

    std::cout << "--- Benchmarking ProtoObject Attribute Access ---" << std::endl;

    // --- Test Setup ---

    // 1. Create a parent object with an attribute
    proto::ProtoString* parent_attr_name = c->fromUTF8String("parent_attr");
    proto::ProtoObject* parent_obj = c->newObject()->setAttribute(c, parent_attr_name, c->fromInteger(123));

    // 2. Create a child object that inherits from the parent
    proto::ProtoObject* child_obj = parent_obj->newChild(c);

    // 3. Add an attribute to the child object itself
    proto::ProtoString* child_attr_name = c->fromUTF8String("child_attr");
    child_obj = child_obj->setAttribute(c, child_attr_name, c->fromInteger(456));

    // 4. Prepare for random access
    std::vector<proto::ProtoString*> keys_to_access;
    keys_to_access.push_back(child_attr_name);
    keys_to_access.push_back(parent_attr_name);

    std::vector<int> random_indices;
    std::mt19937 rng(std::chrono::steady_clock::now().time_since_epoch().count());
    std::uniform_int_distribution<int> dist(0, 1); // 0 for child, 1 for parent

    for (int i = 0; i < NUM_OPERATIONS; ++i) {
        random_indices.push_back(dist(rng));
    }

    // --- Pre-run / Cache Warming ---
    // Perform one access of each attribute to ensure the cache is populated
    // for the subsequent benchmark, which measures pure cache-hit performance.
    volatile int warm_up_child = child_obj->getAttribute(c, child_attr_name)->asInteger(c);
    volatile int warm_up_parent = child_obj->getAttribute(c, parent_attr_name)->asInteger(c);


    // --- Benchmark Execution ---

    auto start = std::chrono::high_resolution_clock::now();

    long long checksum = 0; // Use checksum to prevent compiler from optimizing away the loop
    for (int index : random_indices) {
        proto::ProtoString* key = keys_to_access[index];
        checksum += child_obj->getAttribute(c, key)->asInteger(c);
    }

    auto end = std::chrono::high_resolution_clock::now();

    // --- Results ---

    std::chrono::duration<double> total_time = end - start;
    double average_time_ns = (total_time.count() * 1e9) / NUM_OPERATIONS;

    std::cout << "Performed " << NUM_OPERATIONS << " random attribute lookups." << std::endl;
    std::cout << "Total time: " << total_time.count() << "s" << std::endl;
    std::cout << "Average access time: " << average_time_ns << " ns" << std::endl;
    std::cout << "(Checksum: " << checksum << " to ensure correctness)" << std::endl;


    return 0;
}

int main(int argc, char **argv) {
    auto space = proto::ProtoSpace (benchmarks, argc, argv);
    while (true)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
