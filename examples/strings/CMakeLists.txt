# The string examples of docs/tutorials/strings/.  Each one checks what it
# shows and runs as a test (example.strings.*).
foreach(_example
        01_attribute_names)
    add_executable(protocore_example_${_example} ${_example}.cpp)
    target_link_libraries(protocore_example_${_example} PRIVATE protoCore)
    target_include_directories(protocore_example_${_example} PRIVATE ${PROJECT_SOURCE_DIR}/headers)
    add_test(NAME example.strings.${_example} COMMAND protocore_example_${_example})
    set_tests_properties(example.strings.${_example} PROPERTIES
        TIMEOUT 120 PASS_REGULAR_EXPRESSION "${_example}: PASS" FAIL_REGULAR_EXPRESSION "FAIL")
endforeach()
