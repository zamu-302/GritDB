#pragma once
#include "block_builder.h"
#include "dbformat.h"
#include "filesystem.h"
#include "format.h"
#include "status.h"
#include <cstdint>
#include <string>
#include <string_view>

namespace gritdb {

struct TableFormat {

  size_t block_restart_interval = 16;
  size_t block_size = 4096;
  Compartor comp = CompareInternalKey;
  bool verify_checksums = true;
};
class TableBuilder {
  TableBuilder(const TableFormat &options, WritableFile *file);
  TableBuilder(const TableBuilder &) = delete;
  TableBuilder &operator=(const TableBuilder &) = delete;

  void Add(std::string_view key, std::string_view value);
  Status Finish();
  const Status &status() const;
  uint64_t NumEntries() const;
  uint64_t FileSize() const;

private:
  void Flush();
  void WriteBlock(std::string_view contetns, BlockHandle *handle);

  TableFormat options;
  WritableFile *file;
  uint64_t offset = 0;
  uint64_t num_entries = 0;
  std::string last_key;
  BlockBuilder data_block;
  BlockBuilder index_block;
  std::string handle_encoding;
  bool closed = false;
  Status stat;
};
} // namespace gritdb
