#pragma once
#include "status.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace gritdb {

class MemTable; // forward declaration: the header doesn't need the full class

class WriteBatch {
public:
  WriteBatch();

  void Put(std::string_view key, std::string_view value);
  void Delete(std::string_view key);
  void Clear();

  uint32_t Count() const;
  uint64_t Sequence() const;
  void SetSequence(uint64_t sequence);

  std::string_view Contents() const { return rep_; }
  void SetContents(std::string_view contents);

  Status InsertInto(MemTable *memtable) const;

private:
  static constexpr size_t kBatchHeaderSize = 12;
  void SetCount(uint32_t count);

  std::string rep_;
};

} // namespace gritdb
