#pragma once
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
namespace gritdb {
class BlockBuilder {
public:
  explicit BlockBuilder(size_t restart_interval)
      : restart_interval{restart_interval}, counter{0}, restart_offset{0} {
    assert(restart_interval >= 1);
  }

  void Add(std::string_view key, std::string_view value);
  // valid until the next Add or Reset
  std::string_view Finish();
  void Reset();
  bool empty() const;
  size_t CurrentSizeEstimate() const;

private:
  size_t restart_interval;
  size_t counter;
  std::vector<uint32_t> restart_offset;
  std::string buffer;
  std::string last_key;
  bool finisied = false;
};

} // namespace gritdb
