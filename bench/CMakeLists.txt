add_executable(gritdb_bench
  fsync_bench.cpp
)
target_link_libraries(gritdb_bench PRIVATE gritdb benchmark::benchmark_main)
