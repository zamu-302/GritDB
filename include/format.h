#include "block_builder.h"
#include "filesystem.h"
#include "status.h"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
namespace gritdb {

inline constexpr size_t handle_size = 16;
inline constexpr size_t trailer_size = 5;
inline constexpr size_t footer_size = 40;

inline constexpr uint64_t magic_number = 8881518461642107531ULL;

enum class BlockType : uint8_t { Uncompressed = 0, Compressed = 1 };

struct BlockHandle {
  void EncodeTo(std::string *dst) const;
  Status DecodeFrom(std::string_view *input);
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
};

struct Footer {
  void EncodeTo(std::string *dst) const;
  Status DecodeFrom(std::string_view *input);
  BlockHandle index_handle;
  BlockHandle filter_handle;
};
void MakeTrailer(BlockType type, std::string_view contents, std::string *dst);
Status CheckTrailer(std::string_view block_with_trailer,
                    std::string_view *contents);
Status CheckHandleRange(const BlockHandle &h, uint64_t file_size);
} // namespace gritdb
