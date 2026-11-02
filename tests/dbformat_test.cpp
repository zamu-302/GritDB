#include "dbformat.h"
#include <cstring>
#include <gtest/gtest.h>

using namespace gritdb;

TEST(InternalKeyTest, ExactBytesForAbc) {
  std::string k;
  AppendInternalKey(&k, "abc", 5, ValueType::Value);

  const unsigned char expected[] = {'a', 'b', 'c', 0x01, 0x05, 0,
                                    0,   0,   0,   0,    0};
  ASSERT_EQ(k.size(), sizeof expected);
  EXPECT_EQ(std::memcmp(k.data(), expected, sizeof expected), 0);
}

TEST(InternalKeyTest, RoundTrip) {
  std::string k;
  AppendInternalKey(&k, "hello", 12345, ValueType::Deletion);

  EXPECT_EQ(ExtractUserKey(k), "hello");
  const uint64_t tag = ExtractTag(k);
  EXPECT_EQ(TagSequence(tag), 12345u);
  EXPECT_EQ(TagType(tag), ValueType::Deletion);
}

TEST(InternalKeyTest, EmptyKeyAndEmbeddedZero) {
  std::string k;
  AppendInternalKey(&k, "", 1, ValueType::Value);
  EXPECT_EQ(k.size(), 8u); // tag only
  EXPECT_TRUE(ExtractUserKey(k).empty());

  const std::string with_zero("a\0b",
                              3); // the length matters: a C string would cut it
  std::string k2;
  AppendInternalKey(&k2, with_zero, 1, ValueType::Value);
  EXPECT_EQ(k2.size(), 11u);
  EXPECT_EQ(ExtractUserKey(k2), with_zero);
}
