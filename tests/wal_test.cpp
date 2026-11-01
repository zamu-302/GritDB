#include "coding.h"
#include "crc32c.h"
#include "wal.h"
#include <gtest/gtest.h>
#include <mem_filesystem.h>

using namespace gritdb;

TEST(EncodeRecordTest, AbcFull) {
  std::string rec = EncodeRecord(RecordType::Full, "abc");
  ASSERT_EQ(rec.size(), 10u);
  EXPECT_EQ(rec[4], 3);
  EXPECT_EQ(rec[5], 0);
  EXPECT_EQ(rec[6], 1);

  // Built by hand, not from the function under test, so the check isn't
  // circular.
  const char body[] = {0x03, 0x00, 0x01, 'a', 'b', 'c'};
  EXPECT_EQ(DecodeFixed32(rec.data()), crc32c::Value(body, sizeof body));
}
TEST(LogWriterTest, PadsWhenSixBytesLeft) {
  MemFileSystem fs;
  std::unique_ptr<WritableFile> f;
  ASSERT_TRUE(fs.NewWritableFile("wal.log", &f).ok());
  LogWriter w(std::move(f));
  ASSERT_TRUE(w.AddRecord(std::string(32761 - 6, 'a')).ok()); // leaves 6 bytes
  ASSERT_TRUE(w.AddRecord("x").ok());
  ASSERT_TRUE(w.Close().ok());

  std::string bytes;
  ASSERT_TRUE(ReadFileToString(&fs, "wal.log", &bytes).ok());
  ASSERT_EQ(bytes.size(), kBlockSize + 8);
  EXPECT_EQ(bytes.substr(32762, 6), std::string(6, '\0'));
  EXPECT_EQ(bytes.substr(kBlockSize), EncodeRecord(RecordType::Full, "x"));
}

TEST(LogReaderTest, TornTailAtEveryCut) {
  const std::string a = EncodeRecord(RecordType::Full, "first");
  const std::string b = EncodeRecord(RecordType::Full, "second");
  const std::string c = EncodeRecord(RecordType::Full, "third");
  const std::string all = a + b + c;

  for (size_t cut = a.size() + b.size(); cut < all.size(); ++cut) {
    // Exact-size heap buffer, so ASan flags any read past `cut`.
    std::vector<char> buf(all.begin(), all.begin() + cut);
    LogReader r(std::string_view(buf.data(), buf.size()));
    std::string rec;
    ASSERT_TRUE(r.ReadRecord(&rec));
    EXPECT_EQ(rec, "first");
    ASSERT_TRUE(r.ReadRecord(&rec));
    EXPECT_EQ(rec, "second");
    EXPECT_FALSE(r.ReadRecord(&rec));
    EXPECT_TRUE(r.status().ok()) << "cut at " << cut;
  }
}
