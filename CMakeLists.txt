cmake_minimum_required(VERSION 3.28)
project(LouisCache LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 单元测试构建选项，默认关闭；开启时 Google Test 由 Conan 提供
option(BUILD_TEST "Build unit tests with Google Test" OFF)
if(BUILD_TEST)
    # 必须先于各子目录 enable_testing，子目录里的 gtest_discover_tests 才会注册用例
    enable_testing()
endif()

add_subdirectory(src)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# 语义缓存可选模块，默认关闭；核心库不依赖它
option(LOUISCACHE_WITH_SEMANTIC "Build optional semantic cache module" OFF)
if(LOUISCACHE_WITH_SEMANTIC)
    add_subdirectory(modules/semantic)
endif()

if(BUILD_TEST)
    add_subdirectory(tests)
endif()
