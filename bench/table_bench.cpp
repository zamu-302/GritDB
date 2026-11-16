// Data in every benchmark: N entries, 24-byte internal keys (16-byte user key +
// 8-byte tag), 100-byte values. Same shape as the Day 4 block benchmarks so the
// numbers compose.
//
// Hypotheses (write yours down BEFORE running, then compare):
//   H1  bytes_read/op  ~= block_size + 5 (one data block + its trailer) -- the
//   index is cached. H2  reads/op       == 1 (index stays in memory; one data
//   block read per Get). H3  verify on vs off: the CRC over a 4 KB block costs
//   more than the block's binary search,
//       so verify=1 is clearly slower for a table-driven CRC (check BM_Crc4K to
//       see why).
//   H4  allocs/op is small and constant (the block copy, the Block object, the
//   iterator key
//       buffer, the output strings) and does not grow with block size.
//   H5  read_amplification ~= block_size / entry_size (~35 at 4 KB, ~500 at 64
//   KB). H6  Build throughput barely depends on block size; file size does
//   (index + trailers).
#include <benchmark/benchmark.h>

#include <atomic>
#include <cstdlib>
#include <memory>
#include <new>
#include <random>

#include "../tests/test_util.h"
#include "filesystem.h"
#include "status.h"

using namespace gritdb;
using namespace gritdb::tu;

// ---- allocation counter: replaces global operator new, so define it ONCE per
// binary --------
static std::atomic<uint64_t> g_allocs{0};
void *operator new(size_t n) {
  g_allocs.fetch_add(1, std::memory_order_relaxed);
  if (void *p = std::malloc(n ? n : 1))
    return p;
  throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, size_t) noexcept { std::free(p); }

namespace {

constexpr int kEntries = 20000;
constexpr size_t kValueLen = 100;

std::string UserKey(int i) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "key%013d", i); // exactly 16 bytes
  return buf;
}

Items MakeInternalItems(int n) {
  Items items;
  items.reserve(n);
  for (int i = 0; i < n; ++i)
    items.emplace_back(IKey(UserKey(i), 1, ValueType::Value),
                       std::string(kValueLen, 'v'));
  return items;
}

Options BenchOptions(size_t block_size, bool verify = true) {
  Options o = TestOptions(block_size, CompareInternalKey);
  o.verify_checksums = verify;
  return o;
}

uint64_t RawBytes(const Items &items) {
  uint64_t n = 0;
  for (const auto &kv : items)
    n += kv.first.size() + kv.second.size();
  return n;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// BM_TableBuild: cost of writing a table. Output goes to a sink that only
// counts bytes, so this measures the builder (block building + CRC + index),
// not the filesystem. Arg: block size.
static void BM_TableBuild(benchmark::State &state) {
  const size_t bs = state.range(0);
  Items items = MakeInternalItems(kEntries);
  Options o = BenchOptions(bs);
  uint64_t file_bytes = 0;
  for (auto _ : state) {
    CountingSink sink;
    TableBuilder b(o, &sink);
    for (const auto &kv : items)
      b.Add(kv.first, kv.second);
    Status s = b.Finish();
    benchmark::DoNotOptimize(s);
    file_bytes = sink.bytes();
  }
  state.SetItemsProcessed(state.iterations() * kEntries);
  state.SetBytesProcessed(state.iterations() * RawBytes(items));
  state.counters["file_bytes"] = static_cast<double>(file_bytes);
}
BENCHMARK(BM_TableBuild)
    ->Arg(512)
    ->Arg(1024)
    ->Arg(4096)
    ->Arg(16384)
    ->Arg(65536);

// ---------------------------------------------------------------------------------------------
// BM_TableSize: where do the bytes go? One build per iteration; the interesting
// output is the counters, not the time.
static void BM_TableSize(benchmark::State &state) {
  const size_t bs = state.range(0);
  Items items = MakeInternalItems(kEntries);
  Options o = BenchOptions(bs);
  std::string bytes = BuildTable(items, o);
  Layout L = ParseLayout(bytes);
  for (auto _ : state)
    benchmark::DoNotOptimize(bytes.data());

  const double raw = static_cast<double>(RawBytes(items));
  const double index_bytes = static_cast<double>(L.index.end - L.index.begin);
  const double trailers = 5.0 * (L.data.size() + 1);
  state.counters["file_bytes"] = static_cast<double>(bytes.size());
  state.counters["raw_bytes"] = raw;
  state.counters["data_blocks"] = static_cast<double>(L.data.size());
  state.counters["index_bytes"] = index_bytes;
  state.counters["footer_bytes"] = 40;
  state.counters["trailer_bytes"] = trailers;
  state.counters["overhead_pct"] =
      100.0 * (static_cast<double>(bytes.size()) - raw) / raw;
  state.counters["bytes_per_entry"] =
      static_cast<double>(bytes.size()) / kEntries;
}
BENCHMARK(BM_TableSize)
    ->Arg(512)
    ->Arg(1024)
    ->Arg(4096)
    ->Arg(16384)
    ->Arg(65536)
    ->Iterations(1);

// ---------------------------------------------------------------------------------------------
// BM_TableGet: point lookup of keys that exist. Args: block size, verify
// checksums (0/1). Targets are shuffled once, outside the timed loop, so the
// CPU cannot predict the access order.
static void BM_TableGet(benchmark::State &state) {
  const size_t bs = state.range(0);
  const bool verify = state.range(1) != 0;
  Items items = MakeInternalItems(kEntries);
  Options o = BenchOptions(bs, verify);
  std::string bytes = BuildTable(items, o);

  Env env;
  std::unique_ptr<RandomAccessFile> raw;
  if (!NewRandomFile(env, bytes, &raw).ok())
    return state.SkipWithError("file");
  auto counters = std::make_shared<IoCounters>();
  auto counting =
      std::make_unique<CountingRandomAccessFile>(std::move(raw), counters);
  std::unique_ptr<Table> table;
  if (!Table::Open(o, std::move(counting), bytes.size(), &table).ok())
    return state.SkipWithError("open");

  std::vector<std::string> targets;
  for (int i = 0; i < kEntries; ++i)
    targets.push_back(IKey(UserKey(i), kMaxSequence, ValueType::Value));
  std::shuffle(targets.begin(), targets.end(), std::mt19937_64(7));

  std::string k, v;
  size_t idx = 0;
  const uint64_t reads0 = counters->reads, bytes0 = counters->bytes,
                 allocs0 = g_allocs;
  for (auto _ : state) {
    Status s = table->InternalGet(targets[idx], &k, &v);
    benchmark::DoNotOptimize(s);
    benchmark::DoNotOptimize(v.data());
    if (++idx == targets.size())
      idx = 0;
  }
  const double ops = static_cast<double>(state.iterations());
  const double bytes_per_op = (counters->bytes - bytes0) / ops;
  state.counters["reads/op"] = (counters->reads - reads0) / ops;
  state.counters["bytes_read/op"] = bytes_per_op;
  state.counters["allocs/op"] = (g_allocs - allocs0) / ops;
  state.counters["read_amp"] = bytes_per_op / (24 + kValueLen);
  state.counters["file_bytes"] = static_cast<double>(bytes.size());
}
BENCHMARK(BM_TableGet)->ArgsProduct({{512, 1024, 4096, 16384, 65536}, {0, 1}});

// ---------------------------------------------------------------------------------------------
// BM_TableGetMiss: lookups that fall BETWEEN two keys (the data block is read,
// the answer is the next key), and lookups past the last key (the index alone
// says NotFound: zero data reads).
static void BM_TableGetGap(benchmark::State &state) {
  const size_t bs = state.range(0);
  Items items = MakeInternalItems(kEntries);
  Options o = BenchOptions(bs);
  std::string bytes = BuildTable(items, o);
  Env env;
  std::unique_ptr<RandomAccessFile> raw;
  if (!NewRandomFile(env, bytes, &raw).ok())
    return state.SkipWithError("file");
  auto counters = std::make_shared<IoCounters>();
  std::unique_ptr<Table> table;
  if (!Table::Open(
           o,
           std::make_unique<CountingRandomAccessFile>(std::move(raw), counters),
           bytes.size(), &table)
           .ok())
    return state.SkipWithError("open");

  std::vector<std::string> targets;
  for (int i = 0; i < kEntries; ++i)
    targets.push_back(IKey(UserKey(i) + "x", kMaxSequence,
                           ValueType::Value)); // between i and i+1
  std::shuffle(targets.begin(), targets.end(), std::mt19937_64(7));

  std::string k, v;
  size_t idx = 0;
  const uint64_t reads0 = counters->reads;
  for (auto _ : state) {
    Status s = table->InternalGet(targets[idx], &k, &v);
    benchmark::DoNotOptimize(s);
    if (++idx == targets.size())
      idx = 0;
  }
  state.counters["reads/op"] =
      (counters->reads - reads0) / static_cast<double>(state.iterations());
}
BENCHMARK(BM_TableGetGap)->Arg(1024)->Arg(4096)->Arg(65536);

static void BM_TableGetPastEnd(benchmark::State &state) {
  Items items = MakeInternalItems(kEntries);
  Options o = BenchOptions(4096);
  std::string bytes = BuildTable(items, o);
  Env env;
  std::unique_ptr<RandomAccessFile> raw;
  if (!NewRandomFile(env, bytes, &raw).ok())
    return state.SkipWithError("file");
  auto counters = std::make_shared<IoCounters>();
  std::unique_ptr<Table> table;
  if (!Table::Open(
           o,
           std::make_unique<CountingRandomAccessFile>(std::move(raw), counters),
           bytes.size(), &table)
           .ok())
    return state.SkipWithError("open");
  const std::string target =
      IKey("zzzzzzzzzzzzzzzz", kMaxSequence, ValueType::Value);
  std::string k, v;
  const uint64_t reads0 = counters->reads;
  for (auto _ : state) {
    Status s = table->InternalGet(target, &k, &v);
    benchmark::DoNotOptimize(s);
  }
  state.counters["reads/op"] =
      (counters->reads - reads0) / static_cast<double>(state.iterations());
}
BENCHMARK(BM_TableGetPastEnd);

// ---------------------------------------------------------------------------------------------
// BM_Crc4K / BM_Crc64K: the checksum alone. Compare with BM_TableGet verify=0
// vs 1 to see how much of a verified Get is just CRC.
static void BM_Crc(benchmark::State &state) {
  std::string data(state.range(0), 'x');
  for (auto _ : state) {
    uint32_t c = crc32c::Value(data.data(), data.size());
    benchmark::DoNotOptimize(c);
  }
  state.SetBytesProcessed(state.iterations() * data.size());
}
BENCHMARK(BM_Crc)->Arg(4096)->Arg(65536);

// ---------------------------------------------------------------------------------------------
// BM_TableOpen: cost of opening (footer read + index read + CRC + Block::Open).
// This is the price a table pays once, then every Get gets the index for free.
static void BM_TableOpen(benchmark::State &state) {
  const size_t bs = state.range(0);
  Items items = MakeInternalItems(kEntries);
  Options o = BenchOptions(bs);
  std::string bytes = BuildTable(items, o);

  Env env;
  std::string path = env.NextPath();
  if (!WriteFile(env, path, bytes).ok()) {
    return state.SkipWithError("write.");
  }
  for (auto _ : state) {
    std::unique_ptr<RandomAccessFile> f;
    Status s = env.fs.NewRandomAccessFile(path, &f);
    benchmark::DoNotOptimize(s);
    std::unique_ptr<Table> t;
    s = Table::Open(o, std::move(f), bytes.size(), &t);
    benchmark::DoNotOptimize(s);
    benchmark::DoNotOptimize(t);
  }
}
BENCHMARK(BM_TableOpen)->Arg(1024)->Arg(4096)->Arg(65536);

BENCHMARK_MAIN();
