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
  // Decreasing / equal offsets need 3+ restarts: interval 1 gives 4 restarts
  // at offsets 0,7,16,25; the array starts at byte 30, so restart[2] is at 38.
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
    Scan(*block); // a prefix may validate by accident; it must still be safe
    for (const char *t : {"", "apple", "zzz"}) {
      BlockIterator it(block.get(), CompareByteWise);
      it.Seek(t);
    }
  }
}
