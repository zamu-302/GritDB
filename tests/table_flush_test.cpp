// MemTable -> SSTable flush: the table must answer every lookup exactly like
// the memtable did.
//
// This is the test that proves the pieces fit together: the memtable's ForEach
// yields (user_key, sequence, type, value); we rebuild internal keys, write
// them with TableBuilder using CompareInternalKey, then ask both structures the
// same questions.
#include <random>

#include "test_util.h"

using namespace gritdb;
using namespace gritdb::tu;

namespace {

std::string RandomString(std::mt19937_64 &rng, size_t max_len) {
  static const char alphabet[] = {'a', 'b', 'c', '\0', '\xff'};
  size_t len = rng() % (max_len + 1);
  std::string s;
  for (size_t i = 0; i < len; ++i)
    s.push_back(alphabet[rng() % sizeof alphabet]);
  return s;
}

} // namespace

TEST(TableFlush, TableAgreesWithMemTableOnEveryLookup) {
  for (uint64_t seed : {1, 2, 3, 42})
    for (size_t bs : {size_t{1}, size_t{64}, size_t{4096}}) {
      SCOPED_TRACE("seed " + std::to_string(seed) + " block_size " +
                   std::to_string(bs));
      std::mt19937_64 rng(seed);

      // 1. Fill a memtable with overwrites and deletions over a small key
      // space.
      MemTable mt; // CHECK: constructor arguments (comparator? arena?)
      std::vector<std::string> users;
      for (int i = 0; i < 30; ++i)
        users.push_back(RandomString(rng, 6));
      uint64_t seq = 1;
      for (int op = 0; op < 400; ++op) {
        const std::string &u = users[rng() % users.size()];
        bool del = rng() % 4 == 0;
        // CHECK: your real argument order. Per your design it is (sequence,
        // key, type, value).
        mt.Add(seq++, u, del ? ValueType::Deletion : ValueType::Value,
               del ? std::string() : RandomString(rng, 40));
      }
      const uint64_t last_seq = seq - 1;

      // 2. "Flush": walk the memtable, rebuild internal keys, write a table.
      Items items;
      mt.ForEach([&](std::string_view user, uint64_t sequence, ValueType type,
                     std::string_view value) {
        items.emplace_back(IKey(user, sequence, type), std::string(value));
      });
      ASSERT_FALSE(items.empty());
      for (size_t i = 1; i < items.size(); ++i)
        ASSERT_LT(CompareInternalKey(items[i - 1].first, items[i].first), 0)
            << "ForEach must visit in internal-key order"; // CHECK:
                                                           // CompareInternalKey
                                                           // signature

      Options o = TestOptions(bs, CompareInternalKey);
      std::string bytes = BuildTable(items, o);
      Env env;
      std::unique_ptr<Table> t;
      ASSERT_TRUE(OpenFromBytes(env, bytes, o, &t).ok());

      // 3. Ask both the same questions: every key we used, some we never used,
      // every snapshot.
      std::vector<std::string> probes = users;
      for (int i = 0; i < 20; ++i)
        probes.push_back(RandomString(rng, 6));
      const uint64_t snaps[] = {0, 1, last_seq / 2, last_seq, kMaxSequence};
      for (const auto &u : probes)
        for (uint64_t snap : snaps) {
          std::string mv, tv;
          LookUpResult want = mt.Get(u, snap, &mv);
          LookUpResult got = TableLookup(*t, u, snap, &tv);
          ASSERT_EQ(got, want) << "user " << Hex(u) << " snapshot " << snap;
          if (want == LookUpResult::Found)
            ASSERT_EQ(tv, mv) << "user " << Hex(u);
        }
    }
}

TEST(TableFlush, EmptyMemTableMakesAnEmptyButValidTable) {
  MemTable mt;
  Items items;
  mt.ForEach(
      [&](std::string_view u, uint64_t s, ValueType t, std::string_view v) {
        items.emplace_back(IKey(u, s, t), std::string(v));
      });
  EXPECT_TRUE(items.empty());
  Options o = TestOptions(4096, CompareInternalKey);
  std::string bytes = BuildTable(items, o);
  Env env;
  std::unique_ptr<Table> t;
  ASSERT_TRUE(OpenFromBytes(env, bytes, o, &t).ok());
  std::string v;
  EXPECT_EQ(TableLookup(*t, "anything", kMaxSequence, &v),
            LookUpResult::NotFound);
}
