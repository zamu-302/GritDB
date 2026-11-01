#pragma once
#include "filesystem.h"
#include "status.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
namespace gritdb {

constexpr size_t kBlockSize = 32768;
constexpr size_t kHeaderSize = 7;
enum class RecordType : uint8_t { Zero, Full, First, Middle, Last };
std::string EncodeRecord(RecordType type, std::string_view payload);
class LogWriter {
public:
  explicit LogWriter(std::unique_ptr<WritableFile> file)
      : file{std::move(file)} {}
  Status AddRecord(std::string_view record);
  Status Sync();
  Status Close();

private:
  std::unique_ptr<WritableFile> file;
  size_t blockOffset;
};
class LogReader {

public:
  LogReader(std::string_view data) : data{std::move(data)}, curr_pos{0} {}
  bool ReadRecord(std::string *record);
  Status status() const;

private:
  std::string_view data;
  Status curr;
  size_t curr_pos;
};

} // namespace gritdb
