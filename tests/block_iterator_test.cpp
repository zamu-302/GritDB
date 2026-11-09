#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "block.h"
#include "block_builder.h"
#include "block_iterator.h"
#include "coding.h"
#include "dbformat.h"
#include "status.h"

using namespace gritdb;

namespace {

using Items = std::vector<std::pair<std::string, std::string>>;

std::string Hex(std::string_view s) {
  static const char *d = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    out.push_back(d[c >> 4]);
    out.push_back(d[c & 15]);
    out.push_back(' ');
  }
  return out;
}

std::string BuildBlock(size_t interval, const Items &items) {
  BlockBuilder b(interval);
  for (const auto &[k, v] : items)
    b.Add(k, v);
  return std::string(b.Finish());
}

std::string MakeKey(int n) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "k%06d", n);
  return buf;
}

std::unique_ptr<Block> OpenOk(std::string bytes) {
  std::unique_ptr<Block> block;
  Status s = Block::Open(std::move(bytes), &block);
  EXPECT_TRUE(s.ok());
  return block;
}

std::string Block39() {
  return BuildBlock(2,
                    {{"app", "1"}, {"apple", "2"}, {"apply", "3"}, {"b", "4"}});
}

std::string Patch32(std::string s, size_t pos, uint32_t v) {
  EncodeFixed32(&s[pos], v);
  return s;
}
std::string PatchByte(std::string s, size_t pos, uint8_t v) {
  s[pos] = static_cast<char>(v);
  return s;
}

// A value view must lie inside the block's own bytes.
void CheckInside(const Block &b, std::string_view v) {
  auto lo = reinterpret_cast<uintptr_t>(b.get_data().data());
  auto hi = lo + b.get_data().size();
  auto p = reinterpret_cast<uintptr_t>(v.data());
  EXPECT_GE(p, lo);
  EXPECT_LE(p + v.size(), hi);
}

// Full forward scan. The step cap makes a non-terminating iterator a failure
// instead of a hang: every entry takes at least 3 bytes, so it cannot exceed
// size().
Items Scan(const Block &block, Compartor cmp = CompareByteWise,
           Status *final_status = nullptr) {
  Items out;
  BlockIterator it(&block, cmp);
  for (it.SeekToFirst(); it.Valid(); it.Next()) {
    CheckInside(block, it.get_value());
    out.emplace_back(std::string(it.get_key()), std::string(it.get_value()));
    if (out.size() > block.size()) {
      ADD_FAILURE() << "iterator did not terminate";
      break;
    }
  }
  if (final_status)
    *final_status = it.get_status();
  return out;
}

} // namespace

TEST(BlockOpenTest, ValidBlock) {
  auto block = OpenOk(Block39());
  ASSERT_TRUE(block != nullptr);
  EXPECT_EQ(block->size(), 39u);
  EXPECT_EQ(block->num_restart(), 2u);
  EXPECT_EQ(block->entries_end(), 27u);
  EXPECT_EQ(block->restartsOffset(0), 0u);
  EXPECT_EQ(block->restartsOffset(1), 13u);
}

TEST(BlockOpenTest, EmptyBlockOpens) {
  auto block = OpenOk(BuildBlock(16, {}));
  ASSERT_TRUE(block != nullptr);
  EXPECT_EQ(block->size(), 8u);
  EXPECT_EQ(block->entries_end(), 0u);
  EXPECT_TRUE(Scan(*block).empty());
}

TEST(BlockOpenTest, TooSmallIsCorruption) {
  for (size_t n = 0; n < 4; ++n) {
    std::unique_ptr<Block> block;
    Status s = Block::Open(std::string(n, '\0'), &block);
    EXPECT_FALSE(s.ok()) << "size " << n;
    EXPECT_TRUE(block == nullptr); // *out untouched on failure
  }
}

TEST(BlockOpenTest, BadRestartCount) {
  const std::string good = Block39();
  // count lives in the last 4 bytes (index 35). 8 "fits" but then restart[0]
  // is read from inside entry data and is non-zero.
  for (uint32_t bad : {0u, 8u, 9u, 0x40000001u, 0xFFFFFFFFu}) {
    std::unique_ptr<Block> block;
    Status s = Block::Open(Patch32(good, 35, bad), &block);
    EXPECT_FALSE(s.ok()) << "count " << bad;
    EXPECT_TRUE(block == nullptr);
  }
}

TEST(BlockOpenTest, BadRestartOffsets) {
  const std::string good = Block39(); // restart array: [27]=0, [31]=13
  struct Case {
    size_t pos;
    uint32_t value;
  };
  for (Case c : {Case{27, 1}, Case{31, 0}, Case{31, 27}, Case{31, 100}}) {
    std::unique_ptr<Block> block;
    EXPECT_FALSE(Block::Open(Patch32(good, c.pos, c.value), &block).ok())
        << "pos " << c.pos << " value " << c.value;
  }
  const std::string four =
      BuildBlock(1, {{"app", "1"}, {"apple", "2"}, {"apply", "3"}, {"b", "4"}});
  for (uint32_t bad : {3u, 7u}) { // less than / equal to restart[1] (=7)
    std::unique_ptr<Block> block;
    EXPECT_FALSE(Block::Open(Patch32(four, 38, bad), &block).ok());
  }
}

TEST(BlockOpenTest, TruncationSweepNeverCrashes) {
  const std::string good = Block39();
  for (size_t n = 0; n <= good.size(); ++n) {
    std::unique_ptr<Block> block;
    if (!Block::Open(good.substr(0, n), &block).ok())
      continue;
    Scan(*block);
    for (const char *t : {"", "apple", "zzz"}) {
      BlockIterator it(block.get(), CompareByteWise);
      it.Seek(t);
    }
  }
}

TEST(BlockIterTest, ScanExactBlock) {
  auto block = OpenOk(Block39());
  ASSERT_TRUE(block != nullptr);
  Items want = {{"app", "1"}, {"apple", "2"}, {"apply", "3"}, {"b", "4"}};
  Status st;
  EXPECT_EQ(Scan(*block, CompareByteWise, &st), want);
  EXPECT_TRUE(st.ok()); // running off the end is not an error
}

TEST(BlockIterTest, RoundTripAllIntervalsAndCounts) {
  for (size_t interval : {1, 2, 16, 1000}) {
    for (int n : {0, 1, 2, 15, 16, 17, 1000}) {
      SCOPED_TRACE("interval=" + std::to_string(interval) +
                   " n=" + std::to_string(n));
      Items items;
      for (int i = 0; i < n; ++i)
        items.emplace_back(MakeKey(2 * i), "value" + std::to_string(i));
      auto block = OpenOk(BuildBlock(interval, items));
      ASSERT_TRUE(block != nullptr);
      EXPECT_EQ(Scan(*block), items);
    }
  }
}

TEST(BlockIterTest, EmptyBlockIsInvalidButOk) {
  auto block = OpenOk(BuildBlock(16, {}));
  ASSERT_TRUE(block != nullptr);
  BlockIterator it(block.get(), CompareByteWise);
  it.SeekToFirst();
  EXPECT_FALSE(it.Valid());
  EXPECT_TRUE(it.get_status().ok());
}

TEST(BlockIterTest, EmptyKeyAndSingleEntry) {
  Items a = {{"", "e"}, {"a", "1"}};
  auto b1 = OpenOk(BuildBlock(16, a));
  ASSERT_TRUE(b1 != nullptr);
  EXPECT_EQ(Scan(*b1), a);

  Items one = {{"only", "v"}};
  auto b2 = OpenOk(BuildBlock(16, one));
  ASSERT_TRUE(b2 != nullptr);
  EXPECT_EQ(Scan(*b2), one);
}

TEST(BlockIterTest, PrefixKeysAndVarintBoundaries) {
  Items items = {{std::string(127, 'a'), "v1"},
                 {std::string(128, 'a'), "v2"},
                 {std::string(128, 'a') + "b", "v3"},
                 {std::string(300, 'z'), std::string(127, 'x')},
                 {std::string(301, 'z'), std::string(128, 'y')}};
  auto block = OpenOk(BuildBlock(16, items));
  ASSERT_TRUE(block != nullptr);
  EXPECT_EQ(Scan(*block), items);
}

TEST(BlockIterTest, EmbeddedZeroBytes) {
  Items items = {{std::string("a\0b", 3), std::string("v\0", 2)},
                 {std::string("a\0c", 3), std::string("\0w", 2)}};
  auto block = OpenOk(BuildBlock(2, items));
  ASSERT_TRUE(block != nullptr);
  EXPECT_EQ(Scan(*block), items);
}

TEST(BlockIterTest, OneMegabyteValue) {
  Items items = {{"a", std::string(1 << 20, 'x')}, {"b", "tail"}};
  auto block = OpenOk(BuildBlock(16, items));
  ASSERT_TRUE(block != nullptr);
  EXPECT_EQ(Scan(*block), items);
}

TEST(BlockSeekTest, EveryKeyEveryGapAllIntervals) {
  const int n = 203; // deliberately not a multiple of any interval
  for (size_t interval : {1, 2, 3, 16, 1000}) {
    SCOPED_TRACE("interval=" + std::to_string(interval));
    Items items;
    for (int i = 0; i < n; ++i)
      items.emplace_back(MakeKey(2 * i), "v" + std::to_string(i));
    auto block = OpenOk(BuildBlock(interval, items));
    ASSERT_TRUE(block != nullptr);

    BlockIterator it(block.get(),
                     CompareByteWise); // reused: Seek must reset state
    for (int i = 0; i < n; ++i) {
      it.Seek(MakeKey(2 * i)); // exact hit
      ASSERT_TRUE(it.Valid()) << i;
      EXPECT_EQ(it.get_key(), items[i].first);
      EXPECT_EQ(it.get_value(), items[i].second);

      it.Seek(MakeKey(2 * i + 1)); // gap: lands on the next key
      if (i + 1 < n) {
        ASSERT_TRUE(it.Valid()) << i;
        EXPECT_EQ(it.get_key(), items[i + 1].first);
      } else {
        EXPECT_FALSE(it.Valid());
        EXPECT_TRUE(it.get_status().ok());
      }
    }
    it.Seek(""); // before the first key
    ASSERT_TRUE(it.Valid());
    EXPECT_EQ(it.get_key(), items[0].first);
    it.Seek("a"); // 'a' < 'k'
    ASSERT_TRUE(it.Valid());
    EXPECT_EQ(it.get_key(), items[0].first);
    it.Seek("z"); // after the last key
    EXPECT_FALSE(it.Valid());
    EXPECT_TRUE(it.get_status().ok());
  }
}

TEST(BlockSeekTest, SeekThenNextWalksTheRest) {
  const int n = 40;
  Items items;
  for (int i = 0; i < n; ++i)
    items.emplace_back(MakeKey(2 * i), "v" + std::to_string(i));
  for (size_t interval : {1, 4, 7}) {
    auto block = OpenOk(BuildBlock(interval, items));
    ASSERT_TRUE(block != nullptr);
    for (int start = 0; start < n; ++start) {
      BlockIterator it(block.get(), CompareByteWise);
      it.Seek(MakeKey(2 * start));
      for (int i = start; i < n; ++i, it.Next()) {
        ASSERT_TRUE(it.Valid())
            << "interval " << interval << " start " << start << " i " << i;
        EXPECT_EQ(it.get_key(), items[i].first);
      }
      EXPECT_FALSE(it.Valid());
    }
  }
}
// Scans a block built from `bytes`; expects a clean Corruption at the end.
void ExpectScanCorrupt(std::string bytes) {
  std::unique_ptr<Block> block;
  ASSERT_TRUE(
      Block::Open(std::move(bytes), &block).ok()); // footer is still valid
  Status st;
  Scan(*block, CompareByteWise, &st);
  EXPECT_FALSE(st.ok());
  EXPECT_TRUE(st.IsCorruption());
}

// Layout of Block39(): entry1 starts at 7 -> [7]=shared [8]=non_shared
// [9]=value_len entry2 (restart) starts at 13 -> [13]=shared. restart array at
// 27 and 31.
TEST(BlockCorruptionTest, SharedLongerThanPreviousKey) {
  ExpectScanCorrupt(PatchByte(Block39(), 7, 0x10));
}
TEST(BlockCorruptionTest, NonSharedRunsOffTheEnd) {
  ExpectScanCorrupt(PatchByte(Block39(), 8, 0x7F));
}
TEST(BlockCorruptionTest, ValueLenRunsOffTheEnd) {
  ExpectScanCorrupt(PatchByte(Block39(), 9, 0x7F));
}

TEST(BlockCorruptionTest, TruncatedVarint) {
  std::string s;
  s.push_back('\x80'); // continuation bit set, then the entry region ends
  PutFixed32(&s, 0);   // restart[0] = 0
  PutFixed32(&s, 1);   // one restart
  ExpectScanCorrupt(s);
}

TEST(BlockCorruptionTest, RestartEntryWithNonZeroShared) {
  auto block = OpenOk(PatchByte(Block39(), 13, 0x02));
  ASSERT_TRUE(block != nullptr);
  BlockIterator it(block.get(), CompareByteWise);
  it.Seek("b"); // binary search decodes restart 1
  EXPECT_FALSE(it.Valid());
  EXPECT_TRUE(it.get_status().IsCorruption());
}

TEST(BlockCorruptionTest, RestartPointsIntoMiddleOfAnEntry) {
  auto block = OpenOk(Patch32(Block39(), 31, 9)); // 9 is inside entry1
  ASSERT_TRUE(block != nullptr);
  BlockIterator it(block.get(), CompareByteWise);
  it.Seek("b");
  EXPECT_FALSE(it.Valid());
  EXPECT_TRUE(it.get_status().IsCorruption());
}

// Run every operation on possibly-corrupt bytes: nothing may crash or hang.
void Exercise(std::string bytes) {
  std::unique_ptr<Block> block;
  if (!Block::Open(std::move(bytes), &block).ok())
    return;
  Scan(*block);
  for (const char *t : {"", "a", "app", "apple", "b", "zzz"}) {
    BlockIterator it(block.get(), CompareByteWise);
    it.Seek(t);
    if (it.Valid())
      CheckInside(*block, it.get_value());
  }
}

TEST(BlockCorruptionTest, EveryBitFlipIsSafe) {
  Items big;
  for (int i = 0; i < 100; ++i)
    big.emplace_back(MakeKey(2 * i), "value" + std::to_string(i));
  for (const std::string &good : {Block39(), BuildBlock(4, big)}) {
    for (size_t byte = 0; byte < good.size(); ++byte) {
      for (int bit = 0; bit < 8; ++bit) {
        std::string bad = good;
        bad[byte] = static_cast<char>(bad[byte] ^ (1 << bit));
        Exercise(std::move(bad));
      }
    }
  }
}
TEST(BlockOracleTest, RandomKeysMatchStdMap) {
  static const char kAlphabet[] = {'a', 'b', '\0', 'c'}; // includes a zero byte
  for (uint32_t seed : {1u, 2u, 3u, 42u, 12345u}) {
    SCOPED_TRACE("seed=" +
                 std::to_string(seed)); // reproduce failures with this
    std::mt19937 rng(seed);
    auto random_key = [&] {
      std::string k;
      size_t len =
          rng() % 12; // short keys over a tiny alphabet -> long shared prefixes
      for (size_t i = 0; i < len; ++i)
        k.push_back(kAlphabet[rng() % 4]);
      return k;
    };

    std::map<std::string, std::string> model;
    for (int i = 0; i < 500; ++i)
      model[random_key()] = "v" + std::to_string(rng() % 1000);
    Items items(model.begin(), model.end());

    for (size_t interval : {1, 2, 4, 16, 64}) {
      SCOPED_TRACE("interval=" + std::to_string(interval));
      auto block = OpenOk(BuildBlock(interval, items));
      ASSERT_TRUE(block != nullptr);
      for (int q = 0; q < 3000; ++q) {
        std::string target = random_key();
        BlockIterator it(block.get(), CompareByteWise);
        it.Seek(target);
        auto want = model.lower_bound(target);
        if (want == model.end()) {
          EXPECT_FALSE(it.Valid()) << Hex(target);
          EXPECT_TRUE(it.get_status().ok());
        } else {
          ASSERT_TRUE(it.Valid()) << "target " << Hex(target);
          EXPECT_EQ(std::string(it.get_key()), want->first);
          EXPECT_EQ(std::string(it.get_value()), want->second);
        }
      }
    }
  }
}
