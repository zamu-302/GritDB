#include <cstddef>
#include <cstdint>

namespace gritdb::crc32c {

uint32_t Extend(uint32_t init_crc, const char *data, size_t n);
inline uint32_t Value(const char *data, size_t n) { return Extend(0, data, n); }

} // namespace gritdb::crc32c
