add_executable(gritdb_bench
  codingbench.cpp
  crc32c_bench.cc
)
target_link_libraries(gritdb_bench PRIVATE gritdb benchmark::benchmark_main)
