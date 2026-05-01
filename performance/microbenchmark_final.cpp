#include <iostream>
#include <chrono>
#include <vector>
#include <iomanip>
#include "../headers/protoCore.h"

using namespace proto;

// Prevent optimization
static volatile const ProtoObject* g_sink;

void run_microbenchmark(ProtoContext* c) {
    const int iterations = 10000000;
    std::cout << "=== ProtoCore Final Performance Metrics (Interned Symbols) ===" << std::endl;
    std::cout << "Iterations: " << iterations << " per test" << std::endl;

    // Setup: Object with 1 attribute, using INTERNED SYMBOL
    const ProtoString* attr_name = ProtoString::createSymbol(c, "test_attr");
    const ProtoObject* obj = c->newObject(true)->setAttribute(c, attr_name, c->fromInteger(42));

    // Warm up
    for(int i=0; i<1000; ++i) g_sink = obj->getAttribute(c, attr_name);

    // 1. getAttribute - Hot Cache
    {
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < iterations; ++i) {
            g_sink = obj->getAttribute(c, attr_name);
        }
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::nano> ns = end - start;
        std::cout << std::left << std::setw(45) << "getAttribute (Hot Cache):" 
                  << std::fixed << std::setprecision(2) << ns.count() / iterations << " ns/op" << std::endl;
    }

    // 2. hasAttribute - Hot Cache
    {
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < iterations; ++i) {
            g_sink = obj->hasAttribute(c, attr_name);
        }
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::nano> ns = end - start;
        std::cout << std::left << std::setw(45) << "hasAttribute (Hot Cache):" 
                  << std::fixed << std::setprecision(2) << ns.count() / iterations << " ns/op" << std::endl;
    }

    // 3. getOwnAttributeDirect
    {
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < iterations; ++i) {
            g_sink = obj->getOwnAttributeDirect(c, attr_name);
        }
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::nano> ns = end - start;
        std::cout << std::left << std::setw(45) << "getOwnAttributeDirect (Hot Cache):" 
                  << std::fixed << std::setprecision(2) << ns.count() / iterations << " ns/op" << std::endl;
    }

    // 4. Deep Inheritance (10 levels)
    const ProtoObject* deep_obj = obj;
    for (int i = 0; i < 10; ++i) {
        deep_obj = deep_obj->newChild(c, false);
    }
    {
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < iterations; ++i) {
            g_sink = deep_obj->getAttribute(c, attr_name);
        }
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::nano> ns = end - start;
        std::cout << std::left << std::setw(45) << "getAttribute (10-level inheritance):" 
                  << std::fixed << std::setprecision(2) << ns.count() / iterations << " ns/op" << std::endl;
    }

    // 5. Cold Cache (Worst case: walkthrough chain)
    // To test cold cache we'd need to flush the cache. 
    // Since we don't have a flush API, we can use different objects/names.
    // But for 1 attribute, implGetAt is also very fast.

    std::cout << "===========================================================" << std::endl;
}

int main() {
    proto::ProtoSpace space;
    run_microbenchmark(space.rootContext);
    return 0;
}
