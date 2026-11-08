#pragma once
#include "status.h"
#include <cstdint>
#include <string>
#include <string_view>

namespace gritdb {
class Block;
class BlockIterator {
public:
  BlockIterator(const Block *block, Compartor cmp) : block{block}, cmp{cmp} {};

  bool Valid() const;
  void SeekToFirst();
  void Seek(std::string_view target);
  void Next();
  std::string_view get_key() const;
  std::string_view get_value() const;
  const Status &get_status() const;

private:
  bool ParseNextKey();
  void SeekToRestart(uint32_t index);
  bool DecodeRestartKey(uint32_t index, std::string_view *key);
  void MarkCorrupt(const char *msg);

  const Block *block;
  Compartor cmp;
  size_t current;
  size_t next;
  std::string key;
  std::string_view value;
  Status status;
};
} // namespace gritdb
