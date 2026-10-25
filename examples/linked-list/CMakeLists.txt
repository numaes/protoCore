# The ProtoLinkedList examples of docs/tutorials/linked-list/.  Each one checks
# what it shows and runs as a test (example.linked_list.*).
foreach(_example
        01_front_built_sequences)
    add_executable(protocore_example_ll_${_example} ${_example}.cpp)
    target_link_libraries(protocore_example_ll_${_example} PRIVATE protoCore)
    target_include_directories(protocore_example_ll_${_example} PRIVATE ${PROJECT_SOURCE_DIR}/headers)
    add_test(NAME example.linked_list.${_example} COMMAND protocore_example_ll_${_example})
    set_tests_properties(example.linked_list.${_example} PROPERTIES
        TIMEOUT 120 PASS_REGULAR_EXPRESSION "${_example}: PASS" FAIL_REGULAR_EXPRESSION "FAIL")
endforeach()
