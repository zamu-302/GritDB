#include "format.h"
#include "coding.h"
#include "crc32c.h"
#include "status.h"
#include <string>
#include <string_view>

namespace gritdb {

namespace {
const size_t handle_size = 16;
const size_t trailer_size = 5;
const size_t footer_size = 40;
constexpr uint64_t magic_number = 8881518461642107531;
static_assert(footer_size == 2 * (handle_size) + 8);

enum class BlockType : uint8_t { Uncompressed = 0, Compressed = 1 };
Status RangeCheck(size_t handle, uint64_t file_size) { return Status::OK(); }
void MakeTrailer(BlockType type, std::string *data, std::string *buff) {
  buff->push_back(static_cast<char>(type));
  EncodeFixed32(buff->data(), crc32c::Value(data->data(), data->size()));
}
Status CheckTrailer(std::string_view buff) { return Status::OK(); }

} // namespace
void BlockHandle::EncodeTo(std::string *dst) {
  EncodeFixed64(dst->data(), offset);
  EncodeFixed64(dst->data(), size);
}
Status BlockHandle::DecodeFrom(std::string_view *input) {
  std::string_view data = input->data();

  DecodeFixed32(data.data());
  data.remove_prefix(16);

  if (data.size() < 16) {
    return Status::Corruption("");
  }
  *input = data;
  return Status::OK();
}
void Footer::EncodeTo(std::string *dst) {
  index_handle.EncodeTo(dst);
  filter_handle.EncodeTo(dst);
  DecodeFixed64(dst->data());
}
Status Footer::DecodeFrom(std::string_view *input) {
  if (!(input->size() == footer_size)) {
    return Status::Corruption("invaild footer size.");
  }
  std::string_view data = input->data();

  DecodeFixed64(data.data());

  if (data.size() < 16) {
    return Status::Corruption("");
  }
  *input = data;
  return Status::OK();
}

} // namespace gritdb
