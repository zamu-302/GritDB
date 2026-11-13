#include "table_builder.h"
#include "dbformat.h"
#include <cassert>
#include <string_view>

namespace gritdb {

void TableBuilder::Add(std::string_view key, std::string_view value) {
  assert(!closed);
  assert(num_entries == 0 || options.comp(last_key, key) < 0);

  if (!stat.ok()) {
    return;
  }
  data_block.Add(key, value);
  last_key = key;
  ++num_entries;
  if (data_block.CurrentSizeEstimate() >= options.block_size) {
    Flush();
  }
}
void TableBuilder::Flush() {
  if (!stat.ok() || data_block.empty()) {
    return;
  }
  std::string_view contents = data_block.Finish();
  BlockHandle handle;
  WriteBlock(contents, &handle);
  if (!stat.ok()) {
    return;
  }
  handle_encoding.clear();
  handle.EncodeTo(&handle_encoding);
  index_block.Add(last_key, handle_encoding);
  data_block.Reset();
}
const Status &TableBuilder::status() const { return stat; }

uint64_t TableBuilder::NumEntries() const { return num_entries; }
uint64_t TableBuilder::FileSize() const { return offset; }

void TableBuilder::WriteBlock(std::string_view contents, BlockHandle *handle) {
  if (!stat.ok()) {
    return;
  }
  handle->offset = offset;
  handle->size = contents.size();
  stat = file->Append(contents);
  if (!stat.ok()) {
    return;
  }
  std::string trailer;
  MakeTrailer(BlockType::Uncompressed, contents, &trailer);
  stat = file->Append(trailer);
  if (stat.ok()) {
    offset += contents.size() + trailer.size();
  }
}
Status TableBuilder::Finish() {
  assert(!closed);
  Flush();
  closed = true;
  if (!stat.ok()) {
    return stat;
  }
  Footer footer;
  WriteBlock(index_block.Finish(), &footer.index_handle);
  if (!stat.ok()) {
    return stat;
  }
  std::string buff;
  footer.EncodeTo(&buff);
  stat = file->Append(buff);
  if (!stat.ok()) {
    return stat;
  }
  offset += buff.size();
  return stat;
}
} // namespace gritdb
