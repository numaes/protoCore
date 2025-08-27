# CMake configuration for the Proto project

# 1. Set minimum required version and project details
cmake_minimum_required(VERSION 3.16)
project(Proto CXX)

# 2. Set the C++ standard to C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 3. Define the core Proto library
# This is a static library that will contain the runtime's core logic.

# Find all the source files in the 'core' directory automatically
file(GLOB CORE_SOURCES "core/*.cpp")

add_library(proto STATIC ${CORE_SOURCES})

# Make sure that any target linking against 'proto' automatically gets the
# 'headers' directory in its include path.
target_include_directories(proto PUBLIC headers)

# Enable pthreads for std::thread support
target_link_libraries(proto PRIVATE pthread)

message(STATUS "Configured Proto library target.")

# 4. Define the performance benchmark executables
# This will automatically find every .cpp file in the 'performance' directory
# and create a separate executable for each one.

file(GLOB BENCHMARK_SOURCES "performance/*.cpp")

foreach(BENCH_SOURCE ${BENCHMARK_SOURCES})
    # Get the filename without the extension to use as the target name
    get_filename_component(TARGET_NAME ${BENCH_SOURCE} NAME_WE)

    add_executable(${TARGET_NAME} ${BENCH_SOURCE})

    # Link the benchmark executable against our core 'proto' library
    target_link_libraries(${TARGET_NAME} PRIVATE proto)

    message(STATUS "Configured benchmark: ${TARGET_NAME}")
endforeach()

# 5. User-friendly message on completion
message(STATUS "Proto project configured successfully. Use 'make' to build.")
