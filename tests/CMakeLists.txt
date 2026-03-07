file(GLOB TESTS *.cc)

add_executable(tests ${TESTS})

target_include_directories(tests PUBLIC ${PROJECT_SOURCE_DIR}/src)

target_link_libraries(LouisCacheLib)