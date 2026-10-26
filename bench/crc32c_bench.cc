#include <benchmark/benchmark.h>

#include <random>
#include <string>

#include "../include/crc32c.h"

static void BM_Crc32c_4KB(benchmark::State &state) {
  std::mt19937 rng(42);
  std::string data(4096, '\0');
  for (auto &c : data)
    c = static_cast<char>(rng());

  for (auto _ : state) {
    uint32_t crc = gritdb::crc32c::Value(data.data(), data.size());
    benchmark::DoNotOptimize(crc);
  }
  state.SetBytesProcessed(state.iterations() *
                          static_cast<int64_t>(data.size()));
}
BENCHMARK(BM_Crc32c_4KB);
