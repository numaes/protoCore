# CMake configuration for the Proto project
# This file has been updated to follow modern CMake best practices.

# 1. Set minimum required version and project details
cmake_minimum_required(VERSION 3.16)
project(Proto CXX)

# 2. Set the C++ standard to C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF) # Prefer explicit standard flags

# 3. Define the core Proto static library
# Source files are listed explicitly to ensure robustness and prevent issues
# where newly added files are not automatically detected by the build system.
add_library(proto STATIC
    core/Cell.cpp
    core/Double.cpp
    core/LargeInteger.cpp
    core/ParentLink.cpp
    core/Proto.cpp
    core/ProtoByteBuffer.cpp
    core/ProtoContext.cpp
    core/ProtoList.cpp
    core/ProtoMethodCell.cpp
    core/ProtoObjectCell.cpp
    core/ProtoSpace.cpp
    core/ProtoSparseList.cpp
    core/ProtoString.cpp
    core/ProtoTuple.cpp
    core/Thread.cpp
    core/ProtoExternalPointer.cpp
    core/ProtoSet.cpp
    core/ProtoMultiset.cpp
    core/Integer.cpp
)

# The headers directory is made public so that any consumer linking against
# 'proto' will automatically have access to the necessary headers.
target_include_directories(proto PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/headers>
    $<INSTALL_INTERFACE:include> # For future installation rules
)

# Find and link the platform-native Threads library.
find_package(Threads REQUIRED)
target_link_libraries(proto PUBLIC Threads::Threads)

message(STATUS "Configured Proto library target: proto")

# 4. Define the performance benchmark executables
# Each benchmark is defined as a separate executable.
add_executable(immutable_sharing_benchmark performance/immutable_sharing_benchmark.cpp)
target_link_libraries(immutable_sharing_benchmark PRIVATE proto)
message(STATUS "Configured benchmark: immutable_sharing_benchmark")

add_executable(concurrent_append_benchmark performance/concurrent_append_benchmark.cpp)
target_link_libraries(concurrent_append_benchmark PRIVATE proto)
message(STATUS "Configured benchmark: concurrent_append_benchmark")

# 5. Configure the test suite
# The 'enable_testing()' command allows the use of ctest.
enable_testing()
add_subdirectory(test)

# 6. User-friendly message on completion
message(STATUS "Proto project configured successfully. Use 'make' to build and 'ctest' to run tests.")
