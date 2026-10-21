# The exception examples of docs/tutorials/exceptions/.  Each one checks what
# it shows and runs as a test (example.exceptions.*).
#
# The geometry module is its own shared library, so the examples also show
# that a ProtoException thrown in one library is caught in another: the type
# has one identity because its destructor, the key function, is defined in
# libprotoCore.
add_library(protocore_example_geometry SHARED geometry_module/GeometryModule.cpp)
target_link_libraries(protocore_example_geometry PRIVATE protoCore)
target_include_directories(protocore_example_geometry PRIVATE ${PROJECT_SOURCE_DIR}/headers)

foreach(_example
        01_catch_by_prototype
        02_error_factory
        03_unwinding_and_lifetime
        04_foreign_cpp_exceptions)
    add_executable(protocore_example_${_example} ${_example}.cpp)
    target_link_libraries(protocore_example_${_example} PRIVATE protoCore protocore_example_geometry)
    target_include_directories(protocore_example_${_example} PRIVATE
        ${PROJECT_SOURCE_DIR}/headers ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME example.exceptions.${_example} COMMAND protocore_example_${_example})
    set_tests_properties(example.exceptions.${_example} PROPERTIES
        TIMEOUT 120 PASS_REGULAR_EXPRESSION "${_example}: PASS" FAIL_REGULAR_EXPRESSION "FAIL")
endforeach()
