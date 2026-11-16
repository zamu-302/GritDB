#pragma once
// Test helpers for the SSTable layer. Header-only. Namespace gritdb::tu
// (not "testing", which would clash with GoogleTest).

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "block_builder.h"
#include "block_iterator.h"
#include "coding.h"
#include "crc32c.h"
#include "dbformat.h"
#include "filesystem.h"
#include "mem_filesystem.h"
#include "memtable.h"
#include "status.h"
#include "table.h"
#include "table_builder.h"

namespace gritdb::tu {

using Options = TableFormat;

inline std::string Hex(std::string_view s) {
  static const char *d = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    out.push_back(d[c >> 4]);
    out.push_back(d[c & 15]);
    out.push_back(' ');
  }
  return out;
}

// Index of the first differing byte, or min(size) if one is a prefix of the
// other.
inline size_t FirstDiff(std::string_view a, std::string_view b) {
  size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i)
    if (a[i] != b[i])
      return i;
  return n;
}

inline std::string MakeKey(int n) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "k%06d", n);
  return buf;
}

// "value<i>", right-padded with 'x' up to `len` when len is larger.
inline std::string MakeValue(int i, size_t len = 0) {
  std::string v = "value" + std::to_string(i);
  if (v.size() < len)
    v.append(len - v.size(), 'x');
  return v;
}

using Items = std::vector<std::pair<std::string, std::string>>;

// n entries with keys MakeKey(step * i), so odd numbers are guaranteed gaps.
inline Items MakeItems(int n, int step = 2, size_t value_len = 0) {
  Items items;
  for (int i = 0; i < n; ++i)
    items.emplace_back(MakeKey(step * i), MakeValue(i, value_len));
  return items;
}

// All keys, all gap keys, and sentinels before / after everything.
inline std::vector<std::string> ProbeKeys(const Items &items) {
  std::vector<std::string> probes = {"", "a", "z", std::string("\xff", 1)};
  for (const auto &kv : items) {
    probes.push_back(kv.first);
    probes.push_back(kv.first + std::string(1, '\0')); // just after a key
  }
  return probes;
}

inline std::string IKey(std::string_view user, uint64_t seq, ValueType type) {
  std::string k;
  AppendInternalKey(&k, user, seq,
                    type); // CHECK: argument order / pointer-ness
  return k;
}

inline Options TestOptions(size_t block_size, Compartor cmp = CompareByteWise,
                           size_t restart_interval = 16) {
  Options o;
  o.block_size = block_size;
  o.block_restart_interval = restart_interval; // CHECK: field name
  o.comp = cmp;
  o.verify_checksums = true;
  return o;
}

// -------------------------------------------------------------- mutation -----

inline std::string FlipByte(std::string s, size_t pos, uint8_t mask = 0xFF) {
  EXPECT_LT(pos, s.size());
  if (pos < s.size())
    s[pos] = static_cast<char>(static_cast<uint8_t>(s[pos]) ^ mask);
  return s;
}
inline std::string FlipBit(std::string s, size_t pos, int bit) {
  return FlipByte(std::move(s), pos, static_cast<uint8_t>(1u << bit));
}
inline std::string SetByte(std::string s, size_t pos, uint8_t v) {
  EXPECT_LT(pos, s.size());
  if (pos < s.size())
    s[pos] = static_cast<char>(v);
  return s;
}
inline std::string SetFixed64(std::string s, size_t pos, uint64_t v) {
  EXPECT_LE(pos + 8, s.size());
  if (pos + 8 <= s.size())
    EncodeFixed64(&s[pos], v);
  return s;
}
inline std::string SetFixed32(std::string s, size_t pos, uint32_t v) {
  EXPECT_LE(pos + 4, s.size());
  if (pos + 4 <= s.size())
    EncodeFixed32(&s[pos], v);
  return s;
}

// ------------------------------------------------------------ environment ----

// One in-memory filesystem with a directory, and unique file names.
struct Env {
  MemFileSystem fs; // CHECK: default-constructible?
  int counter = 0;

  Env() {
    Status s = fs.CreateDir("/db"); // CHECK: does your MemFileSystem need this?
    EXPECT_TRUE(s.ok()) << s.ToString();
  }
  std::string NextPath(const char *ext = ".sst") {
    return "/db/f" + std::to_string(counter++) + ext;
  }
};

inline Status WriteFile(Env &env, const std::string &path,
                        std::string_view data) {
  std::unique_ptr<WritableFile> f;
  Status s = env.fs.NewWritableFile(path, &f);
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

// Puts `bytes` into a fresh file and opens it for random access.
inline Status NewRandomFile(Env &env, std::string_view bytes,
                            std::unique_ptr<RandomAccessFile> *out) {
  std::string path = env.NextPath();
  Status s = WriteFile(env, path, bytes);
  if (!s.ok())
    return s;
  return env.fs.NewRandomAccessFile(path, out);
}

// Opens a table from raw bytes. size_override lets a test lie about file_size.
inline Status
OpenFromBytes(Env &env, std::string_view bytes, const Options &opts,
              std::unique_ptr<Table> *out,
              std::optional<uint64_t> size_override = std::nullopt) {
  std::unique_ptr<RandomAccessFile> file;
  Status s = NewRandomFile(env, bytes, &file);
  if (!s.ok())
    return s;
  return Table::Open(opts, std::move(file),
                     size_override.value_or(bytes.size()), out);
}

// Builds a table through a real TableBuilder and returns the file's bytes.
// Also checks FileSize() and NumEntries() on every build.
inline std::string BuildTable(const Items &items, const Options &opts) {
  Env env;
  std::string path = env.NextPath();
  std::unique_ptr<WritableFile> file;
  Status s = env.fs.NewWritableFile(path, &file);
  EXPECT_TRUE(s.ok()) << s.ToString();
  if (!s.ok())
    return {};

  TableBuilder b(opts, file.get());
  for (const auto &kv : items)
    b.Add(kv.first, kv.second);
  s = b.Finish();
  EXPECT_TRUE(s.ok()) << s.ToString();
  EXPECT_EQ(b.NumEntries(), items.size());
  s = file->Sync();
  EXPECT_TRUE(s.ok()) << s.ToString();
  s = file->Close();
  EXPECT_TRUE(s.ok()) << s.ToString();

  std::string bytes;
  s = ReadFileToString(&env.fs, path, &bytes);
  EXPECT_TRUE(s.ok()) << s.ToString();
  EXPECT_EQ(b.FileSize(), bytes.size());
  return bytes;
}

// ------------------------------------------------------------- assertions ----

inline void ExpectGet(const Table &t, std::string_view seek,
                      std::string_view want_key, std::string_view want_value) {
  std::string k, v;
  Status s = t.InternalGet(seek, &k, &v);
  ASSERT_TRUE(s.ok()) << "seek " << Hex(seek) << ": " << s.ToString();
  EXPECT_EQ(k, std::string(want_key)) << "seek " << Hex(seek);
  EXPECT_EQ(v, std::string(want_value)) << "seek " << Hex(seek);
}

inline void ExpectNotFound(const Table &t, std::string_view seek) {
  std::string k, v;
  Status s = t.InternalGet(seek, &k, &v);
  EXPECT_TRUE(s.IsNotFound()) << "seek " << Hex(seek) << ": " << s.ToString();
}

inline void ExpectCorruption(const Status &s, const char *what = "") {
  EXPECT_TRUE(s.IsCorruption()) << what << ": " << s.ToString();
}

// Checks every key, every gap, and the two ends against the expected items
// (items must be sorted, and keys must be the MakeKey(2i) kind so that
// MakeKey(2i+1) is a gap).
inline void ExpectAllReads(const Table &t, const Items &items) {
  for (size_t i = 0; i < items.size(); ++i) {
    SCOPED_TRACE("key index " + std::to_string(i));
    ExpectGet(t, items[i].first, items[i].first, items[i].second);
    std::string gap = items[i].first + std::string(1, '\0'); // just after key i
    if (i + 1 < items.size()) {
      ExpectGet(t, gap, items[i + 1].first, items[i + 1].second);
    } else {
      ExpectNotFound(t, gap);
    }
  }
  if (items.empty()) {
    ExpectNotFound(t, "");
  } else {
    ExpectGet(t, "", items[0].first, items[0].second);
  }
  ExpectNotFound(t, std::string(64, '\xff'));
}

// ----------------------------------------------------------------- layout ----

struct Region {
  size_t begin = 0;
  size_t end = 0;     // exclusive
  BlockHandle handle; // for data and index regions
};

struct Layout {
  bool ok = false;
  std::vector<Region>
      data;      // data blocks, in file order, each includes its 5-byte trailer
  Region index;  // index block + trailer
  Region footer; // last 40 bytes
  std::vector<std::string>
      last_keys; // the index keys (last key of each data block)
};

// Decodes the file the same way a reader would: footer -> index -> handles.
inline Layout ParseLayout(const std::string &bytes) {
  Layout L;
  if (bytes.size() < footer_size) {
    ADD_FAILURE() << "file shorter than a footer";
    return L;
  }
  L.footer = {bytes.size() - footer_size, bytes.size(), {}};
  std::string_view fv(bytes.data() + L.footer.begin, footer_size);
  Footer footer;
  Status s = footer.DecodeFrom(&fv);
  if (!s.ok()) {
    ADD_FAILURE() << "footer: " << s.ToString();
    return L;
  }
  const BlockHandle ih = footer.index_handle;
  s = CheckHandleRange(ih, bytes.size());
  if (!s.ok()) {
    ADD_FAILURE() << "index handle: " << s.ToString();
    return L;
  }
  L.index = {static_cast<size_t>(ih.offset),
             static_cast<size_t>(ih.offset + ih.size + trailer_size), ih};

  std::string_view with_trailer(bytes.data() + ih.offset,
                                ih.size + trailer_size);
  std::string_view body;
  s = CheckTrailer(with_trailer, &body);
  if (!s.ok()) {
    ADD_FAILURE() << "index trailer: " << s.ToString();
    return L;
  }
  std::unique_ptr<Block> index_block;
  s = Block::Open(std::string(body), &index_block);
  if (!s.ok()) {
    ADD_FAILURE() << "index block: " << s.ToString();
    return L;
  }
  BlockIterator it(index_block.get(),
                   CompareByteWise); // only SeekToFirst/Next are used
  for (it.SeekToFirst(); it.Valid(); it.Next()) {
    BlockHandle h;
    std::string_view v = it.get_value();
    s = h.DecodeFrom(&v);
    if (!s.ok() || !v.empty()) {
      ADD_FAILURE() << "bad handle in index";
      return L;
    }
    L.data.push_back({static_cast<size_t>(h.offset),
                      static_cast<size_t>(h.offset + h.size + trailer_size),
                      h});
    L.last_keys.emplace_back(it.get_key());
  }
  EXPECT_TRUE(it.get_status().ok());
  L.ok = true;
  return L;
}

// ------------------------------------------- hand-assembling odd tables ------

inline std::string EncodeHandle(const BlockHandle &h) {
  std::string s;
  h.EncodeTo(&s);
  return s;
}

// A data/index block built with the real BlockBuilder.
inline std::string BuildBlockBytes(const Items &items, size_t restart_interval,
                                   Compartor cmp = CompareByteWise) {
  BlockBuilder b(restart_interval);
  for (const auto &kv : items)
    b.Add(kv.first, kv.second);
  return std::string(b.Finish());
}

// Appends contents + trailer to *file and returns the handle (size excludes
// trailer).
inline BlockHandle AppendBlock(std::string *file, std::string_view contents) {
  BlockHandle h;
  h.offset = file->size();
  h.size = contents.size();
  file->append(contents.data(), contents.size());
  MakeTrailer(BlockType::Uncompressed, contents, file);
  return h;
}

inline void AppendFooter(std::string *file, const BlockHandle &index_handle) {
  Footer f;
  f.index_handle = index_handle; // filter handle stays zero
  f.EncodeTo(file);
}

// ---------------------------------------------------------- I/O decorators ---

// Pure sink: counts bytes, stores nothing. For builder benchmarks.
class CountingSink : public WritableFile {
public:
  Status Append(std::string_view to) override {
    bytes_ += to.size();
    return Status::OK();
  }
  Status Flush() override { return Status::OK(); }
  Status Sync() override { return Status::OK(); }
  Status Close() override { return Status::OK(); }
  uint64_t bytes() const { return bytes_; }

private:
  uint64_t bytes_ = 0;
};

// Forwards the first fail_at appends, then fails every append with IOError.
class FailingWritableFile : public WritableFile {
public:
  FailingWritableFile(std::unique_ptr<WritableFile> inner, size_t fail_at)
      : inner_(std::move(inner)), fail_at_(fail_at) {}
  Status Append(std::string_view to) override {
    if (appends_++ >= fail_at_)
      return Status::IOError("injected append failure");
    return inner_->Append(to);
  }
  Status Flush() override { return inner_->Flush(); }
  Status Sync() override { return inner_->Sync(); }
  Status Close() override { return inner_->Close(); }

private:
  std::unique_ptr<WritableFile> inner_;
  size_t fail_at_;
  size_t appends_ = 0;
};

// The fail_at-th Read (0-based) returns IOError; all others forward.
class FlakyRandomAccessFile : public RandomAccessFile {
public:
  FlakyRandomAccessFile(std::unique_ptr<RandomAccessFile> inner, size_t fail_at)
      : inner_(std::move(inner)), fail_at_(fail_at) {}
  Status Read(std::string_view *result, char *buff, size_t n,
              uint64_t offset) const override {
    if (reads_++ == fail_at_)
      return Status::IOError("injected read failure");
    return inner_->Read(result, buff, n, offset);
  }

private:
  std::unique_ptr<RandomAccessFile> inner_;
  size_t fail_at_;
  mutable size_t reads_ = 0;
};

struct IoCounters {
  std::atomic<uint64_t> reads{0};
  std::atomic<uint64_t> bytes{0}; // sum of requested n
};

// Counts Read calls and bytes; the test keeps the shared IoCounters.
class CountingRandomAccessFile : public RandomAccessFile {
public:
  CountingRandomAccessFile(std::unique_ptr<RandomAccessFile> inner,
                           std::shared_ptr<IoCounters> counters)
      : inner_(std::move(inner)), counters_(std::move(counters)) {}
  Status Read(std::string_view *result, char *buff, size_t n,
              uint64_t offset) const override {
    counters_->reads.fetch_add(1, std::memory_order_relaxed);
    counters_->bytes.fetch_add(n, std::memory_order_relaxed);
    return inner_->Read(result, buff, n, offset);
  }

private:
  std::unique_ptr<RandomAccessFile> inner_;
  std::shared_ptr<IoCounters> counters_;
};

// ------------------------------------------------------------ sweep driver ---

// Open (if possible) and read a set of probe keys. Never asserts on results:
// its job is to run the code on possibly-corrupt bytes so a crash/UB fails the
// test. Returns how many InternalGet calls returned OK.
inline int Exercise(Env &env, std::string_view bytes, const Options &opts,
                    const std::vector<std::string> &probes,
                    std::optional<uint64_t> size_override = std::nullopt) {
  std::unique_ptr<Table> table;
  if (!OpenFromBytes(env, bytes, opts, &table, size_override).ok())
    return -1;
  int ok = 0;
  for (const auto &p : probes) {
    std::string k, v;
    if (table->InternalGet(p, &k, &v).ok())
      ++ok;
  }
  return ok;
}

// ------------------------------------------- memtable <-> table lookups ------

// What DB::Get will do on top of InternalGet:
//   first entry >= (user, snapshot, Value); wrong user key -> NotFound;
//   Deletion -> Deleted; Value -> Found.
inline LookUpResult TableLookup(const Table &t, std::string_view user,
                                uint64_t snapshot, std::string *value) {
  std::string fk, fv;
  Status s = t.InternalGet(IKey(user, snapshot, ValueType::Value), &fk, &fv);
  if (s.IsNotFound())
    return LookUpResult::NotFound;
  EXPECT_TRUE(s.ok()) << s.ToString();
  if (!s.ok())
    return LookUpResult::NotFound;
  if (ExtractUserKey(fk) != user)
    return LookUpResult::NotFound;
  if (TagType(ExtractTag(fk)) == ValueType::Deletion)
    return LookUpResult::Deleted;
  *value = std::move(fv);
  return LookUpResult::Found;
}

} // namespace gritdb::tu
