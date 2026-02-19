# CMake configuration for the Proto test suite

# Set the policy for FetchContent to avoid the timestamp warning
cmake_policy(SET CMP0135 NEW)

# 1. Fetch Google Test (test-only dependency; do not install with the library package)
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
# Disable GTest/GMock install rules so packaging only includes protoCore
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

# Make googletest available
FetchContent_MakeAvailable(googletest)

# 2. Find all test source files
file(GLOB TEST_SOURCES "*.cpp")

# 3. Create an executable for the test suite
add_executable(proto_tests ${TEST_SOURCES})

# 4. Link the test suite against the protoCore shared library and Google Test
target_link_libraries(proto_tests PRIVATE protoCore gtest_main)

# Coverage instrumentation when COVERAGE=ON (set in parent CMakeLists.txt)
if(COVERAGE AND DEFINED COVERAGE_FLAGS)
    target_compile_options(proto_tests PRIVATE ${COVERAGE_FLAGS})
    target_link_options(proto_tests PRIVATE ${COVERAGE_FLAGS})
endif()

# 5. Include Google Test headers
target_include_directories(proto_tests PRIVATE ${googletest_SOURCE_DIR}/googletest/include)

# 6. Add a test to CTest for easy execution
include(GoogleTest)
gtest_discover_tests(proto_tests)

message(STATUS "Configured test suite: proto_tests")
