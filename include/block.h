#include "status.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
namespace gritdb {

class BlockIterator;
class Block {
public:
  Block(const Block &) = delete;
  Block(const Block &&) = delete;
  Block &operator=(const Block &&) = delete;
  Block &operator=(const Block &) = delete;
  ~Block() = default;

  static Status Open(std::string contents, std::unique_ptr<Block> *out);
  size_t entries_end() const;
  size_t size() const;
  uint32_t num_restart() const;
  uint32_t restartsOffset(uint32_t i) const;
  std::string_view get_data() const;
  BlockIterator newIterator(Compartor cmp) const;

private:
  Block(std::string data, uint32_t num_restarts, size_t restarts_offset)
      : data(std::move(data)), num_restarts{num_restarts},
        restarts_offset{restarts_offset} {}
  std::string data;
  uint32_t num_restarts;
  size_t restarts_offset;
};
} // namespace gritdb
