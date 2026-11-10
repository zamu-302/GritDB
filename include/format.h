#include "block_builder.h"
#include "filesystem.h"
#include "status.h"
#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>
namespace gritdb {
namespace {} // namespace

class BlockHandle {
public:
  BlockHandle() : offset{0}, size{0} {}
  void EncodeTo(std::string *dst);
  Status DecodeFrom(std::string_view *input);

private:
  std::uint64_t offset;
  std::uint64_t size;
};

class Footer {
public:
  void EncodeTo(std::string *dst);
  Status DecodeFrom(std::string_view *input);

private:
  BlockHandle index_handle;
  BlockHandle filter_handle;
  uint64_t magic_number;
};
class TableBuilder {
public:
  TableBuilder();
  Status Add(std::string *key, std::string *value);
  Status Finish();

private:
  WritableFile *file;
  BlockBuilder data_block;
  BlockBuilder index_block;
  uint64_t offset;
  std::string key;
  uint64_t count;
  Status s;
  bool finised;
};
} // namespace gritdb
