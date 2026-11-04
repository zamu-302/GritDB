add_executable(gritdb_bench
memtable_bench.cpp
)
target_link_libraries(gritdb_bench PRIVATE gritdb benchmark::benchmark_main)
