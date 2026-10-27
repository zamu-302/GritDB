cmake_minimum_required(VERSION 3.20)

project(GritDB LANGUAGES CXX)


set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)




add_library(gritdb
src/status.cc
src/crc32c.cc
src/filesystem.cc
src/mem_filesystem.cc)
target_include_directories(gritdb PUBLIC include)
target_compile_options(gritdb PRIVATE -Wall -Wextra -Wpedantic)


include(FetchContent)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)

FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

FetchContent_Declare(benchmark
URL https://github.com/google/benchmark/archive/refs/tags/v1.8.3.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

FetchContent_MakeAvailable(benchmark googletest)

enable_testing()
add_subdirectory(bench)
add_subdirectory(tests)

