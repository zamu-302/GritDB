#include "mem_filesystem.h"
#include "memtable.h"
#include "wal.h"
#include "write_batch.h"
#include <gtest/gtest.h>
#include <map>
#include <random>

using namespace gritdb;

TEST(WalReplayTest, ReplayedLogMatchesModel) {
  std::mt19937 rng(2024); // fixed seed: a failure replays exactly
  MemFileSystem fs;
  std::unique_ptr<WritableFile> file;
  ASSERT_TRUE(fs.NewWritableFile("wal.log", &file).ok());
  LogWriter log(std::move(file));

  std::vector<std::string> keys = {""};
  for (int i = 0; i < 20; ++i)
    keys.push_back("k" + std::to_string(i));

  std::map<std::string, std::string> model; // the expected final state
  uint64_t next_seq = 1;
  const int kBatches = 1000;

  for (int b = 0; b < kBatches; ++b) {
    WriteBatch batch;
    const int n = 1 + rng() % 5;
    for (int j = 0; j < n; ++j) {
      const std::string &key = keys[rng() % keys.size()];
      if (rng() % 4 == 0) {
        batch.Delete(key);
        model.erase(key);
      } else {
        const size_t len = (rng() % 50 == 0) ? 40000 : rng() % 200;
        const std::string value(len, static_cast<char>('a' + rng() % 26));
        batch.Put(key, value);
        model[key] = value;
      }
    }
    batch.SetSequence(next_seq);
    next_seq += batch.Count();
    ASSERT_TRUE(log.AddRecord(batch.Contents()).ok());
  }
  ASSERT_TRUE(log.Close().ok());

  std::string bytes;
  ASSERT_TRUE(ReadFileToString(&fs, "wal.log", &bytes).ok());
  LogReader reader(bytes);
  MemTable recovered;
  std::string record;
  size_t replayed = 0;
  uint64_t last_seq = 0;
  while (reader.ReadRecord(&record)) {
    WriteBatch batch;
    batch.SetContents(record);
    ASSERT_TRUE(batch.InsertInto(&recovered).ok());
    last_seq = batch.Sequence() + batch.Count() - 1;
    ++replayed;
  }
  ASSERT_TRUE(reader.status().ok());
  EXPECT_EQ(replayed, static_cast<size_t>(kBatches));
  EXPECT_EQ(last_seq,
            next_seq - 1); // same sequence numbers as before the "crash"

  for (const auto &key : keys) {
    std::string got;
    const LookUpResult r = recovered.Get(key, kMaxSequence, &got);
    auto it = model.find(key);
    if (it == model.end()) {
      EXPECT_NE(r, LookUpResult::Found) << "key '" << key << "'";
    } else {
      ASSERT_EQ(r, LookUpResult::Found) << "key '" << key << "'";
      EXPECT_TRUE(got == it->second)
          << "key '" << key << "'"; // avoids dumping 40 KB on failure
    }
  }
}
