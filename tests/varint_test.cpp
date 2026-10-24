#include "../include/coding.h"
#include <gtest/gtest.h>
using namespace gritdb;

TEST(VarintTest, Varint64RoundTrip) {
  for (uint64_t v : {0ull, 127ull, 128ull, 0xFFFFFFFFull, 0x100000000ull,
                     1ull << 63, 0xFFFFFFFFFFFFFFFFull}) {
    std::string s;
    PutVarint64(&s, v);
    std::string_view in(s);
    uint64_t out = 0;
    ASSERT_TRUE(GetVarint64(&in, &out));
    EXPECT_EQ(out, v);
    EXPECT_TRUE(in.empty());
  }
}

TEST(VarintTest, Varint64MaxIsTenBytes) {
  std::string s;
  PutVarint64(&s, UINT64_MAX);
  ASSERT_EQ(s.size(), 10u);
  EXPECT_EQ(static_cast<uint8_t>(s[9]), 0x01);
}

TEST(VarintTest, Varint64OverflowRejected) {
  std::string s(9, '\xFF');
  s.push_back('\x02');
  std::string_view in(s);
  uint64_t out = 0;
  EXPECT_FALSE(GetVarint64(&in, &out));
}

TEST(VarintTest, Varint64TooLongRejected) {
  std::string s(10, '\x80');
  s.push_back('\x01');
  std::string_view in(s);
  uint64_t out = 0;
  EXPECT_FALSE(GetVarint64(&in, &out));
}

TEST(VarintTest, FailureLeavesInputUntouched) {
  std::string s;
  PutVarint64(&s, 300);
  std::string_view in(s.data(), 1);
  std::string_view before = in;
  uint64_t out = 0;
  EXPECT_FALSE(GetVarint64(&in, &out));
  EXPECT_EQ(in.data(), before.data());
  EXPECT_EQ(in.size(), before.size());
}
