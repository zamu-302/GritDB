#include "../include/crc32c.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace gritdb::crc32c {
namespace {
constexpr std::array<uint32_t, 256> makeTable() {
  std::array<uint32_t, 256> table{};
  for (int i = 0; i < 256; i++) {
    uint32_t crc = i;
    for (int j = 0; j < 8; j++) {
      crc = (crc & 1) ? (crc >> 1) ^ 0x82F63B78u : (crc >> 1);
    }
    table[i] = crc;
  }
  return table;
}
constexpr std::array<uint32_t, 256> kTable = makeTable();
} // namespace
uint32_t Extend(uint32_t init_crc, const char *data, size_t n) {
  const auto *p = reinterpret_cast<const uint8_t *>(data);
  uint32_t crc = ~init_crc;
  for (size_t i = 0; i < n; i++) {
    crc = kTable[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  }
  return ~crc;
}
} // namespace gritdb::crc32c
