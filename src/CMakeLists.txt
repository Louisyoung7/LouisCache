file(GLOB_RECURSE SOURCES *.cc)

add_library(LouisCacheLib STATIC ${SOURCES})

target_include_directories(LouisCacheLib PUBLIC ${PROJECT_SOURCE_DIR}/src)

