#include <benchmark/benchmark.h>
#include <cstdint>
#include <iostream>
#include <memory>

#include "filesystem.h"
#include "mem_filesystem.h"
#include "posix_filesystem.h"
#include <filesystem>

using namespace gritdb;
namespace {

struct MemBackend {
  static std::unique_ptr<FileSystem> Make() {
    return std::make_unique<MemFileSystem>();
  }
  static std::string MakeBase() { return "/bench"; }
  static void Cleanup(const std::string &) {}
};

struct PosixBackend {
  static std::unique_ptr<FileSystem> Make() {
    return std::make_unique<PosixFileSystem>();
  }
  static std::string MakeBase() {
    std::filesystem::path root = std::filesystem::temp_directory_path();
    if (const char *env = std::getenv("GRITDB_BENCH_DIR"))
      root = env;
    std::string tmpl = (root / "gritdb_bench_XXXXXX").string();
    char *made = ::mkdtemp(tmpl.data());
    return made != nullptr ? std::string(made) : std::string();
  }
  static void Cleanup(const std::string &base) {
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
  }
};

template <typename Backend> struct Env {
  Env() : fs(Backend::Make()), base(Backend::MakeBase()) {

    std::cerr << "Benchmark base: [" << base << "]\n";
    ok = !base.empty() && fs->CreateDir(base).ok();
  }
  ~Env() { Backend::Cleanup(base); }

  std::unique_ptr<FileSystem> fs;
  std::string base;
  bool ok = false;
};

constexpr size_t kRecord = 4096;

} // namespace
namespace {
template <typename Backend>
static void BM_AppendNoSync(benchmark::State &state) {
  Env<Backend> env;
  std::unique_ptr<WritableFile> f;
  if (!env.ok || !env.fs->NewWritableFile(env.base + "/data", &f).ok()) {
    state.SkipWithError("setup failed");
    return;
  }
  const std::string payload(kRecord, 'x');
  for (auto _ : state) {
    if (!f->Append(payload).ok()) {
      state.SkipWithError("append failed");
      break;
    }
  }
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(kRecord));
}
template <typename Backend> void BM_AppendThenSync(benchmark::State &state) {
  Env<Backend> env;
  std::unique_ptr<WritableFile> f;
  if (!env.ok || !env.fs->NewWritableFile(env.base + "/data", &f).ok()) {
    state.SkipWithError("setup failed");
    return;
  }
  const std::string payload(kRecord, 'x');
  for (auto _ : state) {
    if (!f->Append(payload).ok() || !f->Sync().ok()) {
      state.SkipWithError("append/sync failed");
      break;
    }
  }
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(kRecord));
}

template <typename Backend> void BM_SyncOnly(benchmark::State &state) {
  Env<Backend> env;
  std::unique_ptr<WritableFile> f;
  if (!env.ok || !env.fs->NewWritableFile(env.base + "/data", &f).ok()) {
    state.SkipWithError("setup failed");
    return;
  }
  const std::string payload(kRecord, 'x');

  for (auto _ : state) {
    if (!f->Append(payload).ok()) {
      state.SkipWithError("append failed");
      break;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const Status s = f->Sync();
    const auto t1 = std::chrono::steady_clock::now();
    if (!s.ok()) {
      state.SkipWithError("sync failed");
      break;
    }
    state.SetIterationTime(std::chrono::duration<double>(t1 - t0).count());
  }
}
BENCHMARK_TEMPLATE(BM_AppendNoSync, MemBackend)
    ->Iterations(50000)
    ->Unit(benchmark::kMicrosecond);
BENCHMARK_TEMPLATE(BM_AppendNoSync, PosixBackend)
    ->Iterations(50000)
    ->Unit(benchmark::kMicrosecond);
BENCHMARK_TEMPLATE(BM_AppendThenSync, MemBackend)
    ->Iterations(2000)
    ->Unit(benchmark::kMicrosecond);
BENCHMARK_TEMPLATE(BM_AppendThenSync, PosixBackend)
    ->Iterations(2000)
    ->Unit(benchmark::kMicrosecond);
BENCHMARK_TEMPLATE(BM_SyncOnly, MemBackend)
    ->Iterations(2000)
    ->UseManualTime()
    ->Unit(benchmark::kMicrosecond);
BENCHMARK_TEMPLATE(BM_SyncOnly, PosixBackend)
    ->Iterations(2000)
    ->UseManualTime()
    ->Unit(benchmark::kMicrosecond);
} // namespace
