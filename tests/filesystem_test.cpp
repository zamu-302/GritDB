#include "../include/mem_filesystem.h"
#include "filesystem.h"
#include "status.h"
#include <gtest/gtest.h>
#include <memory>

using namespace gritdb;

#define ASSERT_OK(expr)                                                        \
  do {                                                                         \
    Status _s = (expr);                                                        \
    ASSERT_TRUE(_s.ok()) << _s.ToString();                                     \
  } while (0)

// helpers
namespace {
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

class MemFsTest : public ::testing::Test {
protected:
  void SetUp() override { ASSERT_OK(fs.CreateDir(base)); }
  std::string P(const std::string &name) const { return base + "/" + name; }

  MemFileSystem fs;
  const std::string base = "/test";
};
} // namespace
TEST(MemFileSystem, CreateBase) {
  MemFileSystem file;
  std::unique_ptr<WritableFile> val;
  Status currStatus = file.NewWritableFile("a.log", &val);
  Status okay = Status::OK();
  EXPECT_EQ(okay.ToString(), currStatus.ToString());
  Status currStatus2 = val->Append("hello world");
  EXPECT_EQ(okay.ToString(), currStatus2.ToString());
  EXPECT_TRUE(file.FileExists("a.log"));
}

TEST(MemFileSystem, CreateDir) {
  MemFileSystem file;
  Status okay = Status::OK();
  EXPECT_EQ(file.CreateDir("math").ToString(), okay.ToString());
  EXPECT_EQ(file.CreateDir("math/C++").ToString(), okay.ToString());
  EXPECT_EQ(file.CreateDir("math/C++/Projects").ToString(), okay.ToString());
  EXPECT_EQ(file.CreateDir("math/C++/Projects/GritDB").ToString(),
            okay.ToString());
  std::unique_ptr<WritableFile> val;
  EXPECT_EQ(okay.ToString(),
            file.NewWritableFile("math/C++/Projects/GritDB/grit.db", &val)
                .ToString());
  EXPECT_TRUE(file.FileExists("math/C++/Projects/GritDB/grit.db"));
}
TEST_F(MemFsTest, AppendInPiecesReadsBackExactly) {
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

TEST_F(MemFsTest, ReadAtOffsetsAndEdges) {
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

TEST_F(MemFsTest, EmptyFile) {
  ASSERT_OK(WriteFile(&fs, P("empty"), ""));
  uint64_t size = 99;
  ASSERT_OK(fs.FileSize(P("empty"), &size));
  EXPECT_EQ(size, 0u);
  std::string got = "not empty";
  ASSERT_OK(ReadAll(&fs, P("empty"), &got));
  EXPECT_EQ(got, "");
}

TEST_F(MemFsTest, LargeFileRoundTrip) {
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

TEST_F(MemFsTest, OpenMissingFileIsNotFound) {
  std::unique_ptr<RandomAccessFile> r;
  Status s = fs.NewRandomAccessFile(P("nope"), &r);
  EXPECT_TRUE(s.IsNotFound()) << s.ToString();
  uint64_t size = 0;
  EXPECT_TRUE(fs.FileSize(P("nope"), &size).IsNotFound());
  EXPECT_FALSE(fs.FileExists(P("nope")));
}

TEST_F(MemFsTest, CreateFileWithMissingParentIsNotFound) {
  std::unique_ptr<WritableFile> w;
  Status s = fs.NewWritableFile(P("no/such/dir/a.log"), &w);
  EXPECT_TRUE(s.IsNotFound()) << s.ToString();
}

// ---- create / overwrite ----

TEST_F(MemFsTest,
       CreatingSamePathTwiceGivesEmptyFileButOldReaderKeepsOldBytes) {
  ASSERT_OK(WriteFile(&fs, P("a"), "abc"));
  std::unique_ptr<RandomAccessFile> old_reader;
  ASSERT_OK(fs.NewRandomAccessFile(P("a"), &old_reader));

  std::unique_ptr<WritableFile> w;
  ASSERT_OK(fs.NewWritableFile(P("a"), &w)); // must succeed and replace

  uint64_t size = 99;
  ASSERT_OK(fs.FileSize(P("a"), &size));
  EXPECT_EQ(size, 0u);
  EXPECT_EQ(ReadAt(old_reader.get(), 0, 3), "abc");
}

TEST_F(MemFsTest, CreateFileOnDirectoryNameFails) {
  ASSERT_OK(fs.CreateDir(P("d")));
  std::unique_ptr<WritableFile> w;
  EXPECT_FALSE(fs.NewWritableFile(P("d"), &w).ok());
}

// ---- rename ----

TEST_F(MemFsTest, RenameOntoExistingFileReplacesIt) {
  ASSERT_OK(WriteFile(&fs, P("src"), "new"));
  ASSERT_OK(WriteFile(&fs, P("dst"), "old-and-longer"));
  ASSERT_OK(fs.RenameFile(P("src"), P("dst")));

  EXPECT_FALSE(fs.FileExists(P("src")));
  std::string got;
  ASSERT_OK(ReadAll(&fs, P("dst"), &got));
  EXPECT_EQ(got, "new");
}

TEST_F(MemFsTest, RenameToNewNameMovesContent) {
  ASSERT_OK(WriteFile(&fs, P("a"), "payload"));
  ASSERT_OK(fs.RenameFile(P("a"), P("b")));
  EXPECT_FALSE(fs.FileExists(P("a")));
  std::string got;
  ASSERT_OK(ReadAll(&fs, P("b"), &got));
  EXPECT_EQ(got, "payload");
}

TEST_F(MemFsTest, RenameMissingSourceIsNotFound) {
  Status s = fs.RenameFile(P("ghost"), P("other"));
  EXPECT_TRUE(s.IsNotFound()) << s.ToString();
  // even when source == destination
  EXPECT_TRUE(fs.RenameFile(P("ghost"), P("ghost")).IsNotFound());
}

TEST_F(MemFsTest, RenameOntoDirectoryFailsAndKeepsSource) {
  ASSERT_OK(fs.CreateDir(P("d")));
  ASSERT_OK(WriteFile(&fs, P("f"), "x"));
  EXPECT_FALSE(fs.RenameFile(P("f"), P("d")).ok());
  EXPECT_TRUE(fs.FileExists(P("f")));
}

// ---- delete ----

TEST_F(MemFsTest, DeleteRemovesFile) {
  ASSERT_OK(WriteFile(&fs, P("a"), "x"));
  ASSERT_OK(fs.DeleteFile(P("a")));
  EXPECT_FALSE(fs.FileExists(P("a")));
  std::unique_ptr<RandomAccessFile> r;
  EXPECT_TRUE(fs.NewRandomAccessFile(P("a"), &r).IsNotFound());
  EXPECT_TRUE(fs.DeleteFile(P("a")).IsNotFound()); // second delete
}

TEST_F(MemFsTest, DeleteWhileReaderOpenReaderStillWorks) {
  ASSERT_OK(WriteFile(&fs, P("a"), "hello"));
  std::unique_ptr<RandomAccessFile> r;
  ASSERT_OK(fs.NewRandomAccessFile(P("a"), &r));
  ASSERT_OK(fs.DeleteFile(P("a")));
  EXPECT_FALSE(fs.FileExists(P("a")));
  EXPECT_EQ(ReadAt(r.get(), 0, 5), "hello");
}

// ---- directories ----

TEST_F(MemFsTest, CreateDirNestedAndTwice) {
  ASSERT_OK(fs.CreateDir(P("x")));
  ASSERT_OK(fs.CreateDir(P("x/y")));
  ASSERT_OK(fs.CreateDir(P("x/y"))); // idempotent on purpose
}

TEST_F(MemFsTest, CreateDirWithMissingParentIsNotFound) {
  Status s = fs.CreateDir(P("no/child"));
  EXPECT_TRUE(s.IsNotFound()) << s.ToString();
}

TEST_F(MemFsTest, CreateDirOverFileFails) {
  ASSERT_OK(WriteFile(&fs, P("f"), "x"));
  EXPECT_FALSE(fs.CreateDir(P("f")).ok());
}

TEST_F(MemFsTest, DirlistReturnsImmediateChildrenSorted) {
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

TEST_F(MemFsTest, DirlistClearsOldContentsAndHandlesEmptyDir) {
  ASSERT_OK(fs.CreateDir(P("empty")));
  std::vector<std::string> names = {"stale"};
  ASSERT_OK(fs.Dirlist(P("empty"), &names));
  EXPECT_TRUE(names.empty());
}

TEST_F(MemFsTest, DirlistMissingDirIsNotFoundAndLeavesOutputAlone) {
  std::vector<std::string> names = {"sentinel"};
  Status s = fs.Dirlist(P("missing"), &names);
  EXPECT_TRUE(s.IsNotFound()) << s.ToString();
  EXPECT_EQ(names, (std::vector<std::string>{"sentinel"}));
}

TEST_F(MemFsTest, TrailingSlashMeansSamePath) {
  ASSERT_OK(WriteFile(&fs, P("a"), "x"));
  std::vector<std::string> names;
  ASSERT_OK(fs.Dirlist(base + "/", &names));
  EXPECT_EQ(names, (std::vector<std::string>{"a"}));
}

// ---- handles ----

TEST_F(MemFsTest, OperationsAfterCloseFail) {
  std::unique_ptr<WritableFile> w;
  ASSERT_OK(fs.NewWritableFile(P("a"), &w));
  ASSERT_OK(w->Close());
  EXPECT_FALSE(w->Append("x").ok());
  EXPECT_FALSE(w->Flush().ok());
  EXPECT_FALSE(w->Sync().ok());
  EXPECT_TRUE(w->Close().ok()); // closing twice is harmless
}

TEST_F(MemFsTest, FlushSyncAndSyncDirSucceed) {
  std::unique_ptr<WritableFile> w;
  ASSERT_OK(fs.NewWritableFile(P("a"), &w));
  ASSERT_OK(w->Append("x"));
  ASSERT_OK(w->Flush());
  ASSERT_OK(w->Sync());
  ASSERT_OK(fs.SyncDir(base));
  EXPECT_TRUE(fs.SyncDir(P("missing")).IsNotFound());
}

TEST_F(MemFsTest, TwoHandlesOnSameFileSeeEachOthersWrites) {
  std::unique_ptr<WritableFile> w;
  ASSERT_OK(fs.NewWritableFile(P("a"), &w));
  std::unique_ptr<RandomAccessFile> r;
  ASSERT_OK(fs.NewRandomAccessFile(P("a"), &r));
  EXPECT_EQ(ReadAt(r.get(), 0, 5), ""); // nothing written yet
  ASSERT_OK(w->Append("hey"));
  EXPECT_EQ(ReadAt(r.get(), 0, 5), "hey");
}
