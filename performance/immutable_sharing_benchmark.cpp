#include <iostream>
#include <vector>
#include <chrono>
#include "../headers/proto.h"

const int INITIAL_SIZE = 1000000; // Start with a large base list
const int NUM_VERSIONS = 500;    // Create 500 new "versions" of the list

// A simple function to simulate reading from a vector to prevent optimizations
long long checksum_vector(const std::vector<int>& vec) {
    long long sum = 0;
    for (int val : vec) {
        sum += val;
    }
    return sum;
}

// A simple function to simulate reading from a ProtoList
long long checksum_proto_list(proto::ProtoContext* c, proto::ProtoList* list) {
    long long sum = 0;
    proto::ProtoListIterator* iter = list->getIterator(c);
    while (iter->hasNext(c)) {
        sum += iter->next(c)->asInteger(c);
        iter = iter->advance(c);
    }
    return sum;
}

proto::ProtoObject* benchmarks(proto::ProtoContext* c, proto::ProtoObject* self, proto::ParentLink* parentLink, proto::ProtoList* args, proto::ProtoSparseList* kwargs) {

    std::cout << "--- Immutable Structural Sharing Benchmark ---" << std::endl;
    std::cout << "Creating a base collection of size " << INITIAL_SIZE << " and then creating " << NUM_VERSIONS << " new versions with one element appended." << std::endl << std::endl;

    // --- 1. std::vector (Full Copy) ---
    std::cout << "--- Benchmarking std::vector (requires full copy) ---" << std::endl;

    // Create the base vector
    std::vector<int> base_vector;
    base_vector.reserve(INITIAL_SIZE);
    for (int i = 0; i < INITIAL_SIZE; ++i) {
        base_vector.push_back(i);
    }

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::vector<int>> versions(NUM_VERSIONS);
    for (int i = 0; i < NUM_VERSIONS; ++i) {
        versions[i] = base_vector; // This performs a deep copy
        versions[i].push_back(INITIAL_SIZE + i); // Modify the copy
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> std_copy_time = end - start;

    // The checksum is just to ensure the vectors are being used
    long long total_sum = 0;
    for(const auto& vec : versions) {
        total_sum += vec.back();
    }

    std::cout << "Time to create " << NUM_VERSIONS << " modified versions: " << std_copy_time.count() << " ms" << std::endl;
    std::cout << "(Checksum: " << total_sum << ")" << std::endl;


    // --- 2. ProtoList (Structural Sharing) ---
    std::cout << "\n--- Benchmarking ProtoList (uses structural sharing) ---" << std::endl;

    // Create the base ProtoList
    proto::ProtoList* base_list = c->newList();
    for (int i = 0; i < INITIAL_SIZE; ++i) {
        base_list = base_list->appendLast(c, c->fromInteger(i));
    }

    start = std::chrono::high_resolution_clock::now();

    std::vector<proto::ProtoList*> proto_versions(NUM_VERSIONS);
    for (int i = 0; i < NUM_VERSIONS; ++i) {
        // This operation is extremely fast and memory-efficient.
        // It creates a new list that shares almost all of its nodes with the base_list.
        proto_versions[i] = base_list->appendLast(c, c->fromInteger(INITIAL_SIZE + i));
    }

    end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> proto_copy_time = end - start;

    total_sum = 0;
    for(const auto& list : proto_versions) {
        total_sum += list->getLast(c)->asInteger(c);
    }

    std::cout << "Time to create " << NUM_VERSIONS << " modified versions: " << proto_copy_time.count() << " ms" << std::endl;
    std::cout << "(Checksum: " << total_sum << ")" << std::endl;

    std::cout << "\n--- Conclusion ---" << std::endl;
    std::cout << "ProtoList was approximately " << (int)(std_copy_time.count() / proto_copy_time.count()) << "x faster than std::vector for this operation." << std::endl;
    std::cout << "This demonstrates the power of immutable data structures and structural sharing for versioning and concurrent modifications." << std::endl;

    return 0;
}

int main(int argc, char **argv) {
    proto::ProtoSpace space(benchmarks, argc, argv);
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
