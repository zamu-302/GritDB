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

} // namespace gritdb
