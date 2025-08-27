# CMake configuration for the Proto project

# 1. Set minimum required version and project details
cmake_minimum_required(VERSION 3.16)
project(Proto CXX)

# 2. Set the C++ standard to C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 3. Enable testing with CTest
enable_testing()

# 4. Define the core Proto library
# This is a static library that will contain the runtime's core logic.

# Find all the source files in the 'core' directory automatically
file(GLOB CORE_SOURCES "core/*.cpp")

add_library(proto STATIC ${CORE_SOURCES})

# Make headers public and link libraries.
# By making the include directory PUBLIC, any target that links against 'proto'
# will automatically get the 'headers' directory in its include path.
target_include_directories(proto PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/headers)
target_link_libraries(proto PUBLIC pthread)

message(STATUS "Configured Proto library target.")

# 5. Define the performance benchmark executables
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

# 6. Configure the test suite
# This tells CMake to process the CMakeLists.txt file in the 'test' directory.
add_subdirectory(test)

# 7. User-friendly message on completion
message(STATUS "Proto project configured successfully. Use 'make' to build and 'ctest' to run tests.")
