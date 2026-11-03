#include "dbformat.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
namespace gritdb {
enum class LookUpResult { Found, Deleted, NotFound };
const size_t kEntryOverhead = 100;

class MemTable {
public:
  void Add(uint64_t sequence, std::string_view key, ValueType type,
           std::string_view value);
  LookUpResult Get(std::string_view key, uint64_t snapshot_seq,
                   std::string *value) const;
  size_t ApproximateMemoryUsage() const { return memory_usage; }
  size_t Count() const { return table.size(); }
  using Visitor =
      std::function<void(std::string_view user_key, uint64_t sequence,
                         ValueType type, std::string_view value)>;

  void ForEach(const Visitor &visit) const;

private:
  std::map<std::string, std::string, InternalKeyComparator> table;
  size_t memory_usage = 0;
};
} // namespace gritdb
