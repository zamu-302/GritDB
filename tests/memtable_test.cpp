#include "dbformat.h"
#include "memtable.h"
#include <gtest/gtest.h>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace gritdb;
// helper
namespace {
std::pair<LookUpResult, std::string>
Lookup(const MemTable &m, std::string_view key, uint64_t snapshot) {
  std::string v;
  LookUpResult r = m.Get(key, snapshot, &v);
  return {r, v};
}
} // namespace

TEST(MemTableTest, ForEachTest) {
  MemTable table;
  table.Add(4, "b", ValueType::Value, "b4");
  table.Add(1, "a", ValueType::Value, "a1");
  table.Add(5, "a", ValueType::Value, "a5");
  table.Add(3, "a", ValueType::Value, "a3");
  struct Seen {
    std::string key;
    uint64_t seq;
    ValueType type;
    std::string value;
  };
  std::vector<Seen> seen;
  table.ForEach([&](std::string_view key, uint64_t seq, ValueType type,
                    std::string_view value) {
    // copy into strings: the views are only valid during the callback
    seen.push_back({std::string(key), seq, type, std::string(value)});
  });

  // user key ascending, then sequence descending
  ASSERT_EQ(seen.size(), 4u);
  EXPECT_EQ(seen[0].key, "a");
  EXPECT_EQ(seen[0].seq, 5u);
  EXPECT_EQ(seen[0].value, "a5");
  EXPECT_EQ(seen[1].key, "a");
  EXPECT_EQ(seen[1].seq, 3u);
  EXPECT_EQ(seen[1].value, "a3");
  EXPECT_EQ(seen[2].key, "a");
  EXPECT_EQ(seen[2].seq, 1u);
  EXPECT_EQ(seen[2].value, "a1");
  EXPECT_EQ(seen[3].key, "b");
  EXPECT_EQ(seen[3].seq, 4u);
  EXPECT_EQ(seen[3].value, "b4");
  EXPECT_EQ(seen[0].type, ValueType::Value);
}
TEST(MemTableTest, DeleteHidesOlderValue) {
  MemTable m;
  m.Add(1, "a", ValueType::Value, "old");
  m.Add(2, "a", ValueType::Deletion, "");

  EXPECT_EQ(Lookup(m, "a", 2).first, LookUpResult::Deleted);
  auto old = Lookup(m, "a", 1);
  EXPECT_EQ(old.first, LookUpResult::Found);
  EXPECT_EQ(old.second, "old");
  EXPECT_EQ(Lookup(m, "a", kMaxSequence).first,
            LookUpResult::Deleted); // "latest"
}

// a miss never writes *value
TEST(MemTableTest, MissAfterLowerBound) {
  MemTable m;
  m.Add(1, "b", ValueType::Value, "x");

  std::string v = "untouched";
  EXPECT_EQ(m.Get("zzz", 10, &v),
            LookUpResult::NotFound);                     // past the last entry
  EXPECT_EQ(m.Get("a", 10, &v), LookUpResult::NotFound); // lands on "b"
  EXPECT_EQ(m.Get("a", 0, &v), LookUpResult::NotFound);  // lands on "b"
  EXPECT_EQ(m.Get("b", 0, &v), LookUpResult::NotFound);
  EXPECT_EQ(v, "untouched");
}

// weird keys and values.
TEST(MemTableTest, AwkwardKeysAndValues) {
  MemTable m;
  const std::string nul_key("a\0", 2);
  m.Add(1, "a", ValueType::Value, "1");
  m.Add(2, "ab", ValueType::Value, "2");
  m.Add(3, nul_key, ValueType::Value, "3");
  m.Add(4, "", ValueType::Value, "empty-key");
  m.Add(5, "e", ValueType::Value, ""); // empty value
  m.Add(kMaxSequence, "max", ValueType::Value, "m");

  auto expect_found = [&](std::string_view key, const std::string &want) {
    auto r = Lookup(m, key, kMaxSequence);
    EXPECT_EQ(r.first, LookUpResult::Found) << "key size " << key.size();
    EXPECT_EQ(r.second, want);
  };
  expect_found("a", "1");
  expect_found("ab", "2");
  expect_found(nul_key, "3"); // distinct from "a"
  expect_found("", "empty-key");
  expect_found("e", "");
  expect_found("max", "m");
}

TEST(MemTableTest, MemoryUsageGrowsPerEntry) {
  MemTable m;
  EXPECT_EQ(m.ApproximateMemoryUsage(), 0u);

  m.Add(1, "key", ValueType::Value, "value");
  const size_t first =
      (3 + 8) + 5 + kEntryOverhead; // internal key = user key + 8-byte tag
  EXPECT_EQ(m.ApproximateMemoryUsage(), first);

  m.Add(2, "key", ValueType::Deletion, "");
  const size_t second = (3 + 8) + 0 + kEntryOverhead;
  EXPECT_EQ(m.ApproximateMemoryUsage(), first + second);
  EXPECT_EQ(m.Count(), 2u);
}

namespace {
struct OracleEntry {
  std::string key;
  uint64_t seq;
  ValueType type;
  std::string value;
};

LookUpResult OracleGet(const std::vector<OracleEntry> &all,
                       const std::string &key, uint64_t snapshot,
                       std::string *value) {
  const OracleEntry *best = nullptr;
  for (const auto &e : all) {
    if (e.key == key && e.seq <= snapshot &&
        (best == nullptr || e.seq > best->seq)) {
      best = &e;
    }
  }
  if (best == nullptr)
    return LookUpResult::NotFound;
  if (best->type == ValueType::Deletion)
    return LookUpResult::Deleted;
  *value = best->value;
  return LookUpResult::Found;
}
} // namespace

TEST(MemTableTest, MatchesBruteForceOracle) {
  std::mt19937 rng(12345); // fixed seed: a failure replays exactly
  const std::vector<std::string> keys = {"",  "a", "ab", std::string("a\0", 2),
                                         "b", "zz"};

  MemTable m;
  std::vector<OracleEntry> all;
  uint64_t seq = 0;

  for (int i = 0; i < 5000; ++i) {
    const std::string &key = keys[rng() % keys.size()];
    if (rng() % 2 == 0) { // write
      ++seq;
      const bool is_delete = (rng() % 4 == 0);
      const ValueType type = is_delete ? ValueType::Deletion : ValueType::Value;
      const std::string value = is_delete ? "" : "v" + std::to_string(seq);
      m.Add(seq, key, type, value);
      all.push_back({key, seq, type, value});
    } else { // read at a random snapshot: 0, old, current, and one past the
             // newest
      const uint64_t snapshot = rng() % (seq + 2);
      std::string got, want;
      const LookUpResult g = m.Get(key, snapshot, &got);
      const LookUpResult w = OracleGet(all, key, snapshot, &want);
      ASSERT_EQ(g, w) << "op " << i << ", key size " << key.size()
                      << ", snapshot " << snapshot;
      if (w == LookUpResult::Found) {
        ASSERT_EQ(got, want) << "op " << i << ", snapshot " << snapshot;
      }
    }
  }
}
