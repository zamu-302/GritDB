add_executable(table_bench table_bench.cpp)
target_include_directories(table_bench PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(table_bench PRIVATE gritdb benchmark::benchmark gtest)
