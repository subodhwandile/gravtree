add_executable(gravtree_tests
    test_bloom.cpp
    test_node.cpp
    test_flush.cpp
    test_tree.cpp
)

target_link_libraries(gravtree_tests
    gravtree_lib
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(gravtree_tests)
