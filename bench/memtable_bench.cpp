#include "mem_filesystem.h"
#include "memtable.h"
#include "wal.h"
#include <atomic>
#include <benchmark/benchmark.h>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <new>
#include <random>
#include <unistd.h>

using namespace gritdb;
// count every heap allocation made.
static std::atomic<int> allocation_count{0};
void *operator new(size_t size) {
  allocation_count.fetch_add(1, std::memory_order_relaxed);
  return std::malloc(size);
}

void operator delete(void *ptr) noexcept { std::free(ptr); }
void operator delete(void *ptr, size_t n) noexcept { std::free(ptr); }

static size_t ResidentBytes() {
  std::ifstream f("/proc/self/statm");
  size_t size = 0, resident = 0;
  f >> size >> resident;
  return resident * static_cast<size_t>(sysconf(_SC_PAGESIZE));
}

static void FillAndReport(benchmark::State &state,
                          const std::vector<std::string> *keys) {
  const int64_t n = state.range(0);
  const std::string value(100, 'v');
  for (auto _ : state) {
    MemTable m;
    const size_t rss_before = ResidentBytes();
    const uint64_t allocs_before = allocation_count.load();
    for (int64_t i = 0; i < n; ++i) {
      if (keys) {
        m.Add(static_cast<uint64_t>(i) + 1, (*keys)[i], ValueType::Value,
              value);
      } else {
        char key[32];
        std::snprintf(key, sizeof key, "%016lld", static_cast<long long>(i));
        m.Add(static_cast<uint64_t>(i) + 1, std::string_view(key, 16),
              ValueType::Value, value);
      }
    }
    const uint64_t allocs = allocation_count.load() - allocs_before;
    const size_t rss_after = ResidentBytes();
    state.counters["allocs/op"] = static_cast<double>(allocs) / n;
    state.counters["rss_B/entry"] =
        static_cast<double>(rss_after - rss_before) / n;
    state.counters["estimate_B/entry"] =
        static_cast<double>(m.ApproximateMemoryUsage()) / n;
    benchmark::DoNotOptimize(m);
  }
  state.SetItemsProcessed(state.iterations() * n);
}
static void BM_MemTableFillSeq(benchmark::State &state) {
  FillAndReport(state, nullptr);
}
BENCHMARK(BM_MemTableFillSeq)->Arg(1'000'000)->Unit(benchmark::kMillisecond);
static void BM_MemTableFillRandom(benchmark::State &state) {
  std::mt19937_64 rng(42);
  std::vector<std::string> keys(state.range(0));
  for (auto &k : keys) {
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx",
                  static_cast<unsigned long long>(rng()));
    k.assign(buf, 16);
  }
  FillAndReport(state, &keys);
}
BENCHMARK(BM_MemTableFillRandom)->Arg(1'000'000)->Unit(benchmark::kMillisecond);

static void BM_WalAppend(benchmark::State &state) {
  const std::string payload(116, 'p');
  const int64_t records = 100'000;
  for (auto _ : state) {
    MemFileSystem fs;
    std::unique_ptr<WritableFile> f;
    (void)fs.NewWritableFile("wal.log", &f);
    LogWriter w(std::move(f));
    for (int64_t i = 0; i < records; ++i)
      (void)w.AddRecord(payload);
    (void)w.Close();
  }
  state.SetBytesProcessed(state.iterations() * records * (116 + 7));
}
BENCHMARK(BM_WalAppend)->Unit(benchmark::kMillisecond);
