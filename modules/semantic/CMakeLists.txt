add_library(LouisCacheSemantic INTERFACE)

target_include_directories(LouisCacheSemantic INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(LouisCacheSemantic INTERFACE LouisCacheLib)

# 模块自带测试，仅在开启模块且开启单元测试时构建
if(BUILD_TEST)
    add_subdirectory(tests)
endif()
