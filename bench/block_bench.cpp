#include <benchmark/benchmark.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <random>
#include <string>
#include <vector>

#include "block.h"
#include "block_builder.h"
#include "block_iterator.h"
#include "dbformat.h"

using namespace gritdb;

// ---- Allocation counting: replace global operator new in THIS binary ----
static std::atomic<uint64_t> g_allocs{0};
void *operator new(std::size_t n) {
  g_allocs.fetch_add(1, std::memory_order_relaxed);
  if (void *p = std::malloc(n ? n : 1))
    return p;
  throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }

namespace {

// 16 bytes: "user:" + 11 digits. Stride 7 so neighbours differ in the last
// digit(s).
std::string MakeKey(uint64_t id) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "user:%011llu",
                static_cast<unsigned long long>(id));
  return buf;
}

struct Data {
  std::vector<std::string> keys;
  std::string bytes; // the finished block (kept for its size)
  std::unique_ptr<Block> block;
};

Data MakeData(size_t interval, size_t entries, size_t value_size) {
  Data d;
  const std::string value(value_size, 'v');
  BlockBuilder b(interval);
  for (size_t i = 0; i < entries; ++i) {
    d.keys.push_back(MakeKey(i * 7));
    b.Add(d.keys.back(), value);
  }
  d.bytes = std::string(b.Finish());
  if (!Block::Open(d.bytes, &d.block).ok())
    std::abort(); // Open copies its argument
  return d;
}

} // namespace

// Seek to existing keys in random order, ONE iterator reused.
static void BM_Seek(benchmark::State &state) {
  const size_t interval = state.range(0), entries = state.range(1);
  Data d = MakeData(interval, entries, 100);

  std::vector<std::string> targets = d.keys; // shuffled outside the timed loop
  std::mt19937 rng(42);
  std::shuffle(targets.begin(), targets.end(), rng);

  BlockIterator it(d.block.get(), CompareByteWise);
  for (const auto &t : targets) { // warm-up, and a correctness check once
    it.Seek(t);
    if (!it.Valid() || it.get_key() != t) {
      state.SkipWithError("seek missed an existing key");
      return;
    }
  }

  const uint64_t before = g_allocs.load();
  size_t idx = 0;
  for (auto _ : state) {
    it.Seek(targets[idx]);
    benchmark::DoNotOptimize(it.get_key().data());
    if (++idx == targets.size())
      idx = 0;
  }
  state.counters["allocs/op"] = static_cast<double>(g_allocs.load() - before) /
                                static_cast<double>(state.iterations());
  state.counters["block_bytes"] = static_cast<double>(d.bytes.size());
}

// Same, but a NEW iterator every time: shows what reuse saves.
static void BM_SeekFreshIterator(benchmark::State &state) {
  Data d = MakeData(state.range(0), state.range(1), 100);
  std::vector<std::string> targets = d.keys;
  std::mt19937 rng(42);
  std::shuffle(targets.begin(), targets.end(), rng);

  const uint64_t before = g_allocs.load();
  size_t idx = 0;
  for (auto _ : state) {
    BlockIterator it(d.block.get(), CompareByteWise);
    it.Seek(targets[idx]);
    benchmark::DoNotOptimize(it.get_key().data());
    if (++idx == targets.size())
      idx = 0;
  }
  state.counters["allocs/op"] = static_cast<double>(g_allocs.load() - before) /
                                static_cast<double>(state.iterations());
}

// Full forward scan: the cost of ParseNextKey alone.
static void BM_Next(benchmark::State &state) {
  Data d = MakeData(state.range(0), state.range(1), 100);
  BlockIterator it(d.block.get(), CompareByteWise);
  for (auto _ : state) {
    size_t n = 0;
    for (it.SeekToFirst(); it.Valid(); it.Next()) {
      benchmark::DoNotOptimize(it.get_value().data());
      ++n;
    }
    benchmark::DoNotOptimize(n);
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                          state.range(1));
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(d.bytes.size()));
}

// Build a block: builder speed, plus the size trade-off.
static void BM_BuildAndSize(benchmark::State &state) {
  const size_t interval = state.range(0), entries = state.range(1),
               value_size = state.range(2);
  std::vector<std::string> keys;
  for (size_t i = 0; i < entries; ++i)
    keys.push_back(MakeKey(i * 7));
  const std::string value(value_size, 'v');

  size_t bytes = 0;
  for (auto _ : state) {
    BlockBuilder b(interval);
    for (const auto &k : keys)
      b.Add(k, value);
    bytes = b.Finish().size();
    benchmark::DoNotOptimize(bytes);
  }
  state.counters["bytes/entry"] =
      static_cast<double>(bytes) / static_cast<double>(entries);
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) *
                          static_cast<int64_t>(entries));
}

BENCHMARK(BM_Seek)->ArgsProduct({{1, 2, 4, 8, 16, 32, 64, 128}, {35, 600}});
BENCHMARK(BM_SeekFreshIterator)->ArgsProduct({{1, 16, 128}, {35}});
BENCHMARK(BM_Next)->ArgsProduct({{1, 2, 4, 8, 16, 32, 64, 128}, {35, 600}});
BENCHMARK(BM_BuildAndSize)
    ->ArgsProduct({{1, 2, 4, 8, 16, 32, 64, 128}, {600}, {100, 8}});
