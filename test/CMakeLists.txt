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
# Link gtest before gtest_main so that gtest symbols are available to gtest_main.
target_link_libraries(proto_tests PRIVATE protoCore protoCoreConformance gtest gtest_main)
# The self-check matrix (test/ConformanceSelfCheckTests.cpp) needs the internal
# SelfHost header, which lives beside the conformance sources and is not
# installed: no embedder may build a Host out of protoCore's own reference one.
target_include_directories(proto_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../conformance)

# Propagate the survivor re-chain macro to test sources so behaviour-specific
# tests can GTEST_SKIP when the feature is OFF.
if(PROTOCORE_GC_REINCLUDE_SURVIVORS)
    target_compile_definitions(proto_tests PRIVATE PROTOCORE_GC_REINCLUDE_SURVIVORS)
endif()
# Same for the GC instrumentation: diagnostics that abort only in
# instrumented builds are asserted with death tests there.
if(PROTOCORE_GC_INSTRUMENT)
    target_compile_definitions(proto_tests PRIVATE PROTOCORE_GC_INSTRUMENT)
endif()

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

# The three conformance cases that cannot report their own failure in-process:
# one aborts, two deadlock the whole space.  They run one-per-process against
# protoCore's reference host, under an external TIMEOUT, and for the two
# deadlocking cases THE TIMEOUT IS THE VERDICT -- which is why they are ctest
# entries with a bound and not GoogleTest cases.
foreach(_case heap.ceiling_progress join.parks stw.quorum_completes)
    add_test(NAME conformance.isolate.${_case}
             COMMAND protocore-conformance-isolate --case=${_case})
    set_tests_properties(conformance.isolate.${_case}
                         PROPERTIES TIMEOUT 240 RUN_SERIAL TRUE)
endforeach()
