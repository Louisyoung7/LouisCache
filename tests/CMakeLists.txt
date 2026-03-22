file(GLOB TESTS *.cc)

add_executable(benchmark ${TESTS})

target_include_directories(benchmark PRIVATE ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(benchmark LouisCacheLib)