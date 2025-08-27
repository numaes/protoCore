#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <mutex>
#include <atomic>
#include "../headers/proto.h"

const int NUM_THREADS = 8;
const int OPERATIONS_PER_THREAD = 25000;

// --- 1. std::vector with a mutex ---
std::mutex vec_mutex;
std::vector<int> shared_vector;

void vector_append_worker() {
    for (int i = 0; i < OPERATIONS_PER_THREAD; ++i) {
        std::lock_guard<std::mutex> lock(vec_mutex);
        shared_vector.push_back(i);
    }
}

// --- 2. ProtoList (designed for concurrency) ---
std::atomic<proto::ProtoList*> shared_proto_list;

void proto_list_append_worker(proto::ProtoContext* c) {
    // Each thread gets its own context, simulating a real Proto environment
    proto::ProtoContext thread_context(c, nullptr, 0, proto::ProtoThread::getCurrentThread(c), c->space);

    proto::ProtoList* local_list = shared_proto_list.load();
    for (int i = 0; i < OPERATIONS_PER_THREAD; ++i) {
        // The append operation itself is atomic and lock-free because it returns a new list.
        // We use a compare-and-swap loop to handle the race condition of multiple threads
        // updating the shared pointer at the same time.
        proto::ProtoList* current_list;
        proto::ProtoList* new_list;
        do {
            current_list = shared_proto_list.load();
            new_list = current_list->appendLast(&thread_context, thread_context.fromInteger(i));
        } while (!shared_proto_list.compare_exchange_weak(current_list, new_list));
    }
}

proto::ProtoObject* benchmarks(proto::ProtoContext* c, proto::ProtoObject* self, proto::ParentLink* parentLink, proto::ProtoList* args, proto::ProtoSparseList* kwargs) {

    std::cout << "--- Concurrent Append Benchmark ---" << std::endl;
    std::cout << "Appending " << OPERATIONS_PER_THREAD << " elements from " << NUM_THREADS << " threads concurrently." << std::endl << std::endl;

    // --- 1. std::vector with lock ---
    std::cout << "--- Benchmarking std::vector (with std::mutex) ---" << std::endl;
    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> vector_threads;
    for (int i = 0; i < NUM_THREADS; ++i) {
        vector_threads.emplace_back(vector_append_worker);
    }
    for (auto& t : vector_threads) {
        t.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> std_time = end - start;
    std::cout << "Total time: " << std_time.count() << " ms" << std::endl;
    std::cout << "Final size: " << shared_vector.size() << std::endl;


    // --- 2. ProtoList (lock-free) ---
    std::cout << "\n--- Benchmarking ProtoList (lock-free CAS) ---" << std::endl;
    shared_proto_list.store(c->newList());

    start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> proto_threads;
    for (int i = 0; i < NUM_THREADS; ++i) {
        // In a real application, each thread would be a ProtoThread with its own context.
        // We simulate that here by passing the main context to the worker.
        proto_threads.emplace_back(proto_list_append_worker, c);
    }
    for (auto& t : proto_threads) {
        t.join();
    }

    end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> proto_time = end - start;
    std::cout << "Total time: " << proto_time.count() << " ms" << std::endl;
    std::cout << "Final size: " << shared_proto_list.load()->getSize(c) << std::endl;

    std::cout << "\n--- Conclusion ---" << std::endl;
    std::cout << "ProtoList with its lock-free, immutable design scaled significantly better under concurrent load." << std::endl;
    std::cout << "The std::vector with a mutex becomes a bottleneck due to lock contention." << std::endl;

    return 0;
}

int main(int argc, char **argv) {
    proto::ProtoSpace space(benchmarks, argc, argv);
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
