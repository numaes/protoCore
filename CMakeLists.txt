# CMake configuration for the Proto project
# This file has been updated to follow modern CMake best practices.

# 1. Set minimum required version and project details
cmake_minimum_required(VERSION 3.16)
project(Proto 
    VERSION 1.0.0
    DESCRIPTION "Proto core library"
    HOMEPAGE_URL "https://github.com/proto-project/proto"
    LANGUAGES CXX
)

include(GNUInstallDirs)

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

# 6. Installation rules
set(PROTO_INSTALL_COMPONENT "proto_lib")
install(TARGETS proto
    EXPORT ProtoTargets
    COMPONENT ${PROTO_INSTALL_COMPONENT}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES headers/protoCore.h
    COMPONENT ${PROTO_INSTALL_COMPONENT}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# 7. Packaging configuration (CPack)
set(CPACK_PACKAGE_VENDOR "ProtoProject")
set(CPACK_PACKAGE_CONTACT "admin@proto.org")
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
set(CPACK_COMPONENTS_ALL proto_lib)

include(CPack)

# 8. User-friendly message on completion
message(STATUS "Proto project configured successfully. Use 'cmake --build <build_dir>' to build, 'ctest' to run tests, and 'cpack' for packaging.")
