# CMake configuration for the protoCore project
# Official library name: protoCore (shared library)
# This file has been updated to follow modern CMake best practices.

# 1. Set minimum required version and project details
cmake_minimum_required(VERSION 3.16)
project(protoCore
    VERSION 1.2.0
    DESCRIPTION "protoCore - High-Performance Embeddable Dynamic Object System for C++"
    HOMEPAGE_URL "https://github.com/numaes/protoCore"
    LANGUAGES CXX
)

include(GNUInstallDirs)

# 2. Set the C++ standard to C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-fno-delete-null-pointer-checks)
set(CMAKE_CXX_EXTENSIONS OFF) # Prefer explicit standard flags

# Option: enable code coverage (gcov/lcov). Requires rebuild with -DCOVERAGE=ON.
option(COVERAGE "Build with coverage instrumentation (gcov) for coverage reports" OFF)
if(COVERAGE)
    set(COVERAGE_FLAGS "--coverage")
    message(STATUS "Coverage enabled: compiler/linker flags ${COVERAGE_FLAGS}")
endif()

# 3. Define the core protoCore shared library (official name)
# Source files are listed explicitly to ensure robustness and prevent issues
# where newly added files are not automatically detected by the build system.
add_library(protoCore SHARED
    core/Cell.cpp
    core/Double.cpp
    core/LargeInteger.cpp
    core/ParentLink.cpp
    core/ProtoByteBuffer.cpp
    core/ProtoContext.cpp
    core/ProtoList.cpp
    core/ModuleCache.cpp
    core/ModuleProvider.cpp
    core/ModuleResolver.cpp
    core/ProtoMethodCell.cpp
    core/ProtoObject.cpp
    core/ProviderRegistry.cpp
    core/ProtoSpace.cpp
    core/ProtoRootSet.cpp
    core/ProtoSparseList.cpp
    core/ProtoString.cpp
    core/SymbolTable.cpp
    core/ProtoTuple.cpp
    core/Thread.cpp
    core/ProtoExternalPointer.cpp
    core/ProtoExternalBuffer.cpp
    core/ProtoSet.cpp
    core/ProtoMultiset.cpp
    core/Integer.cpp
)

# Shared library versioning (version for linker, soversion for ABI)
set_target_properties(protoCore PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    OUTPUT_NAME protoCore
    PUBLIC_HEADER "headers/protoCore.h"
)

# The headers directory is made public so that any consumer linking against
# 'protoCore' will automatically have access to the necessary headers.
target_include_directories(protoCore PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/headers>
    $<INSTALL_INTERFACE:include>
)

# Find and link the platform-native Threads library.
find_package(Threads REQUIRED)
target_link_libraries(protoCore PUBLIC Threads::Threads)

# Resolve intra-DSO calls directly instead of round-tripping through the
# PLT/GOT.  Profile of nqueens showed every recursive ProtoContext teardown
# walking through 5 PLT trampolines (implSetCurrentContext,
# submitYoungGeneration, asCell, Cell::operator new, ReturnReference ctor)
# even though all callees live in the same shared library.  Default-
# visibility ELF symbols force the trampoline to support runtime symbol
# interposition; -Bsymbolic-functions opts out of that for function
# references in this DSO.  Equivalent to -fvisibility=hidden but lighter to
# adopt: no source-level annotation needed, and consumers of the public
# API are unaffected because the symbols stay exported.
if(NOT WIN32 AND NOT APPLE)
    target_link_options(protoCore PRIVATE "-Wl,-Bsymbolic-functions")
endif()

if(COVERAGE)
    target_compile_options(protoCore PRIVATE ${COVERAGE_FLAGS})
    target_link_options(protoCore PUBLIC ${COVERAGE_FLAGS})
endif()

# Option: trace globalMutex acquire/release in GC path (getFreeCells, gcThreadLoop, allocCell STW).
# Use -DPROTO_GC_LOCK_TRACE=ON when configuring to diagnose intermittent benchmark hang.
option(PROTO_GC_LOCK_TRACE "Trace GC path lock acquisition and release to stderr" OFF)
if(PROTO_GC_LOCK_TRACE)
    target_compile_definitions(protoCore PRIVATE PROTO_GC_LOCK_TRACE)
    message(STATUS "GC lock tracing enabled (diagnostic)")
endif()

# Option: re-include GC survivors into the next analysis cycle and enable
# per-context allocation-threshold submission. See
# docs/superpowers/specs/2026-05-03-gc-survivor-rechain.md for the design.
#
# Default ON since the May 2026 audit: full protoCore (148/148) and
# protoPython (163/163) test suites pass, and protoPython benchmarks
# show ~14 % geomean improvement vs the OFF baseline plus a ~3.8x RSS
# reduction on `memory_pressure`.  Configure with
# -DPROTOCORE_GC_REINCLUDE_SURVIVORS=OFF to bisect against the previous
# behaviour.
option(PROTOCORE_GC_REINCLUDE_SURVIVORS "Re-chain sweep survivors into dirtySegments and enable per-context alloc threshold" ON)
if(PROTOCORE_GC_REINCLUDE_SURVIVORS)
    target_compile_definitions(protoCore PRIVATE PROTOCORE_GC_REINCLUDE_SURVIVORS)
    message(STATUS "GC survivor re-chain enabled (default)")
else()
    message(STATUS "GC survivor re-chain DISABLED — will exhibit the historical survivor-tenuring leak")
endif()

# Opt-in per-phase instrumentation in gcThreadLoop.  When enabled, the
# Phase 1/2/4/5/6 wall-times, total cells marked, and total segments
# swept are accumulated; on every 5 GC cycles a one-line summary is
# printed to stderr if PROTOCORE_GC_PROFILE=1 is set in the environment.
# Default OFF — the chrono::now() and atomic fetch_add calls would
# otherwise fire on every cycle and add a few microseconds of overhead
# per cycle in production builds.
option(PROTOCORE_GC_INSTRUMENT "Compile per-phase gcThreadLoop instrumentation (env var PROTOCORE_GC_PROFILE=1 to print)" OFF)
if(PROTOCORE_GC_INSTRUMENT)
    target_compile_definitions(protoCore PRIVATE PROTOCORE_GC_INSTRUMENT)
    message(STATUS "GC per-phase instrumentation compiled in (set PROTOCORE_GC_PROFILE=1 at runtime to print)")
endif()

message(STATUS "Configured protoCore shared library target: protoCore")

# 4. Define the performance benchmark executables
add_executable(immutable_sharing_benchmark performance/immutable_sharing_benchmark.cpp)
target_link_libraries(immutable_sharing_benchmark PRIVATE protoCore)
message(STATUS "Configured benchmark: immutable_sharing_benchmark")

add_executable(concurrent_append_benchmark performance/concurrent_append_benchmark.cpp)
target_link_libraries(concurrent_append_benchmark PRIVATE protoCore)
message(STATUS "Configured benchmark: concurrent_append_benchmark")

add_executable(list_benchmark performance/list_benchmark.cpp)
target_link_libraries(list_benchmark PRIVATE protoCore)
message(STATUS "Configured benchmark: list_benchmark")

add_executable(object_access_benchmark performance/object_access_benchmark.cpp)
target_link_libraries(object_access_benchmark PRIVATE protoCore)
message(STATUS "Configured benchmark: object_access_benchmark")

add_executable(mutable_access_benchmark performance/mutable_access_benchmark.cpp)
target_link_libraries(mutable_access_benchmark PRIVATE protoCore)
message(STATUS "Configured benchmark: mutable_access_benchmark")

add_executable(sparse_list_benchmark performance/sparse_list_benchmark.cpp)
target_link_libraries(sparse_list_benchmark PRIVATE protoCore)
message(STATUS "Configured benchmark: sparse_list_benchmark")

add_executable(string_concat_benchmark performance/string_concat_benchmark.cpp)
target_link_libraries(string_concat_benchmark PRIVATE protoCore)
message(STATUS "Configured benchmark: string_concat_benchmark")

add_executable(cache_timing_benchmark performance/cache_timing_benchmark.cpp)
target_link_libraries(cache_timing_benchmark PRIVATE protoCore)
message(STATUS "Configured benchmark: cache_timing_benchmark")

add_executable(microbenchmark_final performance/microbenchmark_final.cpp)
target_link_libraries(microbenchmark_final PRIVATE protoCore)
message(STATUS "Configured benchmark: microbenchmark_final")

# 5. Configure the test suite
# The 'enable_testing()' command allows the use of ctest.
enable_testing()
add_subdirectory(test)

# Custom target to generate coverage report (only when COVERAGE=ON).
# Run: cmake --build build --target coverage
# Requires: build configured with -DCOVERAGE=ON, and lcov/genhtml installed.
if(COVERAGE)
    set(COVERAGE_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/scripts/coverage.sh")
    if(EXISTS "${COVERAGE_SCRIPT}")
        add_custom_target(coverage
            COMMAND "${COVERAGE_SCRIPT}" "${CMAKE_CURRENT_BINARY_DIR}"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            COMMENT "Running tests and generating coverage report"
            USES_TERMINAL
        )
        message(STATUS "Coverage target added: run 'cmake --build build --target coverage' to generate report")
    endif()
endif()

# 6. Installation rules + packaging for protoCore.
#
# Guarded to the top-level build only. When protoCore is pulled in via
# add_subdirectory() by a sibling project (protoPython does this), that
# project owns packaging — protoCore must NOT inject its own install rules
# (which would bundle libprotoCore into the sibling's package) or its CPack
# config. Each sibling instead declares a dependency on protoCore's package.
if(CMAKE_PROJECT_NAME STREQUAL "protoCore")

set(PROTOCORE_INSTALL_COMPONENT "protoCore")
install(TARGETS protoCore
    EXPORT protoCoreTargets
    COMPONENT ${PROTOCORE_INSTALL_COMPONENT}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# 7. Packaging configuration (CPack)
set(CPACK_PACKAGE_NAME "protoCore")
set(CPACK_PACKAGE_VENDOR "numaes")
set(CPACK_PACKAGE_CONTACT "gamarino@gmail.com")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "protoCore - High-Performance Embeddable Dynamic Object System for C++")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_RPM_PACKAGE_LICENSE "MIT")
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})

# Generator selection: only enable packages for the current OS so cpack does not fail
# when tools for other formats (e.g. rpmbuild) are not installed.
if(WIN32)
    set(CPACK_GENERATOR "ZIP;NSIS")
elseif(APPLE)
    set(CPACK_GENERATOR "TGZ;DragNDrop")
else()
    set(CPACK_GENERATOR "TGZ")
    find_program(DPKG_EXECUTABLE dpkg)
    find_program(RPMBUILD_EXECUTABLE rpmbuild)
    if(DPKG_EXECUTABLE)
        list(APPEND CPACK_GENERATOR "DEB")
        message(STATUS "CPack: DEB generator enabled (dpkg found)")
    endif()
    if(RPMBUILD_EXECUTABLE)
        list(APPEND CPACK_GENERATOR "RPM")
        message(STATUS "CPack: RPM generator enabled (rpmbuild found)")
    endif()
endif()

# Pack only the runtime library and public header (no test frameworks)
set(CPACK_COMPONENTS_ALL protoCore)
# Ensure component-based install is used when packaging
set(CPACK_COMPONENT_PROTOCORE_DISPLAY_NAME "protoCore runtime")
set(CPACK_COMPONENT_PROTOCORE_DESCRIPTION "Shared library and public header for embedding")
include(CPack)

endif()  # CMAKE_PROJECT_NAME STREQUAL "protoCore" — install + packaging

# Optional: custom target to create a clean TGZ with only protoCore (no test frameworks)
# Copies built library and public header into a staging dir and tars it (avoids install manifest).
set(PACKAGE_STAGING_DIR "${CMAKE_BINARY_DIR}/package_staging")
set(PACKAGE_TOP_DIR "protoCore-${PROJECT_VERSION}-Linux")
add_custom_target(package_protocore_only
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${PACKAGE_STAGING_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PACKAGE_STAGING_DIR}/${PACKAGE_TOP_DIR}/include"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PACKAGE_STAGING_DIR}/${PACKAGE_TOP_DIR}/lib"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:protoCore>" "${PACKAGE_STAGING_DIR}/${PACKAGE_TOP_DIR}/lib/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/headers/protoCore.h" "${PACKAGE_STAGING_DIR}/${PACKAGE_TOP_DIR}/include/"
    COMMAND tar czf "${CMAKE_BINARY_DIR}/protoCore-${PROJECT_VERSION}-Linux.tar.gz" -C "${PACKAGE_STAGING_DIR}" "${PACKAGE_TOP_DIR}"
    COMMAND ${CMAKE_COMMAND} -E echo "Created ${CMAKE_BINARY_DIR}/protoCore-${PROJECT_VERSION}-Linux.tar.gz"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Create TGZ with only protoCore runtime and header"
)

# 8. User-friendly message on completion
message(STATUS "protoCore project configured successfully. Use 'cmake --build <build_dir>' to build protoCore, 'ctest' to run tests, and 'cpack' for packaging.")
