# CMake configuration for the protoCore project
# Official library name: protoCore (shared library)
# This file has been updated to follow modern CMake best practices.

# 1. Set minimum required version and project details
cmake_minimum_required(VERSION 3.16)
project(protoCore
    VERSION 2.10.2
    DESCRIPTION "protoCore - High-Performance Embeddable Dynamic Object System for C++"
    HOMEPAGE_URL "https://github.com/numaes/protoCore"
    LANGUAGES CXX
)

include(GNUInstallDirs)

# 2. Set the C++ standard to C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
    # Sources are UTF-8; MSVC otherwise reads them in the console code page.
    add_compile_options(/utf-8)
    # Warning level 3 for protoCore's own targets.  Since CMake 3.15
    # (CMP0092) no /W flag is added by default, which leaves MSVC at /W1 and
    # hides the truncation (C4244, C4267) and printf-format (C4477) warnings.
    # Directory-scoped, so a project that pulls protoCore in with
    # add_subdirectory keeps its own warning level.
    add_compile_options(/W3)
    # At /W3 MSVC also reports C4996 for getenv and fopen, recommending the
    # Microsoft-only _dupenv_s / fopen_s.  protoCore uses the standard C
    # functions on purpose (one code path for every platform; its getenv
    # calls read configuration once, not concurrently with setenv), so the
    # CRT deprecation is switched off rather than the code forked.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-fno-delete-null-pointer-checks)
endif()
set(CMAKE_CXX_EXTENSIONS OFF) # Prefer explicit standard flags

# Option: enable code coverage (gcov/lcov). Requires rebuild with -DCOVERAGE=ON.
option(COVERAGE "Build with coverage instrumentation (gcov) for coverage reports" OFF)
if(COVERAGE)
    set(COVERAGE_FLAGS "--coverage")
    message(STATUS "Coverage enabled: compiler/linker flags ${COVERAGE_FLAGS}")
endif()

# Windows has no rpath: a program finds its DLLs next to itself.  Put every
# executable and DLL of the build in one directory so tests and benchmarks run
# in place.  Only on Windows; other platforms keep their usual layout.
if(WIN32 AND NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
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
    core/ModuleIdentity.cpp
    core/ModuleRoots.cpp
    core/ProtoMethodCell.cpp
    core/ProtoObject.cpp
    core/ProviderRegistry.cpp
    core/ProtoSpace.cpp
    core/ProtoRootSet.cpp
    core/ProtoSparseList.cpp
    core/ProtoMap.cpp
    core/ProtoMPSCQueue.cpp
    core/ProtoHashedCollection.cpp
    core/ProtoString.cpp
    core/SymbolTable.cpp
    core/ProtoTuple.cpp
    core/Thread.cpp
    core/ThreadStack.cpp
    core/ProtoExternalPointer.cpp
    core/ProtoExternalBuffer.cpp
    core/ProtoSet.cpp
    core/ProtoMultiset.cpp
    core/Integer.cpp
    core/MutableCycles.cpp
    core/MultiSpace.cpp
    core/AdaptiveHeap.cpp
)

# The ABI version of libprotoCore. It is the single source of truth for the
# soname, for the generated CMake package config and for protoCore.pc, so a
# consumer's check and the file on disk can never disagree. Bump it only for a
# breaking ABI change, as 1 -> 2 was (see CHANGELOG.md).
#
# 2 -> 3 in 2.2.0 (P3, global interning).  The ProtoSpace layout is byte-for-byte
# identical — no field added, removed or reordered, and the additions are one
# class and four non-virtual methods — so a stale embedder binary LINKS AND RUNS
# and is simply wrong about symbol identity in a multi-space process.  That is
# the one failure mode this project cannot catch at run time: there is no crash,
# no diagnostic, just a getAttribute that returns PROTO_NONE, which is also a
# legitimate value.  A soname bump converts it into a load-time error.  It costs
# a one-line change in each of the five runtimes today, and a great deal later.
set(PROTOCORE_ABI_SOVERSION 3)

# Shared library versioning (version for linker, soversion for ABI)
set_target_properties(protoCore PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROTOCORE_ABI_SOVERSION}
    OUTPUT_NAME protoCore
    PUBLIC_HEADER "headers/protoCore.h"
)

# The public API carries no __declspec annotations; on Windows the linker
# exports every symbol of the DLL instead, which is what ELF does by default.
if(WIN32)
    set_target_properties(protoCore PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    target_compile_definitions(protoCore PRIVATE PROTOCORE_BUILDING_DLL)
    # A DLL has no soname, so the ABI version goes into its file name:
    # protoCore-3.dll, the counterpart of libprotoCore.so.3.  The import
    # library keeps the plain name (protoCore.lib) and records the versioned
    # DLL name, so a consumer links as before and its program then loads
    # exactly the ABI it was built against; a protoCore with another
    # SOVERSION is a missing DLL at start-up, never a silent mismatch.  The
    # package config checks for this file (cmake/protoCoreConfig.cmake.in).
    set_target_properties(protoCore PROPERTIES
        RUNTIME_OUTPUT_NAME "protoCore-${PROTOCORE_ABI_SOVERSION}")
endif()

# The headers directory is made public so that any consumer linking against
# 'protoCore' will automatically have access to the necessary headers.
target_include_directories(protoCore PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/headers>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
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
# docs/archive/design-specs/2026-05-03-gc-survivor-rechain.md for the design.
#
# Default ON since the May 2026 audit: full protoCore (255/255) and
# protoPython (392/392) test suites pass, and protoPython benchmarks
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

add_executable(adaptive_heap_benchmark performance/adaptive_heap_benchmark.cpp)
target_link_libraries(adaptive_heap_benchmark PRIVATE protoCore)
message(STATUS "Configured benchmark: adaptive_heap_benchmark")

add_executable(mpsc_queue_benchmark performance/mpsc_queue_benchmark.cpp)
target_link_libraries(mpsc_queue_benchmark PRIVATE protoCore)
message(STATUS "Configured benchmark: mpsc_queue_benchmark")

add_executable(microbenchmark_final performance/microbenchmark_final.cpp)
target_link_libraries(microbenchmark_final PRIVATE protoCore)
message(STATUS "Configured benchmark: microbenchmark_final")

# P4: the embedder conformance suite.  Always built -- it is how protoCore's own
# test suite runs the self-check matrix, and three of the five runtimes reach
# protoCore through a developer fallback that finds libraries in this build tree
# rather than through an installed package.  Installed only at top level (see
# conformance/CMakeLists.txt).
add_subdirectory(conformance)

# Warnings as errors for protoCore's own targets on MSVC, so a truncation or
# format warning fixed once cannot come back unnoticed.  Off by default (a new
# compiler release must not break a user's build); the Windows CI jobs turn it
# on.  GoogleTest, fetched by the test suite, is not affected.
option(PROTOCORE_MSVC_WARNINGS_AS_ERRORS "Build protoCore's own targets with /WX (MSVC only)" OFF)
function(protocore_msvc_werror)
    if(MSVC AND PROTOCORE_MSVC_WARNINGS_AS_ERRORS)
        foreach(_t IN LISTS ARGN)
            target_compile_options(${_t} PRIVATE /WX)
        endforeach()
    endif()
endfunction()

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

# The Visual C++ runtime (vcruntime140.dll, msvcp140.dll, ...) that
# protoCore-3.dll links against, installed app-local next to it in bin/ so the
# ZIP and the NSIS installer work on a machine without the Visual C++
# Redistributable.  App-local rather than chaining vc_redist.x64.exe: a ZIP
# cannot run an installer, and Microsoft supports app-local deployment of these
# DLLs.  The Universal CRT is not shipped; it is part of Windows 10 and later.
# The Windows CI job checks that the packaged ZIP carries these DLLs.
if(WIN32)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION "${CMAKE_INSTALL_BINDIR}")
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_COMPONENT ${PROTOCORE_INSTALL_COMPONENT})
    include(InstallRequiredSystemLibraries)
endif()

# --- CMake package configuration -------------------------------------------
# Without these rules the EXPORT keyword above names an export set that is
# never written out, so no protoCoreConfig.cmake exists and no consumer can
# check protoCore's version or ABI before linking against it.
include(CMakePackageConfigHelpers)

set(PROTOCORE_CMAKE_CONFIG_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/protoCore")

install(EXPORT protoCoreTargets
    FILE protoCoreTargets.cmake
    NAMESPACE protoCore::
    DESTINATION "${PROTOCORE_CMAKE_CONFIG_INSTALL_DIR}"
    COMPONENT ${PROTOCORE_INSTALL_COMPONENT}
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/protoCoreConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/protoCoreConfig.cmake"
    INSTALL_DESTINATION "${PROTOCORE_CMAKE_CONFIG_INSTALL_DIR}"
    PATH_VARS CMAKE_INSTALL_INCLUDEDIR CMAKE_INSTALL_LIBDIR CMAKE_INSTALL_BINDIR
)

# SameMajorVersion: find_package(protoCore 2.0) accepts any 2.x and rejects
# 1.x and 3.x, matching the rule that protoCore's major version and its
# soname move together. A consumer that needs a feature added in a minor
# release asks for that minor version instead (protoScala asks for 2.1,
# because its actor mailbox needs ProtoMPSCQueue).
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/protoCoreConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/protoCoreConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/protoCoreConfigVersion.cmake"
    DESTINATION "${PROTOCORE_CMAKE_CONFIG_INSTALL_DIR}"
    COMPONENT ${PROTOCORE_INSTALL_COMPONENT}
)

# pkg-config metadata, for consumers that are not CMake projects.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/protoCore.pc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/protoCore.pc"
    @ONLY
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/protoCore.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
    COMPONENT ${PROTOCORE_INSTALL_COMPONENT}
)

# 7. Packaging configuration (CPack)
set(CPACK_PACKAGE_NAME "protoCore")
# Pinned explicitly rather than left to each generator's default casing. The
# DEB generator lower-cases CPACK_PACKAGE_NAME and the RPM generator does not;
# four downstream repositories and protoJS's preinst script depend on the
# resulting names, so they are stated here instead of being inferred.
set(CPACK_DEBIAN_PACKAGE_NAME "protocore")
set(CPACK_RPM_PACKAGE_NAME    "protoCore")

# Ship a DEBIAN/shlibs file, so a consumer's ABI dependency can be derived from
# the binary instead of hand-written.
#
# This closes a hole that was measured, not theorised. The five runtimes declare
# a Depends version RANGE on protocore, and a version range is NOT an ABI check:
# a decoy protoCore 2.1.0 was accepted by dpkg for all five, and every binary
# then died at startup with
#     libprotoCore.so.3: cannot open shared object file
# because SOVERSION went 2 -> 3 in protoCore 2.2.0 while their find_package
# floors are 2.0 and 2.1. The RPM generator never had this hole, since rpm
# derives `Requires: libprotoCore.so.3()(64bit)` from the library itself.
#
# The five runtimes set CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON to get the same
# treatment, but that alone produced nothing: dpkg-shlibdeps resolves the
# SONAME to this package and then reads its shlibs/symbols control file, which
# did not exist, and CPack passes --ignore-missing-info, so the dependency was
# dropped SILENTLY rather than failing the build. The missing piece was here, in
# the producer, not in the consumers.
#
# The policy is ">=" rather than CPack's default "=". The default would pin a
# consumer to the exact protoCore version it was built against, so a 2.5.1
# bugfix release would break every installed runtime. ">=" keeps the SONAME as
# the real compatibility statement, which is what SOVERSION is for.
set(CPACK_DEBIAN_PACKAGE_GENERATE_SHLIBS ON)
set(CPACK_DEBIAN_PACKAGE_GENERATE_SHLIBS_POLICY ">=")
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
    # NSIS only where makensis exists, so cpack does not fail without it.
    set(CPACK_GENERATOR "ZIP")
    set(_protocore_pf86 "ProgramFiles(x86)")
    find_program(MAKENSIS_EXECUTABLE makensis
        PATHS "$ENV{ProgramFiles}/NSIS" "$ENV{${_protocore_pf86}}/NSIS")
    if(MAKENSIS_EXECUTABLE)
        list(APPEND CPACK_GENERATOR "NSIS")
        message(STATUS "CPack: NSIS generator enabled (${MAKENSIS_EXECUTABLE})")
    else()
        message(STATUS "CPack: NSIS generator disabled (makensis not found)")
    endif()
    # A Windows DLL carries no soname, so a dependent installer has nothing on
    # disk to inspect. Record the version and the ABI in the registry instead:
    # protoJS's WiX template reads exactly these two values
    # (protoJS/packaging/templates/windows/protoJS.wxs.template).
    # The backslash is escaped twice: once for this file and once for
    # CPackConfig.cmake, which CPack parses again.
    # The cross-platform CI workflow builds the ZIP and the NSIS installer.
    set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS "
        WriteRegStr HKLM 'SOFTWARE\\\\protoCore' 'Version' '${PROJECT_VERSION}'
        WriteRegStr HKLM 'SOFTWARE\\\\protoCore' 'Soversion' '${PROTOCORE_ABI_SOVERSION}'
        WriteRegStr HKLM 'SOFTWARE\\\\protoCore' 'InstallDir' '$INSTDIR'
    ")
    set(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS "
        DeleteRegValue HKLM 'SOFTWARE\\\\protoCore' 'Version'
        DeleteRegValue HKLM 'SOFTWARE\\\\protoCore' 'Soversion'
        DeleteRegValue HKLM 'SOFTWARE\\\\protoCore' 'InstallDir'
        DeleteRegKey /ifempty HKLM 'SOFTWARE\\\\protoCore'
    ")
elseif(APPLE)
    # macOS TGZ + drag-and-drop .dmg.
    # UNVERIFIED: configured and reviewed, never built - no macOS host (D-I6).
    set(CPACK_GENERATOR "TGZ;DragNDrop")
else()
    set(CPACK_GENERATOR "TGZ")
    find_program(DPKG_EXECUTABLE dpkg)
    find_program(RPMBUILD_EXECUTABLE rpmbuild)
    if(DPKG_EXECUTABLE)
        list(APPEND CPACK_GENERATOR "DEB")
        message(STATUS "CPack: DEB generator enabled (dpkg found)")
    else()
        message(STATUS "CPack: DEB generator disabled (dpkg not found)")
    endif()
    if(RPMBUILD_EXECUTABLE)
        list(APPEND CPACK_GENERATOR "RPM")
        message(STATUS "CPack: RPM generator enabled (rpmbuild found)")
    else()
        message(STATUS "CPack: RPM generator disabled (rpmbuild not found)")
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

add_executable(cache_pressure_benchmark performance/cache_pressure_benchmark.cpp)
target_link_libraries(cache_pressure_benchmark PRIVATE protoCore)
message(STATUS "Configured benchmark: cache_pressure_benchmark")

add_executable(hash_quality_benchmark performance/hash_quality_benchmark.cpp)
target_link_libraries(hash_quality_benchmark PRIVATE protoCore)

# Every target defined in this directory and in conformance/ is protoCore's own;
# test/ applies the same to proto_tests itself.
get_property(_protocore_own_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
get_property(_protocore_conformance_targets DIRECTORY conformance PROPERTY BUILDSYSTEM_TARGETS)
foreach(_t IN LISTS _protocore_own_targets _protocore_conformance_targets)
    get_target_property(_type ${_t} TYPE)
    if(NOT _type STREQUAL "UTILITY")
        protocore_msvc_werror(${_t})
    endif()
endforeach()
