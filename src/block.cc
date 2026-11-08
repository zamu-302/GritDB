#include "block.h"
#include "block_iterator.h"
#include "coding.h"
#include "status.h"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace gritdb {
Status Block::Open(std::string contents, std::unique_ptr<Block> *out) {
  size_t size = contents.size();
  if (size < 4) {
    return Status::Corruption("size of content is smaller than 4 bytes.");
  }
  uint32_t curr_num_restart = DecodeFixed32(contents.data() + size - 4);
  if (curr_num_restart < 1) {
    return Status::Corruption("no restarts.");
  } else if (curr_num_restart > (size - 4) / 4) {
    return Status::Corruption("restart count too large.");
  }
  size_t curr_restart_offset = size - 4 - (4 * curr_num_restart);
  uint32_t prev = 0;
  for (uint32_t i = 0; i < curr_num_restart; ++i) {
    uint32_t off = DecodeFixed32(contents.data() + curr_restart_offset + 4 * i);

    if (i == 0) {
      if (off != 0) {
        return Status::Corruption("first restart must be zero.");
      }
    } else if (off <= prev) {
      return Status::Corruption("restarts not increasing");
    }
    bool inside =
        off < curr_restart_offset || (i == 0 && curr_restart_offset == 0);
    if (!inside) {
      return Status::Corruption("restart past entries");
    }
    prev = off;
  }
  out->reset(
      new Block(std::move(contents), curr_num_restart, curr_restart_offset));
  return Status::OK();
}
size_t Block::size() const { return data.size(); }
uint32_t Block::num_restart() const { return num_restarts; }
size_t Block::entries_end() const { return restarts_offset; }
uint32_t Block::restartsOffset(uint32_t i) const {
  assert(i < num_restarts);
  return DecodeFixed32(data.data() + restarts_offset + (4 * i));
}
std::string_view Block::get_data() const { return std::string_view(data); }
BlockIterator Block::newIterator(Compartor cmp) const {
  return BlockIterator(this, cmp);
}

} // namespace gritdb
