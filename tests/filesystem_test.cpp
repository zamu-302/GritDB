#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "filesystem.h"
#include "mem_filesystem.h"
#include "posix_filesystem.h"
#include "status.h"

using namespace gritdb;

#define ASSERT_OK(expr)                                                        \
  do {                                                                         \
    Status _s = (expr);                                                        \
    ASSERT_TRUE(_s.ok()) << _s.ToString();                                     \
  } while (0)

// ---------------------------------------------------------------------------
// Backends: how to make each filesystem, where it works, how to clean up
// ---------------------------------------------------------------------------
namespace {

struct MemBackend {
  static std::unique_ptr<FileSystem> Make() {
    return std::make_unique<MemFileSystem>();
  }
  static std::string MakeBase() { return "/test"; } // fake path, lives in RAM
  static void Cleanup(const std::string &) {}
};

struct PosixBackend {
  static std::unique_ptr<FileSystem> Make() {
    return std::make_unique<PosixFileSystem>();
  }
  static std::string MakeBase() {
    // mkdtemp replaces the XXXXXX in the buffer and creates the directory.
    std::string tmpl =
        (std::filesystem::temp_directory_path() / "gritdb_test_XXXXXX")
            .string();
    char *made = ::mkdtemp(tmpl.data());
    return made != nullptr ? std::string(made) : std::string();
  }
  static void Cleanup(const std::string &base) {
    std::error_code ec; // never throw from TearDown
    std::filesystem::remove_all(base, ec);
  }
};

// ---------------------------------------------------------------------------
// Typed fixture: one test body runs once per backend
// ---------------------------------------------------------------------------
template <typename Backend> class FsTest : public ::testing::Test {
protected:
  void SetUp() override {
    fs_ = Backend::Make();
    base = Backend::MakeBase();
    ASSERT_FALSE(base.empty());
    ASSERT_OK(
        fs_->CreateDir(base)); // already exists on Posix (mkdtemp made it)
  }
  void TearDown() override { Backend::Cleanup(base); }

  std::string P(const std::string &name) const { return base + "/" + name; }

  std::unique_ptr<FileSystem> fs_;
  std::string base;
};

struct BackendNames {
  template <typename T> static std::string GetName(int) {
    if constexpr (std::is_same_v<T, MemBackend>)
      return "Mem";
    else
      return "Posix";
  }
};

// ---------------------------------------------------------------------------
// Helpers (take FileSystem*, so they work with any backend)
// ---------------------------------------------------------------------------
Status WriteFile(FileSystem *fs, const std::string &path,
                 std::string_view data) {
  std::unique_ptr<WritableFile> f;
  Status s = fs->NewWritableFile(path, &f);
  if (!s.ok())
    return s;
  s = f->Append(data);
  if (!s.ok())
    return s;
  s = f->Sync();
  if (!s.ok())
    return s;
  return f->Close();
}

Status ReadAll(FileSystem *fs, const std::string &path, std::string *out) {
  uint64_t size = 0;
  Status s = fs->FileSize(path, &size);
  if (!s.ok())
    return s;
  std::unique_ptr<RandomAccessFile> f;
  s = fs->NewRandomAccessFile(path, &f);
  if (!s.ok())
    return s;
  std::string scratch(size, '\0');
  std::string_view result;
  s = f->Read(&result, scratch.data(), scratch.size(), 0);
  if (!s.ok())
    return s;
  *out = std::string(result);
  return Status::OK();
}

std::string ReadAt(RandomAccessFile *f, uint64_t offset, size_t n) {
  std::string scratch(n, '\0');
  std::string_view result;
  Status s = f->Read(&result, scratch.data(), n, offset);
  EXPECT_TRUE(s.ok()) << s.ToString();
  return std::string(result);
}

// Removes a Posix temp directory even if an ASSERT_ returns early.
struct ScopedPosixDir {
  ScopedPosixDir() : path(PosixBackend::MakeBase()) {}
  ~ScopedPosixDir() { PosixBackend::Cleanup(path); }
  std::string path;
};

} // namespace

using Backends = ::testing::Types<MemBackend, PosixBackend>;
TYPED_TEST_SUITE(FsTest, Backends, BackendNames);

// A typed-test body can't see fixture members by plain name (they live in a
// dependent base class), so this pulls them in through `this->`.
#define FS_PRELUDE                                                             \
  [[maybe_unused]] FileSystem &fs = *this->fs_;                                \
  [[maybe_unused]] auto P = [this](const std::string &n) {                     \
    return this->P(n);                                                         \
  };                                                                           \
  [[maybe_unused]] const std::string &base = this->base;

// ---------------------------------------------------------------------------
// Mem-only tests: these rely on relative paths and the in-memory root
// ---------------------------------------------------------------------------
TEST(MemOnly, CreateBase) {
  MemFileSystem file;
  std::unique_ptr<WritableFile> val;
  ASSERT_OK(file.NewWritableFile("a.log", &val));
  ASSERT_OK(val->Append("hello world"));
  EXPECT_TRUE(file.FileExists("a.log"));
}

TEST(MemOnly, CreateNestedDirsThenFile) {
  MemFileSystem file;
  ASSERT_OK(file.CreateDir("math"));
  ASSERT_OK(file.CreateDir("math/C++"));
  ASSERT_OK(file.CreateDir("math/C++/Projects"));
  ASSERT_OK(file.CreateDir("math/C++/Projects/GritDB"));
  std::unique_ptr<WritableFile> val;
  ASSERT_OK(file.NewWritableFile("math/C++/Projects/GritDB/grit.db", &val));
  EXPECT_TRUE(file.FileExists("math/C++/Projects/GritDB/grit.db"));
}

// ---------------------------------------------------------------------------
// Posix-only tests: need the real disk
// ---------------------------------------------------------------------------
// If ~PosixWritableFile didn't close its descriptor, this would hit the
// per-process limit (usually 1024) and open() would fail with EMFILE.
TEST(PosixOnly, HandlesDoNotLeakDescriptors) {
  PosixFileSystem fs;
  ScopedPosixDir dir;
  ASSERT_FALSE(dir.path.empty());
  for (int i = 0; i < 5000; i++) {
    std::unique_ptr<WritableFile> w;
    Status s = fs.NewWritableFile(dir.path + "/f", &w);
    ASSERT_TRUE(s.ok()) << "iteration " << i << ": " << s.ToString();
  } // each handle is destroyed here WITHOUT Close(): the destructor is under
    // test
}

// Reads the file back with plain std::ifstream, bypassing our own read code,
// so a bug in PosixRandomAccessFile can't hide a bug in the write path.
TEST(PosixOnly, SyncedDataIsReallyInTheFile) {
  PosixFileSystem fs;
  ScopedPosixDir dir;
  ASSERT_FALSE(dir.path.empty());
  ASSERT_OK(WriteFile(&fs, dir.path + "/data", "persisted"));

  std::ifstream in(dir.path + "/data", std::ios::binary);
  std::string got((std::istreambuf_iterator<char>(in)),
                  std::istreambuf_iterator<char>());
  EXPECT_EQ(got, "persisted");
}

// ---------------------------------------------------------------------------
// Shared tests: every backend must pass every one of these
// ---------------------------------------------------------------------------

// ---- content ----

TYPED_TEST(FsTest, AppendInPiecesReadsBackExactly) {
  FS_PRELUDE;
  std::unique_ptr<WritableFile> w;
  ASSERT_OK(fs.NewWritableFile(P("a.log"), &w));
  ASSERT_OK(w->Append("hello "));
  ASSERT_OK(w->Append("wor"));
  ASSERT_OK(w->Append("ld"));
  ASSERT_OK(w->Sync());
  ASSERT_OK(w->Close());

  std::string got;
  ASSERT_OK(ReadAll(&fs, P("a.log"), &got));
  EXPECT_EQ(got, "hello world");

  uint64_t size = 0;
  ASSERT_OK(fs.FileSize(P("a.log"), &size));
  EXPECT_EQ(size, 11u);
  EXPECT_TRUE(fs.FileExists(P("a.log")));
}

TYPED_TEST(FsTest, ReadAtOffsetsAndEdges) {
  FS_PRELUDE;
  ASSERT_OK(WriteFile(&fs, P("digits"), "0123456789"));
  std::unique_ptr<RandomAccessFile> r;
  ASSERT_OK(fs.NewRandomAccessFile(P("digits"), &r));

  EXPECT_EQ(ReadAt(r.get(), 0, 4), "0123");
  EXPECT_EQ(ReadAt(r.get(), 3, 3), "345");
  EXPECT_EQ(ReadAt(r.get(), 8, 10),
            "89"); // short read: asked 10, got 2, not an error
  EXPECT_EQ(ReadAt(r.get(), 10, 5), "");  // exactly at EOF
  EXPECT_EQ(ReadAt(r.get(), 500, 5), ""); // far past EOF
  EXPECT_EQ(ReadAt(r.get(), 2, 0), "");   // zero-length read
}

TYPED_TEST(FsTest, EmptyFile) {
  FS_PRELUDE;
  ASSERT_OK(WriteFile(&fs, P("empty"), ""));
  uint64_t size = 99;
  ASSERT_OK(fs.FileSize(P("empty"), &size));
  EXPECT_EQ(size, 0u);
  std::string got = "not empty";
  ASSERT_OK(ReadAll(&fs, P("empty"), &got));
  EXPECT_EQ(got, "");
}

TYPED_TEST(FsTest, LargeFileRoundTrip) {
  FS_PRELUDE;
  const size_t kSize = 3u << 20; // 3 MB
  std::string expected;
  expected.reserve(kSize);
  for (size_t i = 0; i < kSize; i++)
    expected.push_back(static_cast<char>(i % 251));

  std::unique_ptr<WritableFile> w;
  ASSERT_OK(fs.NewWritableFile(P("big"), &w));
  for (size_t off = 0; off < expected.size(); off += 4096) {
    size_t n = std::min<size_t>(4096, expected.size() - off);
    ASSERT_OK(w->Append(std::string_view(expected).substr(off, n)));
  }
  ASSERT_OK(w->Sync());
  ASSERT_OK(w->Close());

  uint64_t size = 0;
  ASSERT_OK(fs.FileSize(P("big"), &size));
  EXPECT_EQ(size, expected.size());

  std::string got;
  ASSERT_OK(ReadAll(&fs, P("big"), &got));
  EXPECT_TRUE(got == expected); // avoids printing 3 MB if it fails
}

// ---- missing things ----

TYPED_TEST(FsTest, OpenMissingFileIsNotFound) {
  FS_PRELUDE;
  std::unique_ptr<RandomAccessFile> r;
  Status s = fs.NewRandomAccessFile(P("nope"), &r);
  EXPECT_TRUE(s.IsNotFound()) << s.ToString();
  uint64_t size = 0;
  EXPECT_TRUE(fs.FileSize(P("nope"), &size).IsNotFound());
  EXPECT_FALSE(fs.FileExists(P("nope")));
}

TYPED_TEST(FsTest, CreateFileWithMissingParentIsNotFound) {
  FS_PRELUDE;
  std::unique_ptr<WritableFile> w;
  Status s = fs.NewWritableFile(P("no/such/dir/a.log"), &w);
  EXPECT_TRUE(s.IsNotFound()) << s.ToString();
}

// ---- create / overwrite ----

// Only what BOTH backends can promise: creating an existing path leaves an
// empty file. (Posix truncates in place, Mem swaps in a fresh file, so what an
// already-open reader sees differs, and the suite does not pin that down.)
TYPED_TEST(FsTest, CreatingSamePathTwiceGivesEmptyFile) {
  FS_PRELUDE;
  ASSERT_OK(WriteFile(&fs, P("a"), "abc"));
  std::unique_ptr<WritableFile> w;
  ASSERT_OK(fs.NewWritableFile(P("a"), &w)); // must succeed and leave it empty

  uint64_t size = 99;
  ASSERT_OK(fs.FileSize(P("a"), &size));
  EXPECT_EQ(size, 0u);
}

TYPED_TEST(FsTest, CreateFileOnDirectoryNameFails) {
  FS_PRELUDE;
  ASSERT_OK(fs.CreateDir(P("d")));
  std::unique_ptr<WritableFile> w;
  EXPECT_FALSE(fs.NewWritableFile(P("d"), &w).ok());
}

// ---- rename ----

TYPED_TEST(FsTest, RenameOntoExistingFileReplacesIt) {
  FS_PRELUDE;
  ASSERT_OK(WriteFile(&fs, P("src"), "new"));
  ASSERT_OK(WriteFile(&fs, P("dst"), "old-and-longer"));
  ASSERT_OK(fs.RenameFile(P("src"), P("dst")));

  EXPECT_FALSE(fs.FileExists(P("src")));
  std::string got;
  ASSERT_OK(ReadAll(&fs, P("dst"), &got));
  EXPECT_EQ(got, "new");
}

TYPED_TEST(FsTest, RenameToNewNameMovesContent) {
  FS_PRELUDE;
  ASSERT_OK(WriteFile(&fs, P("a"), "payload"));
  ASSERT_OK(fs.RenameFile(P("a"), P("b")));
  EXPECT_FALSE(fs.FileExists(P("a")));
  std::string got;
  ASSERT_OK(ReadAll(&fs, P("b"), &got));
  EXPECT_EQ(got, "payload");
}

TYPED_TEST(FsTest, RenameMissingSourceIsNotFound) {
  FS_PRELUDE;
  Status s = fs.RenameFile(P("ghost"), P("other"));
  EXPECT_TRUE(s.IsNotFound()) << s.ToString();
  EXPECT_TRUE(
      fs.RenameFile(P("ghost"), P("ghost")).IsNotFound()); // even src == dst
}

TYPED_TEST(FsTest, RenameOntoDirectoryFailsAndKeepsSource) {
  FS_PRELUDE;
  ASSERT_OK(fs.CreateDir(P("d")));
  ASSERT_OK(WriteFile(&fs, P("f"), "x"));
  EXPECT_FALSE(fs.RenameFile(P("f"), P("d")).ok());
  EXPECT_TRUE(fs.FileExists(P("f")));
}

// ---- delete ----

TYPED_TEST(FsTest, DeleteRemovesFile) {
  FS_PRELUDE;
  ASSERT_OK(WriteFile(&fs, P("a"), "x"));
  ASSERT_OK(fs.DeleteFile(P("a")));
  EXPECT_FALSE(fs.FileExists(P("a")));
  std::unique_ptr<RandomAccessFile> r;
  EXPECT_TRUE(fs.NewRandomAccessFile(P("a"), &r).IsNotFound());
  EXPECT_TRUE(fs.DeleteFile(P("a")).IsNotFound()); // second delete
}

TYPED_TEST(FsTest, DeleteWhileReaderOpenReaderStillWorks) {
  FS_PRELUDE;
  ASSERT_OK(WriteFile(&fs, P("a"), "hello"));
  std::unique_ptr<RandomAccessFile> r;
  ASSERT_OK(fs.NewRandomAccessFile(P("a"), &r));
  ASSERT_OK(fs.DeleteFile(P("a")));
  EXPECT_FALSE(fs.FileExists(P("a")));
  EXPECT_EQ(ReadAt(r.get(), 0, 5), "hello");
}

// ---- directories ----

TYPED_TEST(FsTest, CreateDirNestedAndTwice) {
  FS_PRELUDE;
  ASSERT_OK(fs.CreateDir(P("x")));
  ASSERT_OK(fs.CreateDir(P("x/y")));
  ASSERT_OK(fs.CreateDir(P("x/y"))); // idempotent on purpose
}

TYPED_TEST(FsTest, CreateDirWithMissingParentIsNotFound) {
  FS_PRELUDE;
  Status s = fs.CreateDir(P("no/child"));
  EXPECT_TRUE(s.IsNotFound()) << s.ToString();
}

TYPED_TEST(FsTest, CreateDirOverFileFails) {
  FS_PRELUDE;
  ASSERT_OK(WriteFile(&fs, P("f"), "x"));
  EXPECT_FALSE(fs.CreateDir(P("f")).ok());
}

TYPED_TEST(FsTest, DirlistReturnsImmediateChildrenSorted) {
  FS_PRELUDE;
  ASSERT_OK(WriteFile(&fs, P("b"), "1"));
  ASSERT_OK(WriteFile(&fs, P("a"), "2"));
  ASSERT_OK(fs.CreateDir(P("d")));
  ASSERT_OK(WriteFile(&fs, P("d/inner"), "3")); // grandchild: must not appear

  std::vector<std::string> names;
  ASSERT_OK(fs.Dirlist(base, &names));
  EXPECT_EQ(names, (std::vector<std::string>{"a", "b", "d"}));

  ASSERT_OK(fs.Dirlist(P("d"), &names));
  EXPECT_EQ(names, (std::vector<std::string>{"inner"}));
}

TYPED_TEST(FsTest, DirlistClearsOldContentsAndHandlesEmptyDir) {
  FS_PRELUDE;
  ASSERT_OK(fs.CreateDir(P("empty")));
  std::vector<std::string> names = {"stale"};
  ASSERT_OK(fs.Dirlist(P("empty"), &names));
  EXPECT_TRUE(names.empty());
}

TYPED_TEST(FsTest, DirlistMissingDirIsNotFoundAndLeavesOutputAlone) {
  FS_PRELUDE;
  std::vector<std::string> names = {"sentinel"};
  Status s = fs.Dirlist(P("missing"), &names);
  EXPECT_TRUE(s.IsNotFound()) << s.ToString();
  EXPECT_EQ(names, (std::vector<std::string>{"sentinel"}));
}

TYPED_TEST(FsTest, TrailingSlashMeansSamePath) {
  FS_PRELUDE;
  ASSERT_OK(WriteFile(&fs, P("a"), "x"));
  std::vector<std::string> names;
  ASSERT_OK(fs.Dirlist(base + "/", &names));
  EXPECT_EQ(names, (std::vector<std::string>{"a"}));
}

// ---- handles ----

TYPED_TEST(FsTest, OperationsAfterCloseFail) {
  FS_PRELUDE;
  std::unique_ptr<WritableFile> w;
  ASSERT_OK(fs.NewWritableFile(P("a"), &w));
  ASSERT_OK(w->Close());
  EXPECT_FALSE(w->Append("x").ok());
  EXPECT_FALSE(w->Flush().ok());
  EXPECT_FALSE(w->Sync().ok());
  EXPECT_TRUE(w->Close().ok()); // closing twice is harmless
}

TYPED_TEST(FsTest, FlushSyncAndSyncDirSucceed) {
  FS_PRELUDE;
  std::unique_ptr<WritableFile> w;
  ASSERT_OK(fs.NewWritableFile(P("a"), &w));
  ASSERT_OK(w->Append("x"));
  ASSERT_OK(w->Flush());
  ASSERT_OK(w->Sync());
  ASSERT_OK(fs.SyncDir(base));
  EXPECT_TRUE(fs.SyncDir(P("missing")).IsNotFound());
}

TYPED_TEST(FsTest, TwoHandlesOnSameFileSeeEachOthersWrites) {
  FS_PRELUDE;
  std::unique_ptr<WritableFile> w;
  ASSERT_OK(fs.NewWritableFile(P("a"), &w));
  std::unique_ptr<RandomAccessFile> r;
  ASSERT_OK(fs.NewRandomAccessFile(P("a"), &r));
  EXPECT_EQ(ReadAt(r.get(), 0, 5), ""); // nothing written yet
  ASSERT_OK(w->Append("hey"));
  EXPECT_EQ(ReadAt(r.get(), 0, 5), "hey");
}
