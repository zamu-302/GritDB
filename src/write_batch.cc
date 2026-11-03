#include "write_batch.h"
#include "coding.h"
#include "dbformat.h"
#include "memtable.h"
#include "status.h"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>
namespace {
struct ParsedEntry {
  gritdb::ValueType type;
  std::string_view key;
  std::string_view value;
};
bool GetLengthPrefixed(std::string_view *input, std::string_view *result) {
  uint64_t length = 0;
  if (!gritdb::GetVarint64(input, &length))
    return false;
  if (length > input->size())
    return false; // compare as uint64_t, narrow afterwards
  *result = input->substr(0, static_cast<size_t>(length));
  input->remove_prefix(static_cast<size_t>(length));
  return true;
}
} // namespace

namespace gritdb {

WriteBatch::WriteBatch() { Clear(); }
void WriteBatch::Clear() { rep_.assign(kBatchHeaderSize, '\0'); }
// sequence
uint64_t WriteBatch::Sequence() const {
  assert(rep_.size() >= kBatchHeaderSize);
  return DecodeFixed64(rep_.data());
}
// setSequence
void WriteBatch::SetSequence(uint64_t sequence) {
  assert(rep_.size() >= kBatchHeaderSize);
  EncodeFixed64(rep_.data(), sequence);
}
// setCount
uint32_t WriteBatch::Count() const {
  assert(rep_.size() >= kBatchHeaderSize);
  return DecodeFixed32(rep_.data() + 8);
}
void WriteBatch::SetContents(std::string_view contents) {
  rep_.assign(contents.data(), contents.size());
}
void WriteBatch::SetCount(uint32_t count) {
  assert(rep_.size() >= kBatchHeaderSize);
  EncodeFixed32(rep_.data(), count);
}
void WriteBatch::Put(std::string_view key, std::string_view value) {
  SetCount(Count() + 1);
  rep_.push_back(static_cast<char>(static_cast<uint8_t>(ValueType::Value)));
  PutVarint64(&rep_, key.size());
  rep_.append(key.data(), key.size());
  PutVarint64(&rep_, value.size());
  rep_.append(value.data(), value.size());
}
void WriteBatch::Delete(std::string_view key) {
  SetCount(Count() + 1);
  rep_.push_back(static_cast<char>(static_cast<uint8_t>(ValueType::Deletion)));
  PutVarint64(&rep_, key.size());
  rep_.append(key.data(), key.size());
}
Status WriteBatch::InsertInto(MemTable *memtable) const {
  assert(memtable != nullptr);
  if (rep_.size() < kBatchHeaderSize) {
    return Status::Corruption("batch smaller than its 12-byte header");
  }
  const uint64_t base_sequence = Sequence();
  const uint32_t count = Count();
  std::string_view cursor = std::string_view(rep_).substr(kBatchHeaderSize);

  if (count > cursor.size() / 2) {
    return Status::Corruption("entry count larger than the data can hold");
  }
  if (count > 0 && base_sequence > kMaxSequence - (count - 1)) {
    return Status::Corruption("sequence numbers overflow");
  }

  std::vector<ParsedEntry> entries;
  entries.reserve(count); // safe: count was bounded by the data size above
  for (uint32_t i = 0; i < count; ++i) {
    const std::string where = "entry " + std::to_string(i) + ": ";

    if (cursor.empty()) {
      return Status::Corruption(where + "missing type byte");
    }
    const uint8_t type_byte = static_cast<uint8_t>(cursor.front());
    cursor.remove_prefix(1);
    if (type_byte != static_cast<uint8_t>(ValueType::Deletion) &&
        type_byte != static_cast<uint8_t>(ValueType::Value)) {
      return Status::Corruption(where + "unknown type byte");
    }
    const ValueType type = static_cast<ValueType>(type_byte);

    std::string_view key;
    if (!GetLengthPrefixed(&cursor, &key)) {
      return Status::Corruption(where + "bad key length or truncated key");
    }
    std::string_view value; // stays empty for a delete
    if (type == ValueType::Value && !GetLengthPrefixed(&cursor, &value)) {
      return Status::Corruption(where + "bad value length or truncated value");
    }
    entries.push_back({type, key, value});
  }
  if (!cursor.empty()) {
    return Status::Corruption("bytes left over after the last entry");
  }

  for (uint32_t i = 0; i < count; ++i) {
    memtable->Add(base_sequence + i, entries[i].key, entries[i].type,
                  entries[i].value);
  }
  return Status::OK();
}
} // namespace gritdb
