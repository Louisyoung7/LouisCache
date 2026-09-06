find_package(GTest CONFIG REQUIRED)

file(GLOB TESTS *.cc)

add_executable(unit_tests ${TESTS})

target_include_directories(unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(unit_tests 
    LouisCacheLib 
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(unit_tests)