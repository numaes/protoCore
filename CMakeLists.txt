# CMake configuration for the Proto project

# 1. Set minimum required version and project details
cmake_minimum_required(VERSION 3.16)
project(Proto CXX)

# Set CMake policies to avoid warnings
cmake_policy(SET CMP0135 NEW)

# 2. Set the C++ standard to C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 3. Enable testing with CTest
enable_testing()

# 4. Define the core Proto library
file(GLOB CORE_SOURCES "core/*.cpp")
add_library(proto STATIC ${CORE_SOURCES})
target_include_directories(proto PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/headers)
target_link_libraries(proto PUBLIC pthread)
message(STATUS "Configured Proto library target.")

# 5. Define the performance benchmark executables
file(GLOB BENCHMARK_SOURCES "performance/*.cpp")
foreach(BENCH_SOURCE ${BENCHMARK_SOURCES})
    get_filename_component(TARGET_NAME ${BENCH_SOURCE} NAME_WE)
    add_executable(${TARGET_NAME} ${BENCH_SOURCE})
    target_link_libraries(${TARGET_NAME} PRIVATE proto)
    message(STATUS "Configured benchmark: ${TARGET_NAME}")
endforeach()

# 6. Configure the test suite
add_subdirectory(test)

# 7. User-friendly message on completion
message(STATUS "Proto project configured successfully. Use 'make' to build and 'ctest' to run tests.")
