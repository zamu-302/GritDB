#include "../include/status.h"
#include <gtest/gtest.h>

using gritdb::Status;

TEST(StatusTest, OkIsOK) {
  Status s = Status::OK();
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(s.ToString(), "OK");
}
TEST(StatusTest, DefaultOk) {
  Status s;
  EXPECT_TRUE(s.ok());
}

TEST(StatusTest, NotFoundIsOk) {
  Status s = Status::NotFound("key1");
  EXPECT_FALSE(s.ok());
  EXPECT_EQ(s.ToString(), "NotFound: key1");
}

TEST(StatusTest, Corruption) {
  Status s = Status::Corruption("bad crc.");
  EXPECT_FALSE(s.IsNotFound());
  EXPECT_TRUE(s.IsCorruption());
}

TEST(StatusTest, IOError) {
  Status s = Status::IOError("can't open folder1.");
  EXPECT_FALSE(s.IsCorruption());
  EXPECT_TRUE(s.IsIOError());
  EXPECT_EQ(s.ToString(), "IO error: can't open folder1.");
}
TEST(StatusTest, CopyHoldsState) {
  Status a = Status::Corruption("bad crc.");
  Status b = a;
  EXPECT_TRUE(b.IsCorruption());
}
