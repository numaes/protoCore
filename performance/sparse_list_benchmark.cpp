/*
 * sparse_list_benchmark.cpp
 *
 * Deterministic ProtoSparseList benchmark.  Every phase is checked against
 * a std::map model; the program prints VERIFIED and exits 0 only when all
 * phases match, so a silent failure can never be mistaken for a fast run.
 */

#include <chrono>
#include <iostream>
#include <map>
#include <random>
#include <vector>
#include "../headers/protoCore.h"

using namespace proto;

namespace {
    struct IterationSum {
        long long sum;
        unsigned long count;
    };

    void accumulate(ProtoContext* c, void* self, unsigned long key, const ProtoObject* value) {
        auto* s = static_cast<IterationSum*>(self);
        s->sum += static_cast<long long>(key) + value->asLong(c);
        s->count++;
    }

    double seconds(std::chrono::high_resolution_clock::time_point a,
                   std::chrono::high_resolution_clock::time_point b) {
        return std::chrono::duration<double>(b - a).count();
    }
}

int main() {
    ProtoSpace space;
    ProtoContext* c = space.rootContext;

    const int numIterations = 100000;
    const unsigned long keyRange = 10000;
    std::mt19937 gen(12345);
    std::uniform_int_distribution<unsigned long> distrib(0, keyRange);

    std::vector<unsigned long> keys(numIterations);
    std::vector<const ProtoObject*> values(numIterations);
    std::map<unsigned long, long long> model;
    for (int i = 0; i < numIterations; ++i) {
        keys[i] = distrib(gen);
        values[i] = c->fromInteger(i);
        model[keys[i]] = i;
    }

    std::cout << "--- Sparse List Benchmark ---" << std::endl;
    std::cout << "Iterations: " << numIterations << ", key range: " << keyRange << std::endl;

    auto t0 = std::chrono::high_resolution_clock::now();
    const ProtoSparseList* list = c->newSparseList();
    for (int i = 0; i < numIterations; ++i) list = list->setAt(c, keys[i], values[i]);
    auto t1 = std::chrono::high_resolution_clock::now();

    long long checksum = 0;
    for (int i = 0; i < numIterations; ++i) {
        const ProtoObject* v = list->getAt(c, keys[i]);
        if (v != PROTO_NONE) checksum += v->asLong(c);
    }
    auto t2 = std::chrono::high_resolution_clock::now();

    long long expectedChecksum = 0;
    for (int i = 0; i < numIterations; ++i) expectedChecksum += model[keys[i]];

    IterationSum iter{0, 0};
    list->processElements(c, &iter, accumulate);
    auto t3 = std::chrono::high_resolution_clock::now();
    long long expectedIter = 0;
    for (const auto& [k, v] : model) expectedIter += static_cast<long long>(k) + v;
    const unsigned long pairsBeforeRemoval = model.size();

    for (int i = 0; i < numIterations; i += 2) {
        list = list->removeAt(c, keys[i]);
        model.erase(keys[i]);
    }
    auto t4 = std::chrono::high_resolution_clock::now();

    const bool okAccess = checksum == expectedChecksum;
    const bool okIter = iter.sum == expectedIter && iter.count == pairsBeforeRemoval;
    const bool okRemove = list->getSize(c) == model.size();

    std::cout << "Insertion time: " << seconds(t0, t1) << " s" << std::endl;
    std::cout << "Access time: " << seconds(t1, t2) << " s" << std::endl;
    std::cout << "Iteration time: " << seconds(t2, t3) << " s" << std::endl;
    std::cout << "Removal time: " << seconds(t3, t4) << " s" << std::endl;
    std::cout << "Access checksum: " << checksum << " (expected " << expectedChecksum << ")" << std::endl;
    std::cout << "Iteration sum: " << iter.sum << " over " << iter.count << " pairs (expected " << expectedIter << ")" << std::endl;
    std::cout << "Size after removal: " << list->getSize(c) << " (expected " << model.size() << ")" << std::endl;
    const bool ok = okAccess && okIter && okRemove;
    std::cout << (ok ? "VERIFIED" : "MISMATCH") << std::endl;
    return ok ? 0 : 1;
}
