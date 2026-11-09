#include "block_iterator.h"
#include "block.h"
#include "coding.h"
#include "status.h"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
namespace gritdb {
namespace {

bool DecodeEntryHeader(std::string_view *input, uint64_t *shared,
                       uint64_t *non_shared, uint64_t *value_len) {
  if (!GetVarint64(input, shared) || !GetVarint64(input, non_shared) ||
      !GetVarint64(input, value_len)) {
    return false;
  }
  // Subtraction form, so hostile lengths cannot overflow an addition.
  if (*non_shared > input->size())
    return false;
  if (*value_len > input->size() - *non_shared)
    return false;
  return true;
}

} // namespace
bool BlockIterator::Valid() const {
  return status.ok() && current < block->entries_end();
}
void BlockIterator::Seek(std::string_view target) {
  uint32_t left = 0;
  uint32_t right = block->num_restart() - 1;
  while (left < right) {
    size_t mid = left + (right - left + 1) / 2;
    std::string_view mid_key;
    if (!DecodeRestartKey(mid, &mid_key)) {
      return;
    }
    if (cmp(mid_key, target) < 0) {
      left = mid;
    } else {
      right = mid - 1;
    }
  }
  SeekToRestart(left);
  while (ParseNextKey()) {
    if (cmp(key, target) >= 0) {
      return;
    }
  }
}
std::string_view BlockIterator::get_key() const {
  assert(Valid());
  return key;
}
std::string_view BlockIterator::get_value() const {
  assert(Valid());
  return value;
}
bool BlockIterator::ParseNextKey() {
  if (!status.ok()) {
    return false;
  }
  if (next >= block->entries_end()) {
    current = block->entries_end();
    return false;
  }

  std::string_view data = block->get_data();
  std::string_view input = data.substr(next, block->entries_end() - next);

  uint64_t shared, non_shared, value_len;
  if (!DecodeEntryHeader(&input, &shared, &non_shared, &value_len)) {
    MarkCorrupt("bad entry header");
    return false;
  }
  if (shared > key.size()) {
    MarkCorrupt("shared prefix longer that previous key");
    return false;
  }
  key.resize(shared);
  key.append(input.data(), non_shared);
  value = std::string_view(input.data() + non_shared, value_len);

  current = next;
  next = static_cast<size_t>(value.data() + value.size() - data.data());
  return true;
}
void BlockIterator::SeekToRestart(uint32_t i) {
  key.clear();
  value = {};
  next = block->restartsOffset(i);
  current = block->entries_end();
}
void BlockIterator::SeekToFirst() {
  SeekToRestart(0);
  ParseNextKey();
}
void BlockIterator::Next() {
  assert(Valid());
  ParseNextKey();
}
bool BlockIterator::DecodeRestartKey(uint32_t index, std::string_view *key) {
  size_t offset = block->restartsOffset(index);
  if (offset >= block->entries_end()) {
    MarkCorrupt("restart offset past entries.");
    return false;
  }
  std::string_view data = block->get_data();
  std::string_view input = data.substr(offset, block->entries_end() - offset);

  uint64_t shared, non_shared, value_len;
  if (!DecodeEntryHeader(&input, &shared, &non_shared, &value_len) ||
      shared != 0) {
    MarkCorrupt("bad entry header");
    return false;
  }
  *key = std::string_view(input.data(), non_shared);
  return true;
}
void BlockIterator::MarkCorrupt(const char *msg) {
  current = next = block->entries_end();
  key.clear();
  value = {};
  status = Status::Corruption(msg);
}

const Status &BlockIterator::get_status() const { return status; }

} // namespace gritdb
