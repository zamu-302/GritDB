#include "table.h"
#include "block_iterator.h"
#include "filesystem.h"
#include "status.h"
#include "table_builder.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace gritdb {
namespace {
Status ReadBlock(RandomAccessFile *file, const BlockHandle &handle,
                 uint64_t file_size, bool verify, std::string *contents) {
  Status s = CheckHandleRange(handle, file_size);
  if (!s.ok()) {
    return s;
  }
  // size of record
  const size_t n = static_cast<size_t>(handle.size) + trailer_size;
  std::string buff(n, '\0');
  std::string_view result;
  s = file->Read(&result, buff.data(), n, handle.offset);
  if (!s.ok()) {
    return s;
  }
  if (result.size() != n) {
    return Status::Corruption("short block read");
  }
  std::string_view body;
  if (verify) {
    s = CheckTrailer(result, &body);
    if (!s.ok()) {
      return s;
    }
  } else {
    body = result.substr(0, handle.size);
  }
  contents->assign(body.data(), body.size());
  return s;
}
Status DecodeHandleValue(std::string_view value, BlockHandle *handle) {
  Status s = handle->DecodeFrom(&value);
  if (!s.ok()) {
    return s;
  }
  if (!value.empty()) {
    return Status::Corruption("trailing bytes after block handle");
  }
  return Status::OK();
}
} // namespace

Status Table::Open(const TableFormat &options,
                   std::unique_ptr<RandomAccessFile> file, uint64_t file_size,
                   std::unique_ptr<Table> *out) {
  if (file == nullptr) {
    return Status::InvalidArgument("null file.");
  }
  if (file_size < footer_size) {
    return Status::Corruption("file too short to be a table.");
  }
  char footer_buff[footer_size];
  std::string_view footer_view;
  Status s = file->Read(&footer_view, footer_buff, footer_size,
                        file_size - footer_size);
  if (!s.ok()) {
    return s;
  }
  if (footer_view.size() != footer_size) {
    return Status::Corruption("short footer read");
  }

  Footer footer;
  s = footer.DecodeFrom(&footer_view);
  if (!s.ok()) {
    return s;
  }
  std::string index_contents;
  s = ReadBlock(file.get(), footer.index_handle, file_size,
                options.verify_checksums, &index_contents);
  if (!s.ok()) {
    return s;
  }
  std::unique_ptr<Block> index_block;
  s = Block::Open(std::move(index_contents), &index_block);
  if (!s.ok()) {
    return s;
  }
  out->reset(
      new Table(options, std::move(file), file_size, std::move(index_block)));
  return Status::OK();
}
Status Table::InternalGet(std::string_view seek_key, std::string *found_key,
                          std::string *value) const {
  BlockIterator index_it(index_block_.get(), options_.comp);
  index_it.Seek(seek_key);
  if (!index_it.get_status().ok()) {
    return index_it.get_status();
  }
  if (!index_it.Valid()) {
    return Status::NotFound();
  }
  BlockHandle handle;
  Status s = DecodeHandleValue(index_it.get_value(), &handle);
  if (!s.ok()) {
    return s;
  }
  std::string contents;
  s = ReadBlock(file_.get(), handle, file_size_, options_.verify_checksums,
                &contents);
  if (!s.ok()) {
    return s;
  }
  std::unique_ptr<Block> data_block;
  s = Block::Open(std::move(contents), &data_block);
  if (!s.ok()) {
    return s;
  }
  BlockIterator it(data_block.get(), options_.comp);
  it.Seek(seek_key);
  if (!it.get_status().ok()) {
    return it.get_status();
  }
  if (!it.Valid()) {
    return Status::Corruption("index points to a block with no key>=target.");
  }
  found_key->assign(it.get_key().data(), it.get_key().size());
  value->assign(it.get_value().data(), it.get_value().size());
  return Status::OK();
}

} // namespace gritdb
