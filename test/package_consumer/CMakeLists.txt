# A minimal consumer of an INSTALLED protoCore package, used by the Windows CI
# job to check the ZIP that cpack produces: find_package must accept it (the
# ABI check in protoCoreConfig.cmake finds protoCore-<SOVERSION>.dll), and the
# program must run from a directory holding only itself and the ZIP's bin/.
# Not part of the protoCore build.
cmake_minimum_required(VERSION 3.16)
project(protocore_package_consumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
    add_compile_options(/utf-8)
endif()
find_package(protoCore 2.9 REQUIRED CONFIG)
add_executable(package_consumer main.cpp)
target_link_libraries(package_consumer PRIVATE protoCore::protoCore)
