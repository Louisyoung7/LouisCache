cmake_minimum_required(VERSION 3.21)
project(LouisCache LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(src)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# 单元测试构建选项，默认关闭；开启时 Google Test 由 Conan 提供
option(BUILD_TEST "Build unit tests with Google Test" OFF)
if(BUILD_TEST)
    enable_testing()
    add_subdirectory(tests)
endif()
