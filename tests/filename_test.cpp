#include "filename.h"
#include <cstdint>
#include <gtest/gtest.h>
using namespace gritdb;
TEST(FileNameTest, ValidFileName) {
  uint64_t expectednumber = 5;
  FileType expected_type = FileType::kLog;

  uint64_t number;
  FileType type;
  std::string dir = "000005.log";
  ParseFileName(dir, &number, &type);
  EXPECT_EQ(expectednumber, number);
  EXPECT_EQ(expected_type, type);
}
