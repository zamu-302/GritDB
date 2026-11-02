#include "memtable.h"
#include "dbformat.h"
#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>

namespace gritdb {
const size_t kEntryOverhead = 100;
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
  auto it = table.lower_bound(std::string(K));
  if (it == table.end()) {
    return LookUpResult::NotFound;
  }
  auto user_tag = TagType(it->first);
  auto user_val = it->second;
}

} // namespace gritdb
