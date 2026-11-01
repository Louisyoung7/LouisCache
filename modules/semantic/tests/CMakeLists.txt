find_package(GTest CONFIG REQUIRED)

add_executable(semantic_tests
    Embedder_test.cc
    SemanticCache_test.cc
    VectorIndex_test.cc
)

target_link_libraries(semantic_tests
    LouisCacheSemantic
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(semantic_tests TEST_PREFIX "semantic.")
