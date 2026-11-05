find_package(benchmark CONFIG REQUIRED)

file(GLOB_RECURSE BENCHS "bench_*.cc")

add_executable(benchs ${BENCHS})

target_include_directories(benchs PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(benchs 
    LouisCacheLib 
    benchmark::benchmark_main
)
