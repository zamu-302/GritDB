#pragma once
#include "block.h"
#include "filesystem.h" // RandomAccessFile
#include "status.h"
#include "table_builder.h"
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace gritdb {

class Table {
public:
  static Status Open(const TableFormat &options,
                     std::unique_ptr<RandomAccessFile> file, uint64_t file_size,
                     std::unique_ptr<Table> *out);

  Status InternalGet(std::string_view seek_key, std::string *found_key,
                     std::string *value) const;

  uint64_t file_size() const { return file_size_; }

private:
  Table(const TableFormat &options, std::unique_ptr<RandomAccessFile> file,
        uint64_t file_size, std::unique_ptr<Block> index_block);

  TableFormat options_;
  std::unique_ptr<RandomAccessFile> file_;
  uint64_t file_size_;
  std::unique_ptr<Block> index_block_;
};

} // namespace gritdb
