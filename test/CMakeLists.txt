# CMake configuration for the Proto test suite

# Set the policy for FetchContent to avoid the timestamp warning
cmake_policy(SET CMP0135 NEW)

# 1. Fetch Google Test
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)

# Make googletest available
FetchContent_MakeAvailable(googletest)

# 2. Find all test source files
file(GLOB TEST_SOURCES "*.cpp")

# 3. Create an executable for the test suite
add_executable(proto_tests ${TEST_SOURCES})

# 4. Link the test suite against our core 'proto' library and Google Test
target_link_libraries(proto_tests PRIVATE proto gtest_main)

# 5. Include Google Test headers
target_include_directories(proto_tests PRIVATE ${googletest_SOURCE_DIR}/googletest/include)

# 6. Add a test to CTest for easy execution
include(GoogleTest)
gtest_discover_tests(proto_tests)

message(STATUS "Configured test suite: proto_tests")
