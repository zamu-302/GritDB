#pragma once
#include <cstdint>
#include <string>
#include <string_view>
namespace gritdb {
// fixed
inline void EncodeFixed32(char *dst, uint32_t val) {
  auto *p = reinterpret_cast<uint8_t *>(dst);
  for (int i = 0; i < 4; ++i) {
    p[i] = static_cast<uint8_t>(val >> (8 * i));
  }
}
inline void EncodeFixed64(char *dst, uint64_t val) {
  auto *p = reinterpret_cast<uint8_t *>(dst);
  for (int i = 0; i < 8; ++i) {
    p[i] = static_cast<uint8_t>(val >> (8 * i));
  }
}

inline uint32_t DecodeFixed32(const char *dst) {
  auto *p = reinterpret_cast<const uint8_t *>(dst);
  uint32_t val{};
  for (int i = 0; i < 4; i++)
    val |= static_cast<uint32_t>(p[i] << (8 * i));
  return val;
}
inline uint64_t DecodeFixed64(const char *dst) {
  auto *p = reinterpret_cast<const uint8_t *>(dst);
  uint64_t val{};
  for (int i = 0; i < 8; i++)
    val |= static_cast<uint64_t>(p[i] << (8 * i));
  return val;
}

inline void PutFixed32(std::string *dst, uint32_t val) {
  char buf[4]; // 4 byte buffer
  EncodeFixed32(buf, val);
  dst->append(buf, 4);
}
inline void PutFixed64(std::string *dst, uint64_t val) {
  char buf[8];
  EncodeFixed64(buf, val);
  dst->append(buf, 8);
}

inline bool GetFixed32(std::string_view *input, uint32_t *val) {
  if (input->size() < 4)
    return false;
  *val = DecodeFixed32(input->data());
  input->remove_prefix(4);
  return true;
}
inline bool GetFixed64(std::string_view *input, uint64_t *val) {
  if (input->size() < 8)
    return false;
  *val = DecodeFixed64(input->data());
  input->remove_prefix(8);
  return true;
}

// varint
inline void PutVarint64(std::string *dst, uint64_t v) {
  while (v >= 128) {
    dst->push_back(static_cast<char>((v & 0x7F) | 0x80));
    v >>= 7;
  }
  dst->push_back(static_cast<char>(v));
}

inline bool GetVarint64(std::string_view *input, uint64_t *out) {
  std::string_view in = *input;
  uint64_t result = 0;
  for (uint64_t shift = 0; shift <= 63 && !in.empty(); shift += 7) {
    uint64_t byte = static_cast<uint8_t>(in.front());
    in.remove_prefix(1);
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
}; // namespace gritdb
