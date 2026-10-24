cmake_minimum_required(VERSION 3.20)

project(GritDB LANGUAGES CXX)


set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)



add_library(gritdb
src/status.cc
src/crc32c.cc)
target_link_directories(gritdb PUBLIC include)
target_compile_options(gritdb PRIVATE -Wall -Wextra -Wpedantic)


include(FetchContent)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)



FetchContent_MakeAvailable(googletest)


enable_testing()
add_subdirectory(tests)

