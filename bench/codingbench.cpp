#include <benchmark/benchmark.h>

#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "coding.h" // match your layout, e.g. "minilsm/coding.h"

namespace {

// A mix of small and large numbers, since real lengths are mostly small.
// small values: lengths < 128, like real key/value sizes
std::vector<uint64_t> MakeValues() {
  std::mt19937_64 rng(42);
  std::vector<uint64_t> v(1024);
  for (auto &x : v)
    x = rng() % 128;
  return v;
}

} // namespace

static void BM_PutVarint64(benchmark::State &state) {
  const auto values = MakeValues();
  std::string buf;
  for (auto _ : state) {
    buf.clear();
    for (uint64_t v : values)
      gritdb::PutVarint64(&buf, v);
    benchmark::DoNotOptimize(buf.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() *
                          static_cast<int64_t>(values.size()));
}
BENCHMARK(BM_PutVarint64);

static void BM_GetVarint64(benchmark::State &state) {
  const auto values = MakeValues();
  std::string encoded;
  for (uint64_t v : values)
    gritdb::PutVarint64(&encoded, v);

  for (auto _ : state) {
    std::string_view in(encoded);
    uint64_t out = 0, sum = 0;
    while (!in.empty()) {
      if (!gritdb::GetVarint64(&in, &out)) {
        state.SkipWithError("decode failed");
        return;
      }
      sum += out;
    }
    benchmark::DoNotOptimize(sum);
  }
  state.SetItemsProcessed(state.iterations() *
                          static_cast<int64_t>(values.size()));
}
BENCHMARK(BM_GetVarint64);
