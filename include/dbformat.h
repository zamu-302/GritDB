#pragma once
#include "coding.h"
#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>

namespace gritdb {

enum class ValueType { Deletion = 0, Value = 1 };
struct ParsedInternalKey {
  std::string_view user_key;
  uint64_t sequence;
  ValueType type;
};
inline uint64_t TagSequence(uint64_t tag) { return tag >> 8; }
inline ValueType TagType(uint64_t tag) {
  return static_cast<ValueType>(tag & 0xFF);
}
inline bool ParseInternalKey(std::string_view, ParsedInternalKey *out) {
  return out->user_key.size() >= 8 ||
         (out->type > ValueType::Value || out->type < ValueType::Deletion);
}
constexpr uint64_t kMaxSequence = (1ULL << 56) - 1;
inline uint64_t PackTag(uint64_t sequence, ValueType type) {
  assert(sequence <= kMaxSequence);
  return (sequence << 8) | static_cast<std::uint64_t>(type);
}
inline void AppendInternalKey(std::string *dst, std::string_view user_key,
                              uint64_t sequence, ValueType type) {
  dst->append(user_key.data(), user_key.size());
  PutFixed64(dst, PackTag(sequence, type));
}
inline std::string_view ExtractUserKey(std::string_view internal_key) {
  assert(internal_key.size() >= 8);
  return internal_key.substr(0, internal_key.size() - 8);
}
inline uint64_t ExtractTag(std::string_view internal_key) {
  assert(internal_key.size() >= 8);
  return DecodeFixed64(internal_key.data() + internal_key.size() - 8);
}
inline int CompareInternalKey(std::string_view a, std::string_view b) {
  auto key_a = ExtractUserKey(a);
  auto key_b = ExtractUserKey(b);
  auto tag_a = ExtractTag(a);
  auto tag_b = ExtractTag(b);
  auto res = key_a.compare(key_b);
  if (res) {
    return res;
  }
  if (tag_a > tag_b) {
    return -1;
  }
  return (tag_a < tag_b) ? 1 : 0;
}
inline int CompareByteWise(std::string_view a, std::string_view b) {
  return a.compare(b);
}

struct InternalKeyComparator {
  inline bool operator()(const std::string &a, const std::string &b) const {
    return CompareInternalKey(a, b) < 0;
  }
};
} // namespace gritdb
