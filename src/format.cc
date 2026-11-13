#include "format.h"
#include "coding.h"
#include "crc32c.h"
#include "status.h"
#include <cassert>
#include <cstddef>
#include <string>
#include <string_view>

namespace gritdb {

namespace {
static_assert(footer_size == 2 * (handle_size) + 8);

} // namespace
void BlockHandle::EncodeTo(std::string *dst) const {
  PutFixed64(dst, offset);
  PutFixed64(dst, size);
}
Status BlockHandle::DecodeFrom(std::string_view *input) {
  if (input->size() < handle_size) {
    return Status::Corruption("truncated block handle.");
  }

  offset = DecodeFixed32(input->data());
  size = DecodeFixed64(input->data() + 8);
  input->remove_prefix(handle_size);
  return Status::OK();
}

void Footer::EncodeTo(std::string *dst) const {
  const size_t before = dst->size();
  index_handle.EncodeTo(dst);
  filter_handle.EncodeTo(dst);

  PutFixed64(dst, magic_number);
  assert(dst->size() == before + footer_size);
  (void)before;
}
Status Footer::DecodeFrom(std::string_view *input) {
  if (!(input->size() == footer_size)) {
    return Status::Corruption("invaild footer size.");
  }
  std::string_view data = *input;

  if ((DecodeFixed64(data.data() + 2 * handle_size) != magic_number)) {
    return Status::Corruption("not a gritdb type of file");
  }
  BlockHandle index, filter;
  Status s = index.DecodeFrom(&data);
  if (!s.ok()) {
    return s;
  }
  s = filter.DecodeFrom(&data);
  if (!s.ok()) {
    return s;
  }
  index_handle = index;
  filter_handle = filter;
  input->remove_prefix(footer_size);
  return Status::OK();
}

void MakeTrailer(BlockType type, std::string_view contents, std::string *dst) {
  const char type_byte = static_cast<char>(static_cast<uint8_t>(type));
  uint32_t crc = crc32c::Value(contents.data(), contents.size());
  crc = crc32c::Extend(crc, &type_byte, 1);
  dst->push_back(type_byte);
  PutFixed32(dst, crc);
}

Status CheckTrailer(std::string_view block_with_trailer,
                    std::string_view *contents) {
  if (block_with_trailer.size() < trailer_size) {
    return Status::Corruption("block shorter than its trailer");
  }
  const size_t n = block_with_trailer.size() - trailer_size;
  const char *trailer = block_with_trailer.data() + n;
  const char type_byte = trailer[0];
  if (static_cast<uint8_t>(type_byte) !=
      static_cast<uint8_t>(BlockType::Uncompressed)) {
    return Status::Corruption("unknown or unsupported block type");
  }
  uint32_t crc = crc32c::Value(block_with_trailer.data(), n);
  crc = crc32c::Extend(crc, &type_byte, 1);
  if (crc != DecodeFixed32(trailer + 1)) {
    return Status::Corruption("block checksum mismatch");
  }
  *contents = block_with_trailer.substr(0, n);
  return Status::OK();
}

Status CheckHandleRange(const BlockHandle &h, uint64_t file_size) {
  if (file_size < footer_size)
    return Status::Corruption("file smaller than footer");
  const uint64_t data_end = file_size - footer_size; // where the footer begins
  if (data_end < trailer_size || h.size > data_end - trailer_size ||
      h.offset > data_end - trailer_size - h.size) {
    return Status::Corruption("block handle outside file");
  }
  return Status::OK();
}
} // namespace gritdb
