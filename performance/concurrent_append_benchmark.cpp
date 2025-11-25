/*
 * concurrent_append_benchmark.cpp
 *
 *  Created on: 2024-01-15
 *      Author: gamarino
 */

#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include "../headers/proto.h"

using namespace proto;

#define NUM_THREADS 4
#define NUM_APPENDS 10000

// Corrected: std::atomic must work with pointers for complex types.
std::atomic<const ProtoList*> shared_proto_list;

void proto_list_append_worker(ProtoContext* c) {
    // Corrected: Simplified context creation
    proto::ProtoContext thread_context(c->space);

    for (int i = 0; i < NUM_APPENDS; ++i) {
        const ProtoList* current_list;
        const ProtoList* new_list;
        do {
            current_list = shared_proto_list.load();
            // Corrected: appendLast now returns a const pointer
            new_list = current_list->appendLast(&thread_context, thread_context.fromInteger(i));
        } while (!shared_proto_list.compare_exchange_weak(current_list, new_list));
    }
}

const ProtoObject* benchmarks(
    ProtoContext* c,
    const ProtoObject* self,
    const ParentLink* parentLink,
    const ProtoList* positionalParameters,
    const ProtoSparseList* keywordParametersDict
) {
    std::cout << "--- Concurrent Append Benchmark ---" << std::endl;
    std::cout << "Threads: " << NUM_THREADS << ", Appends per thread: " << NUM_APPENDS << std::endl;

    // Initialize the shared list
    shared_proto_list.store(c->newList());

    std::vector<std::thread> threads;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back(proto_list_append_worker, c);
    }

    for (auto& t : threads) {
        t.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;

    std::cout << "Total time: " << diff.count() << " s" << std::endl;
    std::cout << "Final size: " << shared_proto_list.load()->getSize(c) << std::endl;
    std::cout << "------------------------------------" << std::endl;

    return PROTO_NONE;
}

int main(int argc, char* argv[]) {
    // Corrected: ProtoSpace constructor takes no arguments
    proto::ProtoSpace space;
    // You might need to register the 'benchmarks' method with the space if you want to call it dynamically.
    // For this simple case, we can call it directly.
    benchmarks(space.rootContext, nullptr, nullptr, nullptr, nullptr);
    return 0;
}
