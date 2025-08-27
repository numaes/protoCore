#include <iostream>
#include <string>
#include <chrono>
#include "../headers/proto.h"

const int NUM_CONCATS = 50000;
const char* SMALL_STRING = "abc ";

proto::ProtoObject* benchmarks(proto::ProtoContext* c, proto::ProtoObject* self, proto::ParentLink* parentLink, proto::ProtoList* args, proto::ProtoSparseList* kwargs) {

    std::cout << "--- String Concatenation Benchmark ---" << std::endl;
    std::cout << "Appending a small string " << NUM_CONCATS << " times." << std::endl << std::endl;

    // --- 1. std::string (repeated reallocations) ---
    std::cout << "--- Benchmarking std::string ---" << std::endl;

    auto start = std::chrono::high_resolution_clock::now();

    std::string std_str = "";
    for (int i = 0; i < NUM_CONCATS; ++i) {
        std_str += SMALL_STRING; // This can cause many reallocations and copies
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> std_time = end - start;

    std::cout << "Total time: " << std_time.count() << " ms" << std::endl;
    std::cout << "Final string length: " << std_str.length() << std::endl;


    // --- 2. ProtoString (rope implementation) ---
    std::cout << "\n--- Benchmarking ProtoString ---" << std::endl;

    // Pre-create the small string to append
    proto::ProtoString* proto_small_string = c->fromUTF8String(SMALL_STRING);

    start = std::chrono::high_resolution_clock::now();

    proto::ProtoString* proto_str = c->fromUTF8String("");
    for (int i = 0; i < NUM_CONCATS; ++i) {
        // This is very fast. It doesn't copy the string data, it just creates
        // a new tree node pointing to the existing strings (a rope).
        proto_str = proto_str->appendLast(c, proto_small_string);
    }

    end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> proto_time = end - start;

    std::cout << "Total time: " << proto_time.count() << " ms" << std::endl;
    std::cout << "Final string length: " << proto_str->getSize(c) << std::endl;

    std::cout << "\n--- Conclusion ---" << std::endl;
    std::cout << "ProtoString was approximately " << (int)(std_time.count() / proto_time.count()) << "x faster than std::string." << std::endl;
    std::cout << "This is because ProtoString uses a rope data structure, avoiding costly memory reallocations and copies during concatenation." << std::endl;

    return 0;
}

int main(int argc, char **argv) {
    proto::ProtoSpace space(benchmarks, argc, argv);
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
