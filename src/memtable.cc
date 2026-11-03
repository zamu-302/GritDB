#include "memtable.h"
#include "dbformat.h"
#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>

namespace gritdb {
void MemTable::Add(uint64_t sequence, std::string_view key, ValueType type,
                   std::string_view value) {
  assert(sequence <= kMaxSequence);
  assert(type == ValueType::Value || value.empty());

  std::string ikey;
  AppendInternalKey(&ikey, key, sequence, type);

  const size_t entry_size = ikey.size() + value.size() + kEntryOverhead;
  auto val = table.emplace(std::move(ikey), std::string(value));
  assert(val.second);
  // adding only if inserted.
  if (val.second) {
    memory_usage += entry_size;
  }
}
LookUpResult MemTable::Get(std::string_view key, uint64_t snapshot_seq,
                           std::string *value) const {
  std::string K;
  AppendInternalKey(&K, key, snapshot_seq, ValueType::Value);
  auto it = table.lower_bound(K);
  if (it == table.end()) {
    return LookUpResult::NotFound;
  }
  if (ExtractUserKey(it->first) != key) {
    return LookUpResult::NotFound;
  }
  switch (TagType(ExtractTag(std::string(it->first)))) {
  case ValueType::Value:
    *value = it->second;
    return LookUpResult::Found;
  case ValueType::Deletion:
    return LookUpResult::Deleted;
  default:
    return LookUpResult::NotFound;
  }
}
void MemTable::ForEach(const Visitor &visit) const {
  for (const auto &[key, value] : table) {
    auto tag = ExtractTag(key);
    visit(ExtractUserKey(key), TagSequence(tag), TagType(tag), value);
  }
}

} // namespace gritdb
