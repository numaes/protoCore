# CMake configuration for the protoCore project
# Official library name: protoCore (shared library)
# This file has been updated to follow modern CMake best practices.

# 1. Set minimum required version and project details
cmake_minimum_required(VERSION 3.16)
project(protoCore
    VERSION 1.0.0
    DESCRIPTION "protoCore - High-Performance Embeddable Dynamic Object System for C++"
    HOMEPAGE_URL "https://github.com/numaes/protoCore"
    LANGUAGES CXX
)

include(GNUInstallDirs)

# 2. Set the C++ standard to C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
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
    core/Proto.cpp
    core/ProtoByteBuffer.cpp
    core/ProtoContext.cpp
    core/ProtoList.cpp
    core/ModuleCache.cpp
    core/ModuleProvider.cpp
    core/ModuleResolver.cpp
    core/ProtoMethodCell.cpp
    core/ProtoObjectCell.cpp
    core/ProviderRegistry.cpp
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

message(STATUS "Configured protoCore shared library target: protoCore")

# 4. Define the performance benchmark executables
add_executable(immutable_sharing_benchmark performance/immutable_sharing_benchmark.cpp)
target_link_libraries(immutable_sharing_benchmark PRIVATE protoCore)
message(STATUS "Configured benchmark: immutable_sharing_benchmark")

add_executable(concurrent_append_benchmark performance/concurrent_append_benchmark.cpp)
target_link_libraries(concurrent_append_benchmark PRIVATE protoCore)
message(STATUS "Configured benchmark: concurrent_append_benchmark")

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

# 6. Installation rules for protoCore shared library
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
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})

# Generator selection
if(WIN32)
    set(CPACK_GENERATOR "ZIP;NSIS")
elseif(APPLE)
    set(CPACK_GENERATOR "TGZ;DragNDrop")
else()
    set(CPACK_GENERATOR "TGZ;DEB;RPM")
endif()

# Only pack the library component by default
set(CPACK_COMPONENTS_ALL protoCore)

include(CPack)

# 8. User-friendly message on completion
message(STATUS "protoCore project configured successfully. Use 'cmake --build <build_dir>' to build protoCore, 'ctest' to run tests, and 'cpack' for packaging.")
