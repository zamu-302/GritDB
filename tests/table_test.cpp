// Tests for TableBuilder + Table (the SSTable file as a whole).
//
// Reading guide (top to bottom):
//   1. Golden vectors     - exact bytes, worked out by hand. If these pass, the
//   format is what the
//                           docs say it is.
//   2. Layout invariants  - blocks are contiguous, index keys are right, empty
//   table size.
//   3. Round trips        - every count x block size x verify combination reads
//   back correctly.
//   4. Awkward data       - empty keys/values, zero and 0xFF bytes, huge
//   values.
//   5. Internal keys      - versions and tombstones through TableLookup.
//   6. Oracle tests       - random data checked against std::map / a linear
//   scan.
//   7. Corruption         - truncation, byte flips, lying sizes, hand-assembled
//   bad tables.
//   8. I/O errors         - failures from the file must surface as IOError,
//   never crash.
#include "test_util.h"
#include <algorithm>
#include <map>
#include <random>

using namespace gritdb;
using namespace gritdb::tu;

namespace {

Options Opts(size_t block_size, bool verify = true) {
  Options o = TestOptions(block_size);
  o.verify_checksums = verify;
  return o;
}

std::string Bytes(std::initializer_list<int> v) {
  std::string s;
  for (int b : v)
    s.push_back(static_cast<char>(b));
  return s;
}

// The trailer for `contents`, with the CRC computed independently of
// MakeTrailer.
std::string ExpectedTrailer(const std::string &contents) {
  std::string with_type = contents + std::string(1, '\0');
  uint32_t crc = crc32c::Value(with_type.data(), with_type.size());
  std::string t(1, '\0');
  char buf[4];
  EncodeFixed32(buf, crc);
  t.append(buf, 4);
  return t;
}

} // namespace

// =========================================================================
// 1. Golden vectors
// =========================================================================

// Two items, block_size 1 so that every Add flushes its own data block.
//
//   block0 (13 bytes): 00 01 01 'a' '1' | 00 00 00 00 | 01 00 00 00
//           entry: shared=0 non_shared=1 value_len=1 key=a value=1;
//           restart[0]=0; count=1
//   trailer0 (5)     : offset 13..17
//   block1 (13 bytes): same with 'b','2'                 offset 18..30,
//   trailer 31..35 index (52 bytes) : offset 36
//           entry0: 00 01 10 'a' + handle{0,13}
//           entry1: 00 01 10 'b' + handle{18,13}         (interval 1 -> shared
//           is always 0) restarts 0, 20 (=0x14); count 2
//   trailer(index)   : offset 88..92
//   footer (40)      : offset 93..132, index handle {36,52}
//   total 133
TEST(TableGolden, TwoEntriesBlockSizeOne) {
  Items items = {{"a", "1"}, {"b", "2"}};
  std::string got = BuildTable(items, Opts(1));

  std::string block0 =
      Bytes({0x00, 0x01, 0x01, 'a', '1', 0, 0, 0, 0, 1, 0, 0, 0});
  std::string block1 =
      Bytes({0x00, 0x01, 0x01, 'b', '2', 0, 0, 0, 0, 1, 0, 0, 0});

  std::string index;
  index += Bytes({0x00, 0x01, 0x10, 'a'});
  index += Bytes({0, 0, 0, 0, 0, 0, 0, 0, 13, 0, 0, 0, 0, 0, 0, 0}); // {0, 13}
  index += Bytes({0x00, 0x01, 0x10, 'b'});
  index +=
      Bytes({18, 0, 0, 0, 0, 0, 0, 0, 13, 0, 0, 0, 0, 0, 0, 0}); // {18, 13}
  index += Bytes({0, 0, 0, 0, 0x14, 0, 0, 0}); // restarts 0, 20
  index += Bytes({2, 0, 0, 0});                // count
  ASSERT_EQ(index.size(), 52u);

  std::string footer;
  footer += Bytes({36, 0, 0, 0, 0, 0, 0, 0, 52, 0, 0, 0, 0, 0, 0, 0});
  footer += std::string(16, '\0');
  footer += Bytes({0x8B, 0x6A, 0x40, 0x0C, 0xFC, 0x7D, 0x41, 0x7B});

  std::string want = block0 + ExpectedTrailer(block0) + block1 +
                     ExpectedTrailer(block1) + index + ExpectedTrailer(index) +
                     footer;
  ASSERT_EQ(want.size(), 133u);
  ASSERT_EQ(got.size(), want.size());
  EXPECT_EQ(got, want) << "first difference at byte " << FirstDiff(got, want);
}

// 10 items k000000, k000002 ... k000018 with values value0..value9,
// block_size 64. Each entry costs 16 bytes (first) or 10 bytes (shares 6 key
// bytes), plus 4*restarts+4: 5 entries -> 16 + 4*10 = 56, +4 (one restart) +4
// (count) = 64. So 2 data blocks of 64 bytes.
//   data0 [0,69)  data1 [69,138)  index: 2 entries * 26 + 8 + 4 = 64 at 138,
//   [138,207) footer [207,247)   total 247
TEST(TableGolden, TenEntriesLayout) {
  Items items = MakeItems(10);
  std::string bytes = BuildTable(items, Opts(64));
  ASSERT_EQ(bytes.size(), 247u);

  Layout L = ParseLayout(bytes);
  ASSERT_TRUE(L.ok);
  ASSERT_EQ(L.data.size(), 2u);
  EXPECT_EQ(L.data[0].handle.offset, 0u);
  EXPECT_EQ(L.data[0].handle.size, 64u);
  EXPECT_EQ(L.data[1].handle.offset, 69u);
  EXPECT_EQ(L.data[1].handle.size, 64u);
  EXPECT_EQ(L.index.handle.offset, 138u);
  EXPECT_EQ(L.index.handle.size, 64u);
  EXPECT_EQ(L.footer.begin, 207u);
  ASSERT_EQ(L.last_keys.size(), 2u);
  EXPECT_EQ(L.last_keys[0], MakeKey(8));  // 5th item
  EXPECT_EQ(L.last_keys[1], MakeKey(18)); // 10th item
}

// =========================================================================
// 2. Layout invariants
// =========================================================================

TEST(TableLayout, EmptyTableIsFiftyThreeBytes) {
  // index block with no entries: restart[0]=0 + count=1 -> 8 bytes; + 5 trailer
  // + 40 footer.
  std::string bytes = BuildTable({}, Opts(64));
  EXPECT_EQ(bytes.size(), 53u);
  Layout L = ParseLayout(bytes);
  ASSERT_TRUE(L.ok);
  EXPECT_TRUE(L.data.empty());
  EXPECT_EQ(L.index.begin, 0u);
  EXPECT_EQ(L.index.handle.size, 8u);
}

TEST(TableLayout, RegionsAreContiguousAndFillTheFile) {
  for (size_t block_size :
       {size_t{1}, size_t{64}, size_t{1000}, size_t{4096}}) {
    SCOPED_TRACE("block_size " + std::to_string(block_size));
    Items items = MakeItems(300, 2, 20);
    std::string bytes = BuildTable(items, Opts(block_size));
    Layout L = ParseLayout(bytes);
    ASSERT_TRUE(L.ok);
    size_t pos = 0;
    for (const Region &r : L.data) {
      EXPECT_EQ(r.begin, pos);
      pos = r.end;
    }
    EXPECT_EQ(L.index.begin, pos);
    EXPECT_EQ(L.footer.begin, L.index.end);
    EXPECT_EQ(L.footer.end, bytes.size());
    // Every data block's last key is its true last key: strictly increasing,
    // last = last item.
    for (size_t i = 1; i < L.last_keys.size(); ++i)
      EXPECT_LT(L.last_keys[i - 1], L.last_keys[i]);
    EXPECT_EQ(L.last_keys.back(), items.back().first);
  }
}

TEST(TableLayout, BiggerBlockSizeMeansFewerBlocks) {
  Items items = MakeItems(500, 2, 30);
  size_t prev = SIZE_MAX;
  for (size_t bs :
       {size_t{64}, size_t{256}, size_t{1024}, size_t{4096}, size_t{1} << 20}) {
    Layout L = ParseLayout(BuildTable(items, Opts(bs)));
    ASSERT_TRUE(L.ok);
    EXPECT_LE(L.data.size(), prev);
    prev = L.data.size();
  }
  EXPECT_EQ(prev, 1u); // 1 MiB holds all 500 entries
}

TEST(TableLayout, BlocksAreSizedNearTheTarget) {
  // A block is flushed as soon as it reaches block_size, so a block is at most
  // block_size + one entry. Only the last data block may be smaller than
  // block_size.
  const size_t bs = 512;
  Items items = MakeItems(1000, 2, 40);
  Layout L = ParseLayout(BuildTable(items, Opts(bs)));
  ASSERT_TRUE(L.ok);
  for (size_t i = 0; i + 1 < L.data.size(); ++i) {
    EXPECT_GE(L.data[i].handle.size, bs);
    EXPECT_LT(L.data[i].handle.size, bs + 100);
  }
}

TEST(TableLayout, BuilderIsDeterministic) {
  Items items = MakeItems(200, 2, 15);
  EXPECT_EQ(BuildTable(items, Opts(256)), BuildTable(items, Opts(256)));
}

// =========================================================================
// 3. Round trips
// =========================================================================

TEST(TableRoundTrip, CountsBlockSizesAndVerify) {
  for (int n : {0, 1, 10, 1000})
    for (size_t bs : {size_t{1}, size_t{64}, size_t{4096}, size_t{1} << 20})
      for (bool verify : {true, false}) {
        SCOPED_TRACE("n=" + std::to_string(n) + " block_size=" +
                     std::to_string(bs) + " verify=" + std::to_string(verify));
        Items items = MakeItems(n);
        Options o = Opts(bs, verify);
        std::string bytes = BuildTable(items, o);
        Env env;
        std::unique_ptr<Table> t;
        ASSERT_TRUE(OpenFromBytes(env, bytes, o, &t).ok());
        ExpectAllReads(*t, items);
      }
}

TEST(TableRoundTrip, RestartIntervalsOfTheBlocks) {
  Items items = MakeItems(200, 2, 10);
  for (size_t interval : {size_t{1}, size_t{2}, size_t{16}, size_t{1000}}) {
    SCOPED_TRACE("interval " + std::to_string(interval));
    Options o = TestOptions(256, CompareByteWise, interval);
    std::string bytes = BuildTable(items, o);
    Env env;
    std::unique_ptr<Table> t;
    ASSERT_TRUE(OpenFromBytes(env, bytes, o, &t).ok());
    ExpectAllReads(*t, items);
  }
}

TEST(TableRoundTrip, TableOutlivesNothingItNeedsToKeep) {
  // The Table must own what it needs: build, open, then let the byte string
  // die.
  Items items = MakeItems(50);
  Env env;
  std::unique_ptr<Table> t;
  {
    std::string bytes = BuildTable(items, Opts(64));
    ASSERT_TRUE(OpenFromBytes(env, bytes, Opts(64), &t).ok());
  }
  ExpectAllReads(*t, items);
}

TEST(TableRoundTrip, RepeatedReadsGiveTheSameAnswer) {
  Items items = MakeItems(100);
  Env env;
  std::unique_ptr<Table> t;
  std::string bytes = BuildTable(items, Opts(128));
  ASSERT_TRUE(OpenFromBytes(env, bytes, Opts(128), &t).ok());
  for (int pass = 0; pass < 3; ++pass)
    ExpectAllReads(*t, items);
}

// =========================================================================
// 4. Awkward data
// =========================================================================

TEST(TableAwkward, EmptyKeyAndEmptyValues) {
  Items items = {{"", ""}, {"a", ""}, {"b", "x"}};
  for (size_t bs : {size_t{1}, size_t{4096}}) {
    std::string bytes = BuildTable(items, Opts(bs));
    Env env;
    std::unique_ptr<Table> t;
    ASSERT_TRUE(OpenFromBytes(env, bytes, Opts(bs), &t).ok());
    ExpectGet(*t, "", "", "");
    ExpectGet(*t, "a", "a", "");
    ExpectGet(*t, "b", "b", "x");
    ExpectAllReads(*t, items);
  }
}

TEST(TableAwkward, ZeroAndHighBytesInKeysAndValues) {
  Items items = {
      {std::string("\0", 1), std::string("\0\0", 2)},
      {std::string("\0\0", 2), std::string("\xff\xff\xff", 3)},
      {std::string("a\0b", 3), std::string("v\0", 2)},
      {std::string("a\0c", 3), std::string("\x80\x7f", 2)},
      {std::string("\xff", 1), std::string("last")},
      {std::string("\xff\xff", 2), std::string("last2")},
  };
  for (size_t bs : {size_t{1}, size_t{64}, size_t{4096}}) {
    SCOPED_TRACE("bs " + std::to_string(bs));
    std::string bytes = BuildTable(items, Opts(bs));
    Env env;
    std::unique_ptr<Table> t;
    ASSERT_TRUE(OpenFromBytes(env, bytes, Opts(bs), &t).ok());
    for (const auto &kv : items)
      ExpectGet(*t, kv.first, kv.first, kv.second);
    ExpectAllReads(*t, items);
  }
}

TEST(TableAwkward, OneHugeValueGetsItsOwnOversizedBlock) {
  // A 1 MiB value cannot be split. The block holding it just becomes huge (the
  // builder only checks the size *after* adding), so the offsets must still be
  // exact and 64-bit safe.
  std::string big(1 << 20, 'x');
  big[0] = 'A';
  big.back() = 'Z';
  Items items = {{"a", "small"}, {"b", big}, {"c", "small2"}};
  std::string bytes = BuildTable(items, Opts(4096));
  Layout L = ParseLayout(bytes);
  ASSERT_TRUE(L.ok);
  ASSERT_EQ(L.data.size(), 2u);
  EXPECT_EQ(L.data[0].handle.size,
            (1u << 20) + 23u); // a,b together (see the Python model)
  Env env;
  std::unique_ptr<Table> t;
  ASSERT_TRUE(OpenFromBytes(env, bytes, Opts(4096), &t).ok());
  ExpectGet(*t, "b", "b", big);
  ExpectGet(*t, "c", "c", "small2");
  ExpectAllReads(*t, items);
}

TEST(TableAwkward, LongSharedPrefixes) {
  Items items;
  std::string prefix(300,
                     'p'); // longer than a varint byte, forces 2-byte varints
  for (int i = 0; i < 50; ++i)
    items.emplace_back(prefix + MakeKey(i), MakeValue(i));
  std::string bytes = BuildTable(items, Opts(512));
  Env env;
  std::unique_ptr<Table> t;
  ASSERT_TRUE(OpenFromBytes(env, bytes, Opts(512), &t).ok());
  for (const auto &kv : items)
    ExpectGet(*t, kv.first, kv.first, kv.second);
  ExpectAllReads(*t, items);
}

TEST(TableAwkward, SingleEntryTable) {
  Items items = {{"only", "one"}};
  std::string bytes = BuildTable(items, Opts(4096));
  Env env;
  std::unique_ptr<Table> t;
  ASSERT_TRUE(OpenFromBytes(env, bytes, Opts(4096), &t).ok());
  ExpectGet(*t, "", "only", "one");
  ExpectGet(*t, "only", "only", "one");
  ExpectGet(*t, "a", "only", "one");
  ExpectNotFound(*t, "onlyz");
  ExpectNotFound(*t, "p");
}

// =========================================================================
// 5. Internal keys: versions and tombstones
// =========================================================================

TEST(TableInternalKeys, VersionsAndTombstones) {
  // Internal order: user key ascending, then sequence DESCENDING (newest
  // first).
  Items items = {
      {IKey("a", 9, ValueType::Value), "a9"},
      {IKey("a", 7, ValueType::Deletion), ""},
      {IKey("a", 3, ValueType::Value), "a3"},
      {IKey("b", 4, ValueType::Value), "b4"},
  };
  for (size_t bs : {size_t{1}, size_t{64}, size_t{4096}}) {
    SCOPED_TRACE("bs " + std::to_string(bs));
    Options o = TestOptions(bs, CompareInternalKey);
    std::string bytes = BuildTable(items, o);
    Env env;
    std::unique_ptr<Table> t;
    ASSERT_TRUE(OpenFromBytes(env, bytes, o, &t).ok());

    auto lookup = [&](std::string_view user, uint64_t snap, std::string *v) {
      return TableLookup(*t, user, snap, v);
    };
    std::string v;
    EXPECT_EQ(lookup("a", kMaxSequence, &v), LookUpResult::Found);
    EXPECT_EQ(v, "a9");
    EXPECT_EQ(lookup("a", 10, &v), LookUpResult::Found);
    EXPECT_EQ(v, "a9");
    EXPECT_EQ(lookup("a", 9, &v), LookUpResult::Found); // snapshot is inclusive
    EXPECT_EQ(v, "a9");
    EXPECT_EQ(lookup("a", 8, &v), LookUpResult::Deleted);
    EXPECT_EQ(lookup("a", 7, &v), LookUpResult::Deleted);
    EXPECT_EQ(lookup("a", 6, &v),
              LookUpResult::Found); // tombstone at 7 is invisible
    EXPECT_EQ(v, "a3");
    EXPECT_EQ(lookup("a", 3, &v), LookUpResult::Found);
    EXPECT_EQ(v, "a3");
    EXPECT_EQ(lookup("a", 2, &v),
              LookUpResult::NotFound); // lands on b's entry -> wrong user
    EXPECT_EQ(lookup("a", 0, &v), LookUpResult::NotFound);
    EXPECT_EQ(lookup("b", 4, &v), LookUpResult::Found);
    EXPECT_EQ(v, "b4");
    EXPECT_EQ(lookup("b", 3, &v),
              LookUpResult::NotFound); // nothing after b at all
    EXPECT_EQ(lookup("c", kMaxSequence, &v), LookUpResult::NotFound);
    EXPECT_EQ(lookup("", kMaxSequence, &v), LookUpResult::NotFound);
    EXPECT_EQ(lookup("aa", kMaxSequence, &v),
              LookUpResult::NotFound); // between a and b
  }
}

TEST(TableInternalKeys, ReturnedKeyIsTheFullInternalKey) {
  Items items = {{IKey("k", 5, ValueType::Value), "v5"}};
  Options o = TestOptions(64, CompareInternalKey);
  std::string bytes = BuildTable(items, o);
  Env env;
  std::unique_ptr<Table> t;
  ASSERT_TRUE(OpenFromBytes(env, bytes, o, &t).ok());
  std::string k, v;
  ASSERT_TRUE(t->InternalGet(IKey("k", 100, ValueType::Value), &k, &v).ok());
  EXPECT_EQ(ExtractUserKey(k), "k");
  EXPECT_EQ(TagSequence(ExtractTag(k)), 5u);
  EXPECT_EQ(TagType(ExtractTag(k)), ValueType::Value);
  EXPECT_EQ(v, "v5");
}

// =========================================================================
// 6. Oracle tests
// =========================================================================

namespace {

std::string RandomBytes(std::mt19937_64 &rng, size_t max_len) {
  static const char alphabet[] = {
      'a', 'b', 'c', '\0', '\xff'}; // tiny alphabet => many shared prefixes
  size_t len = rng() % (max_len + 1);
  std::string s;
  for (size_t i = 0; i < len; ++i)
    s.push_back(alphabet[rng() % sizeof alphabet]);
  return s;
}

} // namespace

TEST(TableOracle, PlainKeysAgainstStdMapLowerBound) {
  for (uint64_t seed : {1, 2, 3, 42}) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    std::mt19937_64 rng(seed);
    std::map<std::string, std::string> model;
    size_t n = 1 + rng() % 400;
    while (model.size() < n)
      model[RandomBytes(rng, 12)] = RandomBytes(rng, 60);

    Items items(model.begin(), model.end());
    const size_t sizes[] = {1, 32, 200, 4096};
    size_t bs = sizes[rng() % 4];
    Options o = Opts(bs);
    std::string bytes = BuildTable(items, o);
    Env env;
    std::unique_ptr<Table> t;
    ASSERT_TRUE(OpenFromBytes(env, bytes, o, &t).ok());

    std::vector<std::string> probes = ProbeKeys(items);
    for (int i = 0; i < 500; ++i)
      probes.push_back(RandomBytes(rng, 12));
    for (const auto &p : probes) {
      auto it = model.lower_bound(p);
      if (it == model.end())
        ExpectNotFound(*t, p);
      else
        ExpectGet(*t, p, it->first, it->second);
    }
  }
}

TEST(TableOracle, InternalKeysAgainstLinearScan) {
  struct E {
    std::string user;
    uint64_t seq;
    ValueType type;
    std::string value;
  };
  for (uint64_t seed : {1, 2, 3, 42}) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    std::mt19937_64 rng(seed);

    std::vector<E> es;
    std::vector<std::string> users;
    for (int i = 0; i < 40; ++i)
      users.push_back(RandomBytes(rng, 5));
    std::sort(users.begin(), users.end());
    users.erase(std::unique(users.begin(), users.end()), users.end());
    uint64_t next_seq = 1;
    for (const auto &u : users) {
      int versions = 1 + rng() % 5;
      for (int v = 0; v < versions; ++v) {
        bool del = rng() % 4 == 0;
        es.push_back({u, next_seq, del ? ValueType::Deletion : ValueType::Value,
                      del ? "" : RandomBytes(rng, 30)});
        next_seq += 1 + rng() % 3;
      }
    }
    // Internal order: user ascending, sequence descending.
    std::stable_sort(es.begin(), es.end(), [](const E &a, const E &b) {
      if (a.user != b.user)
        return a.user < b.user;
      return a.seq > b.seq;
    });

    Items items;
    for (const auto &e : es)
      items.emplace_back(IKey(e.user, e.seq, e.type), e.value);

    const size_t sizes[] = {1, 64, 4096};
    size_t bs = sizes[rng() % 3];
    Options o = TestOptions(bs, CompareInternalKey);
    std::string bytes = BuildTable(items, o);
    Env env;
    std::unique_ptr<Table> t;
    ASSERT_TRUE(OpenFromBytes(env, bytes, o, &t).ok());

    std::vector<std::string> probe_users = users;
    for (int i = 0; i < 20; ++i)
      probe_users.push_back(RandomBytes(rng, 5));
    const uint64_t snaps[] = {0, 1, next_seq / 2, next_seq, kMaxSequence};
    for (const auto &u : probe_users)
      for (uint64_t snap : snaps) {
        // The oracle: first entry in order with user > u, or user == u and seq
        // <= snap.
        LookUpResult want = LookUpResult::NotFound;
        std::string want_value;
        for (const auto &e : es) {
          if (e.user > u || (e.user == u && e.seq <= snap)) {
            if (e.user == u) {
              want = e.type == ValueType::Deletion ? LookUpResult::Deleted
                                                   : LookUpResult::Found;
              want_value = e.value;
            }
            break;
          }
        }
        std::string got_value;
        LookUpResult got = TableLookup(*t, u, snap, &got_value);
        ASSERT_EQ(got, want) << "user " << Hex(u) << " snap " << snap;
        if (want == LookUpResult::Found)
          ASSERT_EQ(got_value, want_value);
      }
  }
}

// =========================================================================
// 7. Corruption
// =========================================================================

TEST(TableCorruption, EveryTruncationIsRejected) {
  Items items = MakeItems(10);
  Options o = Opts(64);
  std::string good = BuildTable(items, o);
  for (size_t n = 0; n < good.size(); ++n) {
    SCOPED_TRACE("truncated to " + std::to_string(n));
    Env env;
    std::unique_ptr<Table> t;
    Status s = OpenFromBytes(env, std::string_view(good.data(), n), o, &t);
    ExpectCorruption(s, "truncated file");
  }
}

// For every byte of the file, flip it and check the *right* thing happens:
//   data block bytes (incl. its trailer)  -> Open fine; reads of THAT block's
//   keys are
//                                            Corruption, reads of other blocks
//                                            are still right
//   index bytes (incl. its trailer)       -> Open is Corruption (checked at
//   Open) footer: index handle or magic         -> Open is Corruption footer:
//   reserved filter handle        -> ignored for now; everything still reads
//   fine
TEST(TableCorruption, ByteFlipSweepWithChecksums) {
  Items items = MakeItems(10);
  Options o = Opts(64);
  std::string good = BuildTable(items, o);
  Layout L = ParseLayout(good);
  ASSERT_TRUE(L.ok);

  for (size_t i = 0; i < good.size(); ++i) {
    SCOPED_TRACE("flipped byte " + std::to_string(i));
    std::string bad = FlipByte(good, i);
    Env env;
    std::unique_ptr<Table> t;
    Status open = OpenFromBytes(env, bad, o, &t);

    if (i >= L.index.begin && i < L.index.end) {
      ExpectCorruption(open, "index byte");
      continue;
    }
    if (i >= L.footer.begin) {
      size_t off = i - L.footer.begin;
      bool reserved_filter_handle = off >= 16 && off < 32;
      if (!reserved_filter_handle) {
        ExpectCorruption(open, "footer byte");
        continue;
      }
    }
    ASSERT_TRUE(open.ok()) << open.ToString();

    int flipped_block = -1;
    for (size_t b = 0; b < L.data.size(); ++b)
      if (i >= L.data[b].begin && i < L.data[b].end)
        flipped_block = static_cast<int>(b);

    for (const auto &kv : items) {
      size_t block = 0;
      while (L.last_keys[block] < kv.first)
        ++block;
      std::string k, v;
      Status s = t->InternalGet(kv.first, &k, &v);
      if (static_cast<int>(block) == flipped_block) {
        ExpectCorruption(s, "key in the flipped block");
      } else {
        ASSERT_TRUE(s.ok()) << s.ToString();
        EXPECT_EQ(k, kv.first);
        EXPECT_EQ(v, kv.second);
      }
    }
  }
}

// Same sweep with checksums OFF. We can't say what the answers are (a flipped
// value byte is simply returned), only that nothing crashes, hangs, or trips
// ASan/UBSan, and that a non-OK status is always Corruption (never a wrong
// NotFound from a lying block).
TEST(TableCorruption, ByteFlipSweepWithoutChecksumsNeverCrashes) {
  Items items = MakeItems(10);
  Options o = Opts(64, /*verify=*/false);
  std::string good = BuildTable(items, o);
  std::vector<std::string> probes = ProbeKeys(items);
  for (size_t i = 0; i < good.size(); ++i) {
    for (uint8_t mask : {uint8_t{0xFF}, uint8_t{0x01}, uint8_t{0x80}}) {
      Env env;
      std::unique_ptr<Table> t;
      Status open = OpenFromBytes(env, FlipByte(good, i, mask), o, &t);
      if (!open.ok()) {
        EXPECT_TRUE(open.IsCorruption()) << open.ToString();
        continue;
      }
      for (const auto &p : probes) {
        std::string k, v;
        Status s = t->InternalGet(p, &k, &v);
        EXPECT_TRUE(s.ok() || s.IsNotFound() || s.IsCorruption())
            << s.ToString();
      }
    }
  }
}

TEST(TableCorruption, FlippedValueByteIsCaughtOnlyWhenVerifying) {
  Items items = MakeItems(10);
  std::string good = BuildTable(items, Opts(64));
  // Block 0 starts at byte 0: first entry = 00 07 06 "k000000" "value0"; value
  // starts at byte 10.
  ASSERT_EQ(good.substr(10, 6), "value0");
  std::string bad = FlipByte(good, 10, 0x01); // 'v' -> 'w'

  {
    Env env;
    std::unique_ptr<Table> t;
    ASSERT_TRUE(OpenFromBytes(env, bad, Opts(64, true), &t).ok());
    std::string k, v;
    ExpectCorruption(t->InternalGet(MakeKey(0), &k, &v), "verify on");
  }
  {
    Env env;
    std::unique_ptr<Table> t;
    ASSERT_TRUE(OpenFromBytes(env, bad, Opts(64, false), &t).ok());
    ExpectGet(*t, MakeKey(0), MakeKey(0),
              "walue0"); // the damage is returned silently
  }
}

TEST(TableCorruption, FooterHandleWithOverflowingValues) {
  Items items = MakeItems(10);
  std::string good = BuildTable(items, Opts(64));
  size_t fo = good.size() - footer_size;
  struct Case {
    uint64_t offset, size;
  } cases[] = {{UINT64_MAX, UINT64_MAX}, {UINT64_MAX - 5, 10},
               {0, UINT64_MAX},          {good.size(), 0},
               {0, good.size()},         {1ULL << 63, 1ULL << 63}};
  for (const auto &c : cases) {
    SCOPED_TRACE("offset " + std::to_string(c.offset) + " size " +
                 std::to_string(c.size));
    std::string bad =
        SetFixed64(SetFixed64(good, fo, c.offset), fo + 8, c.size);
    Env env;
    std::unique_ptr<Table> t;
    ExpectCorruption(OpenFromBytes(env, bad, Opts(64), &t), "bad index handle");
  }
}

TEST(TableCorruption, LyingFileSize) {
  Items items = MakeItems(10);
  Options o = Opts(64);
  std::string good = BuildTable(items, o);
  for (int64_t delta : {-100, -40, -1, 1, 5, 100, 100000}) {
    SCOPED_TRACE("delta " + std::to_string(delta));
    Env env;
    std::unique_ptr<Table> t;
    Status s = OpenFromBytes(env, good, o, &t,
                             static_cast<uint64_t>(good.size() + delta));
    EXPECT_FALSE(s.ok()) << "a wrong file_size must never open a table";
  }
  // Size zero, and the largest possible size.
  Env env;
  std::unique_ptr<Table> t;
  EXPECT_FALSE(OpenFromBytes(env, good, o, &t, 0).ok());
  EXPECT_FALSE(OpenFromBytes(env, good, o, &t, UINT64_MAX).ok());
}

// ---- hand-assembled tables ------------------------------------------------

namespace {

// One data block {a:1, b:2}, an index claiming `last_key` for it, and a footer.
std::string AssembleTable(const std::string &data_block_bytes,
                          const std::string &index_value,
                          const std::string &last_key = "b",
                          BlockHandle *data_handle = nullptr) {
  std::string file;
  BlockHandle dh = AppendBlock(&file, data_block_bytes);
  if (data_handle)
    *data_handle = dh;
  std::string index = BuildBlockBytes({{last_key, index_value}}, 1);
  BlockHandle ih = AppendBlock(&file, index);
  AppendFooter(&file, ih);
  return file;
}

BlockHandle Handle(uint64_t off, uint64_t size) {
  BlockHandle h;
  h.offset = off;
  h.size = size;
  return h;
}

} // namespace

TEST(TableHandAssembled, ControlCaseReadsFine) {
  std::string data = BuildBlockBytes({{"a", "1"}, {"b", "2"}}, 16);
  std::string file;
  BlockHandle dh = AppendBlock(&file, data);
  BlockHandle ih =
      AppendBlock(&file, BuildBlockBytes({{"b", EncodeHandle(dh)}}, 1));
  AppendFooter(&file, ih);

  Env env;
  std::unique_ptr<Table> t;
  ASSERT_TRUE(OpenFromBytes(env, file, Opts(4096), &t).ok());
  ExpectGet(*t, "a", "a", "1");
  ExpectGet(*t, "b", "b", "2");
  ExpectGet(*t, "aa", "b", "2");
  ExpectNotFound(*t, "c"); // past the last index key: honest NotFound
}

TEST(TableHandAssembled, IndexHandlePointsOutsideTheFile) {
  std::string data = BuildBlockBytes({{"a", "1"}, {"b", "2"}}, 16);
  for (BlockHandle bad : {Handle(1000000, 10), Handle(0, 1000000),
                          Handle(UINT64_MAX, 1), Handle(1, UINT64_MAX)}) {
    std::string file = AssembleTable(data, EncodeHandle(bad));
    Env env;
    std::unique_ptr<Table> t;
    ASSERT_TRUE(OpenFromBytes(env, file, Opts(4096), &t)
                    .ok()); // the index itself is fine
    std::string k, v;
    ExpectCorruption(t->InternalGet("a", &k, &v), "handle out of range");
  }
}

TEST(TableHandAssembled, IndexHandleValueWithWrongLength) {
  std::string data = BuildBlockBytes({{"a", "1"}, {"b", "2"}}, 16);
  std::string good_handle;
  {
    std::string f;
    good_handle = EncodeHandle(AppendBlock(&f, data));
  }
  for (size_t len :
       {size_t{0}, size_t{1}, size_t{15}, size_t{17}, size_t{32}}) {
    SCOPED_TRACE("handle value length " + std::to_string(len));
    std::string value = good_handle;
    value.resize(len, '\0');
    std::string file = AssembleTable(data, value);
    Env env;
    std::unique_ptr<Table> t;
    ASSERT_TRUE(OpenFromBytes(env, file, Opts(4096), &t).ok());
    std::string k, v;
    ExpectCorruption(t->InternalGet("a", &k, &v), "bad handle length");
  }
}

TEST(TableHandAssembled, IndexLiesAboutTheLastKeyIsCorruptionNotNotFound) {
  // The data block holds a,b. The index claims it ends at "z". A lookup for "c"
  // is sent to that block, finds nothing >= "c" in it, and the block iterator
  // is invalid with an OK status. That can only mean the index lied, so the
  // answer must be Corruption, not NotFound.
  std::string data = BuildBlockBytes({{"a", "1"}, {"b", "2"}}, 16);
  std::string f;
  BlockHandle dh = AppendBlock(&f, data);
  std::string file = AssembleTable(data, EncodeHandle(dh), /*last_key=*/"z");
  Env env;
  std::unique_ptr<Table> t;
  ASSERT_TRUE(OpenFromBytes(env, file, Opts(4096), &t).ok());
  ExpectGet(*t, "a", "a", "1"); // honest lookups still work
  std::string k, v;
  ExpectCorruption(t->InternalGet("c", &k, &v), "index lies");
}

TEST(TableHandAssembled, GarbageDataBlockWithValidChecksum) {
  // Valid CRC, invalid block structure: only Block::Open / the iterator can
  // catch this.
  std::vector<std::string> garbage = {"", "x", "garbage!!!!!",
                                      std::string(4, '\xff'),
                                      std::string("\x00\x00\x00\x00", 4)};
  for (const auto &g : garbage) {
    SCOPED_TRACE("garbage " + Hex(g));
    std::string f;
    BlockHandle dh = AppendBlock(&f, g);
    std::string file = AssembleTable(g, EncodeHandle(dh));
    Env env;
    std::unique_ptr<Table> t;
    ASSERT_TRUE(OpenFromBytes(env, file, Opts(4096), &t).ok());
    std::string k, v;
    ExpectCorruption(t->InternalGet("a", &k, &v), "garbage data block");
  }
}

TEST(TableHandAssembled, GarbageIndexBlockWithValidChecksum) {
  std::vector<std::string> garbage = {
      "", "x", "garbage!!!!!", std::string(8, '\xff'), std::string(8, '\0')};
  for (const auto &g : garbage) {
    SCOPED_TRACE("garbage " + Hex(g));
    std::string file;
    BlockHandle ih = AppendBlock(&file, g);
    AppendFooter(&file, ih);
    Env env;
    std::unique_ptr<Table> t;
    ExpectCorruption(OpenFromBytes(env, file, Opts(4096), &t), "garbage index");
  }
}

TEST(TableHandAssembled, BadMagicAndBadFooterLength) {
  std::string data = BuildBlockBytes({{"a", "1"}}, 16);
  std::string file;
  BlockHandle dh = AppendBlock(&file, data);
  BlockHandle ih =
      AppendBlock(&file, BuildBlockBytes({{"a", EncodeHandle(dh)}}, 1));
  AppendFooter(&file, ih);
  Env env;
  std::unique_ptr<Table> t;
  ASSERT_TRUE(OpenFromBytes(env, file, Opts(4096), &t).ok());
  ExpectCorruption(
      OpenFromBytes(env, FlipBit(file, file.size() - 1, 3), Opts(4096), &t),
      "magic");
  ExpectCorruption(OpenFromBytes(env, file.substr(1), Opts(4096), &t),
                   "dropped first byte");
}

// =========================================================================
// 8. I/O errors
// =========================================================================

namespace {

Status OpenWithFlakyRead(const std::string &bytes, size_t fail_at,
                         const Options &o, std::unique_ptr<Table> *out) {
  static Env
      env; // files must outlive the Table; one shared Env is fine for tests
  std::unique_ptr<RandomAccessFile> raw;
  Status s = NewRandomFile(env, bytes, &raw);
  if (!s.ok())
    return s;
  auto flaky = std::make_unique<FlakyRandomAccessFile>(std::move(raw), fail_at);
  return Table::Open(o, std::move(flaky), bytes.size(), out);
}

} // namespace

TEST(TableIoErrors, FailedFooterReadAndIndexReadFailOpen) {
  Items items = MakeItems(10);
  Options o = Opts(64);
  std::string bytes = BuildTable(items, o);
  for (size_t fail_at :
       {size_t{0}, size_t{1}}) { // read 0 = footer, read 1 = index block
    std::unique_ptr<Table> t;
    Status s = OpenWithFlakyRead(bytes, fail_at, o, &t);
    EXPECT_TRUE(s.IsIOError()) << "fail_at " << fail_at << ": " << s.ToString();
    EXPECT_EQ(t, nullptr) << "a failed Open must not hand out a Table";
  }
}

TEST(TableIoErrors, FailedDataReadFailsGetAndIsTransient) {
  Items items = MakeItems(10);
  Options o = Opts(64);
  std::string bytes = BuildTable(items, o);
  std::unique_ptr<Table> t;
  ASSERT_TRUE(OpenWithFlakyRead(bytes, /*fail_at=*/2, o, &t)
                  .ok()); // read 2 = first data read
  std::string k, v;
  Status s = t->InternalGet(MakeKey(0), &k, &v);
  EXPECT_TRUE(s.IsIOError()) << s.ToString();
  // The failure was a one-off: the same lookup again must work (no sticky state
  // in Table).
  ExpectGet(*t, MakeKey(0), MakeKey(0), MakeValue(0));
}

TEST(TableIoErrors, ShortReadIsCorruptionNotACrash) {
  // MemFileSystem::Read returns OK with fewer bytes when the range runs past
  // EOF, so lying about the file size exercises the reader's own short-read
  // check.
  Items items = MakeItems(10);
  std::string bytes = BuildTable(items, Opts(64));
  Env env;
  std::unique_ptr<Table> t;
  Status s = OpenFromBytes(env, bytes, Opts(64), &t, bytes.size() + 4096);
  EXPECT_FALSE(s.ok());
}

// Builder side: the first failed Append must make the whole build fail, and
// stay failed.
TEST(TableIoErrors, BuilderErrorIsStickyAtEveryFailurePoint) {
  Items items = MakeItems(20, 2, 10);
  Options o = Opts(64);
  std::string good = BuildTable(items, o);

  int failures = 0;
  for (size_t fail_at = 0; fail_at < 40; ++fail_at) {
    SCOPED_TRACE("fail_at " + std::to_string(fail_at));
    Env env;
    std::unique_ptr<WritableFile> inner;
    ASSERT_TRUE(env.fs.NewWritableFile(env.NextPath(), &inner).ok());
    FailingWritableFile file(std::move(inner), fail_at);

    TableBuilder b(o, &file);
    for (const auto &kv : items)
      b.Add(kv.first, kv.second);
    Status s = b.Finish();

    if (s.ok()) {
      EXPECT_TRUE(
          b.status().ok()); // no append failed: this is a successful build
    } else {
      ++failures;
      EXPECT_TRUE(s.IsIOError()) << s.ToString();
      EXPECT_FALSE(b.status().ok())
          << "status() must report the failure after Finish";
      EXPECT_TRUE(b.status().IsIOError());
    }
  }
  EXPECT_GT(failures, 0) << "fail_at=0 must fail";
  EXPECT_LT(failures, 40) << "a large fail_at must let the build succeed";
}

TEST(TableIoErrors, FailureOnVeryFirstAppendFails) {
  Env env;
  std::unique_ptr<WritableFile> inner;
  ASSERT_TRUE(env.fs.NewWritableFile(env.NextPath(), &inner).ok());
  FailingWritableFile file(std::move(inner), 0);
  TableBuilder b(Opts(1), &file);
  b.Add("a", "1");
  b.Add("b", "2");
  EXPECT_TRUE(b.Finish().IsIOError());
}
