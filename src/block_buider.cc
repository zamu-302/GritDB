#include "block_builder.h"
#include "coding.h"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <sys/types.h>
namespace gritdb {
void BlockBuilder::Add(std::string_view key, std::string_view value) {
  assert(!finisied);
  assert(buffer.empty() || std::string_view(last_key) < key);
  assert(buffer.size() < std::numeric_limits<uint32_t>::max());

  size_t shared = 0;
  if (counter >= restart_interval) {
    counter = 0;
    restart_offset.push_back(static_cast<uint32_t>(buffer.size()));
  } else {
    size_t min_len = std::min(last_key.size(), key.size());
    auto mm = std::mismatch(last_key.begin(), last_key.begin() + min_len,
                            key.begin());
    shared = static_cast<size_t>(mm.first - last_key.begin());
  }
  PutVarint64(&buffer, shared);
  PutVarint64(&buffer, key.size() - shared);
  PutVarint64(&buffer, value.size());
  buffer.append(key.data() + shared, key.size() - shared);
  buffer.append(value.data(), value.size());

  last_key.resize(shared);
  last_key.append(key.data() + shared, key.size() - shared);
  assert(std::string_view(last_key) == key);
  ++counter;
}
std::string_view BlockBuilder::Finish() {
  assert(!finisied);
  for (uint32_t offset : restart_offset) {
    PutFixed32(&buffer, offset);
  }
  PutFixed32(&buffer, static_cast<uint32_t>(restart_offset.size()));
  finisied = true;
  return buffer;
}
void BlockBuilder::Reset() {
  buffer.clear();
  restart_offset.clear();
  restart_offset.push_back(0);
  counter = 0;
  last_key.clear();
  finisied = false;
}
bool BlockBuilder::empty() const { return buffer.empty(); }

size_t BlockBuilder::CurrentSizeEstimate() const {
  if (finisied) {
    return buffer.size();
  }
  return buffer.size() + restart_offset.size() * sizeof(uint32_t) +
         sizeof(uint32_t);
}
} // namespace gritdb
