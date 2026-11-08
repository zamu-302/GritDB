#include "block_builder.h"
#include <gtest/gtest.h>
#include <string>
#include <string_view>

using namespace gritdb;
std::string Hex(std::string_view s) {
  static const char *digits = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    out.push_back(digits[c >> 4]);
    out.push_back(digits[c & 15]);
    out.push_back(' ');
  }
  return out;
}
using Items = std::vector<std::pair<std::string, std::string>>;

std::string Build(size_t interval, const Items &items) {
  BlockBuilder b(interval);
  for (const auto &[k, v] : items)
    b.Add(k, v);
  return std::string(
      b.Finish()); // copy: Finish() is only valid until the next Add/Reset
}
TEST(BlockTest, ExpectedBlockValue) {
  std::string got =
      Build(2, {{"app", "1"}, {"apple", "2"}, {"apply", "3"}, {"b", "4"}});
  const char expected[] = {0x00, 0x03, 0x01, 'a',  'p',  'p',  '1',  0x03,
                           0x02, 0x01, 'l',  'e',  '2',  0x00, 0x05, 0x01,
                           'a',  'p',  'p',  'l',  'y',  '3',  0x00, 0x01,
                           0x01, 'b',  '4',  0x00, 0x00, 0x00, 0x00, 0x0D,
                           0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00};
  std::string_view want(expected, sizeof expected);

  EXPECT_EQ(Hex(got), Hex(want));
}

TEST(BlockTest, ValueLongerThan127UsesTwoBytesVarint) {
  std::string value(200, 'x');
  std::string got = Build(16, {{"k", value}});

  std::string prefix("\x00\x01\xC8\x01k", 5);
  ASSERT_EQ(got.size(), 5 + 200 + 4 + 4u);
  EXPECT_EQ(Hex(got.substr(0, 5)), Hex(prefix));
  EXPECT_EQ(got.substr(5, 200), value);
}
