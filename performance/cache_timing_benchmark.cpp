
#include <iostream>
#include <chrono>
#include <vector>
#include <iomanip>
#include <algorithm>
#include <random>
#include "../headers/protoCore.h"

using namespace proto;

void run_benchmark(ProtoContext* c) {
    const int iterations = 10000000; // 10 million
    
    const ProtoString* attr_a = ProtoString::createSymbol(c, "a");
    const ProtoString* attr_c = ProtoString::createSymbol(c, "c");
    
    const ProtoObject* obj1 = c->newObject();
    const_cast<ProtoObject*>(obj1)->setAttribute(c, attr_a, c->fromInteger(42));
    
    const ProtoObject* obj2 = c->newObject();
    const_cast<ProtoObject*>(obj2)->addParent(c, obj1);
    const ProtoObject* obj3 = c->newObject();
    const_cast<ProtoObject*>(obj3)->addParent(c, obj2);
    const_cast<ProtoObject*>(obj3)->setAttribute(c, attr_c, c->fromInteger(100));

    std::cout << "--- protoCore Attribute Cache Benchmark ---" << std::endl;
    std::cout << "Iterations: " << iterations << std::endl << std::endl;

    // 1. Simple Cache Hit
    {
        obj1->getAttribute(c, attr_a);
        auto start = std::chrono::high_resolution_clock::now();
        volatile const ProtoObject* res;
        for (int i = 0; i < iterations; ++i) {
            res = obj1->getAttribute(c, attr_a);
        }
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::nano> diff = end - start;
        std::cout << "1. Simple Cache Hit:           " << std::fixed << std::setprecision(2) 
                  << (diff.count() / iterations) << " ns" << std::endl;
    }

    // 2. Mutable Cache Hit
    {
        const ProtoObject* mobj = c->newObject(true);
        const_cast<ProtoObject*>(mobj)->setAttribute(c, attr_a, c->fromInteger(55));
        mobj->getAttribute(c, attr_a);
        auto start = std::chrono::high_resolution_clock::now();
        volatile const ProtoObject* res;
        for (int i = 0; i < iterations; ++i) {
            res = mobj->getAttribute(c, attr_a);
        }
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::nano> diff = end - start;
        std::cout << "2. Mutable Cache Hit:          " << std::fixed << std::setprecision(2) 
                  << (diff.count() / iterations) << " ns" << std::endl;
    }

    // 3. True Cache Miss (AVL size 1)
    {
        const int num_objs = 4096; // 4x the cache size
        std::vector<const ProtoObject*> objs(num_objs);
        std::vector<const ProtoString*> attrs(num_objs);
        for(int i=0; i<num_objs; ++i) {
            attrs[i] = ProtoString::createSymbol(c, ("a" + std::to_string(i)).c_str());
            objs[i] = c->newObject();
            const_cast<ProtoObject*>(objs[i])->setAttribute(c, attrs[i], c->fromInteger(i));
        }

        auto start = std::chrono::high_resolution_clock::now();
        volatile const ProtoObject* res;
        for (int i = 0; i < iterations; ++i) {
            int idx = i % num_objs;
            res = objs[idx]->getAttribute(c, attrs[idx]);
        }
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::nano> diff = end - start;
        std::cout << "3. Cache Miss (AVL size 1):    " << std::fixed << std::setprecision(2) 
                  << (diff.count() / iterations) << " ns" << std::endl;
    }

    // 4. True Cache Miss (AVL size 50)
    {
        const int num_attrs = 50;
        const int num_objs = 100; // Total 5000 unique (obj, attr) pairs
        std::vector<const ProtoString*> deep_attrs(num_attrs);
        for(int i=0; i<num_attrs; ++i) deep_attrs[i] = ProtoString::createSymbol(c, ("d" + std::to_string(i)).c_str());
        
        std::vector<const ProtoObject*> objs(num_objs);
        for(int i=0; i<num_objs; ++i) {
            objs[i] = c->newObject();
            for(int j=0; j<num_attrs; ++j) const_cast<ProtoObject*>(objs[i])->setAttribute(c, deep_attrs[j], c->fromInteger(j));
        }

        auto start = std::chrono::high_resolution_clock::now();
        volatile const ProtoObject* res;
        for (int i = 0; i < iterations; ++i) {
            int obj_idx = (i / num_attrs) % num_objs;
            int attr_idx = i % num_attrs;
            res = objs[obj_idx]->getAttribute(c, deep_attrs[attr_idx]);
        }
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::nano> diff = end - start;
        std::cout << "4. Cache Miss (AVL size 50):   " << std::fixed << std::setprecision(2) 
                  << (diff.count() / iterations) << " ns" << std::endl;
    }

    std::cout << std::endl;
}

int main() {
    ProtoSpace space;
    run_benchmark(space.rootContext);
    return 0;
}
