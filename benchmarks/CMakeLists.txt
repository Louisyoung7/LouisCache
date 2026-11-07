find_package(benchmark CONFIG REQUIRED)
find_package(Threads REQUIRED)

file(GLOB_RECURSE BENCHS "bench_*.cc")

add_executable(benchs ${BENCHS})

target_include_directories(benchs PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(benchs
    LouisCacheLib
    benchmark::benchmark_main
    Threads::Threads
)

# 命中率研究工具：不计时、只统计，不依赖 Google Benchmark。
# 文件名不匹配上面的 bench_*.cc glob，因此不会被并进压测程序。
add_executable(hitrate_study hitrate_study.cc)

target_include_directories(hitrate_study PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(hitrate_study
    LouisCacheLib
    Threads::Threads
)
