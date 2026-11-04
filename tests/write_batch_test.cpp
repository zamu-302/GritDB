#include "coding.h"
#include "dbformat.h"
#include "memtable.h"
#include "write_batch.h"
#include <cstring>
#include <gtest/gtest.h>
#include <initializer_list>
#include <string_view>
#include <sys/types.h>
#include <vector>

using namespace gritdb;
namespace {
std::string Bytes(std::initializer_list<int> v) {
  std::string s;
  for (int b : v)
    s.push_back(static_cast<char>(b));
  return s;
}
std::string MakeBatch(uint64_t seq, uint32_t count,
                      const std::string &entries) {
  std::string s;
  PutFixed64(&s, seq);
  PutFixed32(&s, count);
  return s + entries;
}
Status Apply(const std::string &bytes, MemTable *m) {
  WriteBatch b;
  b.SetContents(bytes);
  return b.InsertInto(m);
}
}; // namespace

TEST(WriteBatchTest, NewBatchIsTwelveZeroBytes) {
  WriteBatch b;
  EXPECT_EQ(b.Contents(), std::string(12, '\0'));
}

TEST(WriteBatchTest, SequenceIsStoredAsBinary) {
  WriteBatch b;
  EXPECT_EQ(b.Contents().size(), 12u);
  b.SetSequence(5);
  EXPECT_EQ(static_cast<u_int8_t>(b.Contents()[0]), 5);
  for (int i = 1; i < 8; ++i) {
    EXPECT_EQ(b.Contents()[i], 0);
  }
  b.Clear();
  EXPECT_EQ(b.Contents(), std::string(12, '\0'));
}
TEST(WriteBatch, Put_Delete_Accuracy) {
  WriteBatch b;
  b.Put("sami", "100");
  b.Delete("sami");
  const char expected[] = {
      0, 0, 0,   0,   0,   0,   0, 0,             // sequence
      2, 0, 0,   0,                               // count
      1, 4, 's', 'a', 'm', 'i', 3, '1', '0', '0', // put sami
      0, 4, 's', 'a', 'm', 'i'                    // delete samu
  };
  EXPECT_EQ(memcmp(b.Contents().data(), expected, sizeof(expected)), 0);
}
TEST(WriteBatchTest, LongKeyUsesTwoVarintBytes) {
  WriteBatch b;
  b.Put(std::string(300, 'k'), "v");
  ASSERT_EQ(b.Contents().size(), 12u + 1 + 2 + 300 + 1 + 1);
  EXPECT_EQ(static_cast<uint8_t>(b.Contents()[13]), 0xAC);
  EXPECT_EQ(static_cast<uint8_t>(b.Contents()[14]), 0x02);
}

TEST(WriteBatchTest, RoundTripAppliesWithConsecutiveSequences) {
  WriteBatch b;
  b.Put("a", "1");
  b.Put("b", "2");
  b.Delete("a");
  b.SetSequence(10);

  WriteBatch copy;
  copy.SetContents(b.Contents());
  MemTable m;
  ASSERT_TRUE(copy.InsertInto(&m).ok());
  EXPECT_EQ(m.Count(), 3u);

  std::string v;
  EXPECT_EQ(m.Get("a", 11, &v), LookUpResult::Found);
  EXPECT_EQ(v, "1");
  EXPECT_EQ(m.Get("b", 11, &v), LookUpResult::Found);
  EXPECT_EQ(v, "2");
  EXPECT_EQ(m.Get("a", 12, &v), LookUpResult::Deleted);
  EXPECT_EQ(m.Get("a", 9, &v), LookUpResult::NotFound);
}

TEST(WriteBatchTest, EmptyBatchApplies) {
  WriteBatch b;
  MemTable m;
  EXPECT_TRUE(b.InsertInto(&m).ok());
  EXPECT_EQ(m.Count(), 0u);
}

TEST(WriteBatchTest, EveryTruncationIsCorruption) {
  WriteBatch b;
  b.Put("key1", "value1");
  b.Delete("key2");
  b.Put("key3", std::string(300, 'x'));
  b.SetSequence(1);
  const std::string full(b.Contents());

  for (size_t len = 0; len < full.size(); ++len) {
    std::vector<char> buf(
        full.begin(), full.begin() + len); // exact size: ASan sees over-reads
    const std::string_view view =
        len ? std::string_view(buf.data(), len) : std::string_view();
    WriteBatch t;
    t.SetContents(view);
    MemTable m;
    EXPECT_TRUE(t.InsertInto(&m).IsCorruption()) << "length " << len;
    EXPECT_EQ(m.Count(), 0u) << "length " << len;
  }
}

TEST(WriteBatchTest, MalformedBatchesAreCorruption) {
  struct Case {
    const char *name;
    std::string bytes;
  };
  const std::vector<Case> cases = {
      {"count with no entries", MakeBatch(1, 1, "")},
      {"unknown type byte", MakeBatch(1, 1, Bytes({2, 1, 'k'}))},
      {"truncated varint", MakeBatch(1, 1, Bytes({1, 0x80}))},
      {"key length beyond data", MakeBatch(1, 1, Bytes({1, 100, 'a'}))},
      {"missing value part", MakeBatch(1, 1, Bytes({1, 1, 'a'}))},
      {"leftover byte", MakeBatch(1, 1, Bytes({0, 1, 'a', 0}))},
      {"sequence overflow",
       MakeBatch(kMaxSequence, 2, Bytes({0, 1, 'a', 0, 1, 'b'}))},
      {"huge count", MakeBatch(1, 0xFFFFFFFFu, Bytes({0, 1, 'a'}))},
  };
  for (const auto &c : cases) {
    MemTable m;
    EXPECT_TRUE(Apply(c.bytes, &m).IsCorruption()) << c.name;
    EXPECT_EQ(m.Count(), 0u) << c.name;
  }
  MemTable edge; // the largest legal sequence with a single entry is fine
  EXPECT_TRUE(
      Apply(MakeBatch(kMaxSequence, 1, Bytes({0, 1, 'a'})), &edge).ok());
}

TEST(WriteBatchTest, DamagedThirdEntryLeavesMemTableUntouched) {
  const std::string entries = Bytes({1, 1, 'a', 1, '1'}) +
                              Bytes({1, 1, 'b', 1, '2'}) +
                              Bytes({1, 100, 'c'}); // third entry is cut off
  MemTable m;
  EXPECT_TRUE(Apply(MakeBatch(1, 3, entries), &m).IsCorruption());
  EXPECT_EQ(m.Count(), 0u); // atomic: entries 1 and 2 were not applied
}
