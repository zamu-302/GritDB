#pragma once
#include <charconv>
#include <cstdint>
#include <format>
#include <iterator>
#include <string>
#include <string_view>
namespace gritdb {
enum class FileType { kLog, kTable, kCurrent, kTemp, kUnknown };
// dir + "/" + %06d + ".log"
inline std::string LogFileName(const std::string &dir, uint64_t n) {
  std::string buff = dir + "/";
  std::format_to(std::back_inserter(buff), "{:06d}", n);
  buff += ".log";
  return buff;
}
// ".sst"
inline std::string TableFileName(const std::string &dir, uint64_t n) {
  std::string buff = "/" + dir + "/";
  std::format_to(std::back_inserter(buff), "{:06d}", n);
  buff += ".sst";
  return buff;
}
// dir + "/CURRENT"

inline std::string CurrentFileName(const std::string &dir) {
  return "/" + dir + "/CURRENT";
}
// dir + "/CURRENT.tmp"

inline std::string TempFileName(const std::string &dir) {
  return "/" + dir + "/CURRENT.tmp";
}
inline bool ParseFileName(std::string_view name, uint64_t *number,
                          FileType *type) {
  if (name == "CURRENT") {
    *number = 0;
    *type = FileType::kCurrent;
    return true;
  }
  if (name == "CURRENT.tmp") {
    *number = 0;
    *type = FileType::kTemp;
    return true;
  }
  if (name.size() < 10) {
    return false;
  }

  FileType t;
  if (name.ends_with(".log"))
    t = FileType::kLog;
  else if (name.ends_with(".sst"))
    t = FileType::kTable;
  else
    return false;

  name.remove_suffix(4);
  if (name.size() > 6 && name.front() == '0')
    return false;
  uint64_t n = 0;
  auto [ptr, ec] = std::from_chars(name.data(), name.data() + name.size(), n);
  if (ec != std::errc() || ptr != name.data() + name.size())
    return false;

  *number = n;
  *type = t;
  return true;
}
} // namespace gritdb
