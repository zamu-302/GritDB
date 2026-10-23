#include <bitset>
#include <cstdint>
#include <string_view>

inline void PutVarint64(std::string *dst, uint64_t v) {
  while (v >= 128) {
    dst->push_back(static_cast<char>((v & 0x7F) | 0x80));
    v >>= 7;
  }
  dst->push_back(static_cast<char>(v));
}

inline bool GetVarint64(std::string_view *input, uint64_t *out) {
  uint64_t result = 0;
  for (uint64_t shift = 0; shift <= 63 && !input->empty(); shift += 7) {
    uint64_t byte = static_cast<uint8_t>(input->front());
    input->remove_prefix(1);
    if (shift == 63 && (byte & 0x7F) > 1) {
      return false;
    }
    result |= (byte & 0x7F) << shift;
    if ((byte & 0x80) == 0) {
      *out = result;
      return true;
    }
  }
  return false;
}
