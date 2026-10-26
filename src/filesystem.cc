#include "filesystem.h"
#include "status.h"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
namespace gritdb {

class MemWritableFile : public WritableFile {
public:
  explicit MemWritableFile(std::shared_ptr<FileState> state)
      : state(std::move(state)) {}

  Status Append(std::string_view to) override {
    if (!closed) {
      std::lock_guard<std::mutex> lock(state->mu);
      state->data.append(to.data(), to.size());
      return Status::OK();
    }
    return Status::IOError("append to closed file");
  }
  Status Flush() override {
    if (!closed) {
      return Status::OK();
    }
    return Status::IOError("flused on closed file");
  }
  Status Sync() override {
    if (!closed) {
      std::lock_guard<std::mutex> lock(state->mu);
      state->synced_size = state->data.size();
      return Status::OK();
    }
    return Status::IOError("synced on closed file");
  }
  Status Close() override {
    closed = true;
    return Status::OK();
  }

private:
  bool closed = false;
  std::shared_ptr<FileState> state;
};
class MemRandomAccessFile : public RandomAccessFile {
public:
  explicit MemRandomAccessFile(std::shared_ptr<FileState> state)
      : state{std::move(state)} {}
  Status Read(std::string_view *result, char *buff, size_t n,
              uint64_t offset) const override {
    std::lock_guard<std::mutex> lock(state->mu);
    const std::string &data = state->data;
    if (offset >= data.size()) {
      *result = std::string_view();
      return Status::OK();
    }
    size_t available = data.size() - static_cast<size_t>(offset);
    size_t len = std::min(n, available);
    std::memcpy(buff, data.data() + offset, len);
    *result = std::string_view(buff, len);
    return Status::OK();
  }

private:
  std::shared_ptr<FileState> state;
};
} // namespace gritdb
