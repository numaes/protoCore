/*
 * string_concat_benchmark.cpp
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
    const int num_iterations = 10000;
    std::cout << "--- String Concatenation Benchmark ---" << std::endl;
    std::cout << "Iterations: " << num_iterations << std::endl;

    // --- Proto String ---
    auto start_proto = std::chrono::high_resolution_clock::now();
    const ProtoString* proto_string = c->fromUTF8String("")->asString(c);
    const ProtoString* to_append = c->fromUTF8String("a")->asString(c);
    for (int i = 0; i < num_iterations; ++i) {
        proto_string = proto_string->appendLast(c, to_append);
    }
    auto end_proto = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff_proto = end_proto - start_proto;
    std::cout << "Proto string time: " << diff_proto.count() << " s" << std::endl;

    // --- Verification ---
    if (proto_string->getSize(c) != num_iterations) {
        std::cerr << "Size mismatch! Got " << proto_string->getSize(c) << ", expected " << num_iterations << std::endl;
    } else {
        std::cout << "Size verified." << std::endl;
    }

    std::cout << "------------------------------------" << std::endl;
    return PROTO_NONE;
}

int main(int argc, char* argv[]) {
    // Corrected: ProtoSpace constructor takes no arguments
    proto::ProtoSpace space;
    benchmarks(space.rootContext, nullptr, nullptr, nullptr, nullptr);
    return 0;
}
