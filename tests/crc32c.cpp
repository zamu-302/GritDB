#include "../include/crc32c.h"
#include <cstring>
#include <gtest/gtest.h>
#include <string>

using gritdb::crc32c::Extend;
using gritdb::crc32c::Value;

TEST(crc32cTest, StandardCheckValue) {
  const char *s = "123456789";
  EXPECT_EQ(Value(s, 9), 0xE3069283u);
}
TEST(Crc32cTest, EmptyInputIsZero) { EXPECT_EQ(Value("", 0), 0u); }

TEST(Crc32cTest, Rfc3720Vectors) {
  char buf[32];

  std::memset(buf, 0x00, sizeof(buf));
  EXPECT_EQ(Value(buf, sizeof(buf)), 0x8A9136AAu);

  std::memset(buf, 0xFF, sizeof(buf));
  EXPECT_EQ(Value(buf, sizeof(buf)), 0x62A8AB43u);

  for (int i = 0; i < 32; i++)
    buf[i] = static_cast<char>(i);
  EXPECT_EQ(Value(buf, sizeof(buf)), 0x46DD794Eu);

  for (int i = 0; i < 32; i++)
    buf[i] = static_cast<char>(31 - i);
  EXPECT_EQ(Value(buf, sizeof(buf)), 0x113FDB5Cu);

  const uint8_t pdu[48] = {
      0x01, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
      0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x18, 0x28, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  EXPECT_EQ(Value(reinterpret_cast<const char *>(pdu), sizeof(pdu)),
            0xD9963A56u);
}

TEST(Crc32cTest, ExtendMatchesWholeBuffer) {
  std::string data;
  for (int i = 0; i < 100; i++)
    data.push_back(static_cast<char>(i * 7 + 3));

  uint32_t whole = Value(data.data(), data.size());
  for (size_t split = 0; split <= data.size(); split++) {
    uint32_t part = Value(data.data(), split);
    part = Extend(part, data.data() + split, data.size() - split);
    EXPECT_EQ(part, whole) << "split at " << split;
  }
}

TEST(Crc32cTest, DetectsSingleBitFlip) {
  std::string data(64, 'x');
  uint32_t original = Value(data.data(), data.size());
  for (size_t byte = 0; byte < data.size(); byte++) {
    for (int bit = 0; bit < 8; bit++) {
      std::string copy = data;
      copy[byte] = static_cast<char>(copy[byte] ^ (1 << bit));
      EXPECT_NE(Value(copy.data(), copy.size()), original);
    }
  }
}

TEST(Crc32cTest, DifferentDataDifferentCrc) {
  EXPECT_NE(Value("a", 1), Value("b", 1));
  EXPECT_NE(Value("ab", 2), Value("ba", 2)); // order matters
}
