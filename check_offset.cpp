#include <iostream>
#include <cstddef>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <memory>
#include "/home/gamarino/Documentos/proyectos/protoCore/headers/protoCore.h"

int main() {
    std::cout << "sizeof(ProtoSpace) = " << sizeof(proto::ProtoSpace) << std::endl;
    std::cout << "offset of literalData = " << offsetof(proto::ProtoSpace, literalData) << std::endl;
    std::cout << "offset of nonMethodCallback = " << offsetof(proto::ProtoSpace, nonMethodCallback) << std::endl;
    std::cout << "offset of mainContext = " << offsetof(proto::ProtoSpace, mainContext) << std::endl;
    std::cout << "offset of resolutionChain_ = " << offsetof(proto::ProtoSpace, resolutionChain_) << std::endl;
    std::cout << "offset of moduleRoots = " << offsetof(proto::ProtoSpace, moduleRoots) << std::endl;
    return 0;
}
