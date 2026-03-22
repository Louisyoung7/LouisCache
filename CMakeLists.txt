cmake_minimum_required(VERSION 3.21)
project(LouisCache LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(src)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# 添加测试构建选项，默认关闭
option(BUILD_TEST "Build benchmark" OFF)

if(BUILD_TEST) 
  add_subdirectory(tests)
endif()