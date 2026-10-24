#include "../include/coding.h"
#include <gtest/gtest.h>
#include <string>

TEST(VarintTest, Fixed32) {
  std::string s;
  gritdb::PutFixed32(&s, 0x04030201);
  EXPECT_EQ(s[0], 0x01);
  EXPECT_EQ(s[1], 0x02);
  EXPECT_EQ(s[2], 0x03);
  EXPECT_EQ(s[3], 0x04);
}
TEST(FixedTest, Fixed32RoundTrip) {
  for (uint32_t v :
       {0u, 1u, 255u, 256u, 65535u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu}) {
    std::string s;
    gritdb::PutFixed32(&s, v);
    std::string_view in(s);
    uint32_t out = 0;
    ASSERT_TRUE(gritdb::GetFixed32(&in, &out));
    EXPECT_EQ(out, v);
    EXPECT_TRUE(in.empty()); // input fully consumed
  }
}

TEST(FixedTest, Fixed64RoundTrip) {
  for (uint64_t v :
       {0ull, 1ull, 0xFFFFFFFFull, 0x100000000ull, 0xFFFFFFFFFFFFFFFFull}) {
    std::string s;
    gritdb::PutFixed64(&s, v);
    ASSERT_EQ(s.size(), 8u);
    std::string_view in(s);
    uint64_t out = 0;
    ASSERT_TRUE(gritdb::GetFixed64(&in, &out));
    EXPECT_EQ(out, v);
  }
}

TEST(FixedTest, TruncatedInputFails) {
  std::string s;
  gritdb::PutFixed32(&s, 12345);
  std::string_view in(s.data(), 3); // only 3 of 4 bytes
  uint32_t out = 0;
  EXPECT_FALSE(gritdb::GetFixed32(&in, &out));
}
