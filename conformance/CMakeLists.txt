# The embedder conformance suite (P4).
#
# Built INSIDE protoCore so its cases can include headers/proto_internal.h,
# which is not installed (the package ships headers/protoCore.h alone).  The
# public header uses only public types, so a consumer never needs the internal
# one.
#
# Framework-free on purpose: protoCore's GoogleTest is a FetchContent download
# with INSTALL_GTEST OFF, so it is not available to a consumer of the installed
# package -- and two runtimes in this family use Catch2, so imposing one
# framework was never an option.  Cases return results as data and the
# embedder's own framework does the asserting.
add_library(protoCoreConformance STATIC
    Runner.cpp
    CycleDriver.cpp
    CaseGC.cpp
    CaseThreads.cpp
    CaseHeap.cpp
    CaseExternal.cpp
    CaseSymbols.cpp
    CaseModules.cpp)

target_include_directories(protoCoreConformance
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../headers>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(protoCoreConformance PUBLIC protoCore)
set_target_properties(protoCoreConformance PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(PROTOCORE_GC_REINCLUDE_SURVIVORS)
    # The cases must know whether the submission path they audit exists in this
    # build; rule 1 is vacuous without it and reports NotApplicable rather than
    # passing.
    target_compile_definitions(protoCoreConformance PUBLIC PROTOCORE_GC_REINCLUDE_SURVIVORS)
endif()

add_library(protoCore::conformance ALIAS protoCoreConformance)

# protoCore's own reference isolate binary.  Each embedder builds its own from
# its own Host with PROTOCORE_CONFORMANCE_ISOLATE_MAIN; this one is the
# reference, and it is what proves the aborting case can be observed from
# outside by exit status.
add_executable(protocore-conformance-isolate main.cpp)
target_link_libraries(protocore-conformance-isolate PRIVATE protoCoreConformance)

# Installed only when protoCore is the top-level project -- protoPython consumes
# protoCore by add_subdirectory and owns its own packaging.  Note that the
# LIBRARY itself is built unconditionally: every consumer that reaches protoCore
# through a developer fallback (find_library against ../protoCore/build_release)
# picks the archive out of the build tree, which is the only mode three of the
# five runtimes have.
if(CMAKE_PROJECT_NAME STREQUAL "protoCore")
    install(TARGETS protoCoreConformance protocore-conformance-isolate
        EXPORT protoCoreTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/../headers/protoCoreConformance.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../headers/protoCoreConformanceGTest.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/../headers/protoCoreConformanceCatch2.h"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()
