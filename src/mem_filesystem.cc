#include "mem_filesystem.h"
#include "status.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
namespace fs = std::filesystem;
namespace {
std::string Normalize(std::string path) {
  while (path.size() > 1 && path.back() == '/') {
    path.pop_back();
  }
  return path;
}

std::string ParentOf(const std::string &path) {
  return fs::path(path).parent_path().string();
}

} // namespace

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

Status MemFileSystem::NewWritableFile(const std::string &path,
                                      std::unique_ptr<WritableFile> *result) {
  const std::string normalized_path = Normalize(path);
  std::lock_guard lock(mu_);
  if (!dirs_.contains(ParentOf(normalized_path))) {
    return Status::NotFound("parent directory not found.");
  }
  if (dirs_.contains(normalized_path)) {
    return Status::InvalidArgument("path is a directory.");
  }
  auto file_state = std::make_shared<FileState>();
  state[normalized_path] = file_state;
  *result = std::make_unique<MemWritableFile>(file_state);
  return Status::OK();
}

Status MemFileSystem::RenameFile(const std::string &from,
                                 const std::string &to) {

  const std::string normalized_from = Normalize(from);
  const std::string normalized_to = Normalize(to);
  std::lock_guard lock(mu_);
  auto it = state.find(normalized_from);
  if (it == state.end()) {
    return Status::NotFound("rename source not found: " + normalized_from);
  }
  if (!dirs_.contains(ParentOf(normalized_to))) {
    return Status::NotFound("destination directory not found.");
  }
  if (dirs_.contains(normalized_to)) {
    return Status::InvalidArgument("destination is a directory: " +
                                   normalized_to);
  }
  if (normalized_to == normalized_from) {
    return Status::OK();
  }
  state[normalized_to] = std::move(it->second);
  state.erase(it);
  return Status::OK();
}
Status
MemFileSystem::NewRandomAccessFile(const std::string &path,
                                   std::unique_ptr<RandomAccessFile> *result) {
  const std::string normalized_path = Normalize(path);
  std::lock_guard lock(mu_);
  if (!dirs_.contains(ParentOf(normalized_path))) {
    return Status::NotFound("parent directory not found.");
  }

  auto it = state.find(normalized_path);
  if (it == state.end()) {
    return Status::NotFound("file not found: " + path);
  }
  *result = std::make_unique<MemRandomAccessFile>(it->second);
  return Status::OK();
}

Status MemFileSystem::DeleteFile(const std::string &path) {
  const std::string normalized_path = Normalize(path);
  std::lock_guard lock(mu_);
  if (!dirs_.contains(ParentOf(normalized_path))) {
    return Status::NotFound("parent directory not found: " + normalized_path);
  } else if (!state.contains(normalized_path)) {
    return Status::NotFound("path doesn't exist.");
  }
  state.erase(normalized_path);
  return Status::OK();
}
Status MemFileSystem::Dirlist(const std::string &path,
                              std::vector<std::string> *names) const {
  const std::string normalized_path = Normalize(path);
  std::lock_guard lock(mu_);
  if (!dirs_.contains(normalized_path))
    return Status::NotFound("directory not found: " + normalized_path);
  names->clear();
  for (const auto &dir : dirs_) {
    if (dir != normalized_path && ParentOf(dir) == normalized_path) {
      names->emplace_back(fs::path(dir).filename().string());
    }
  }
  for (const auto &entry : state) {
    if (ParentOf(entry.first) == normalized_path) {
      names->push_back(fs::path(entry.first).filename().string());
    }
  }
  std::sort(names->begin(), names->end());
  return Status::OK();
}
Status MemFileSystem::CreateDir(const std::string &path) {
  const std::string normalized_path = Normalize(path);
  std::lock_guard lock(mu_);
  if (!dirs_.contains(ParentOf(normalized_path))) {
    return Status::NotFound("parent directory not found.");
  }
  if (state.contains(normalized_path)) {
    return Status::InvalidArgument("path already exists.");
  }
  dirs_.insert(normalized_path);
  return Status::OK();
}
Status MemFileSystem::FileSize(const std::string &path, uint64_t *size) const {
  const std::string normalized_path = Normalize(path);
  std::shared_ptr<FileState> curr_state;
  {
    std::lock_guard lock(mu_);
    auto it = state.find(normalized_path);
    if (it == state.end()) {
      return Status::NotFound("file not found: " + normalized_path);
    }
    curr_state = it->second;
  }
  std::lock_guard file_lock(curr_state->mu);
  *size = curr_state->data.size();
  return Status::OK();
}
bool MemFileSystem::FileExists(const std::string &path) const {
  const std::string normalized_path = Normalize(path);
  std::lock_guard lock(mu_);
  if (!dirs_.contains(ParentOf(normalized_path))) {
    return false;
  }
  if (!state.contains(normalized_path)) {
    return false;
  }
  return true;
}
Status MemFileSystem::SyncDir(const std::string &path) {
  const std::string normalized_path = Normalize(path);
  std::lock_guard lock(mu_);
  if (!dirs_.contains(normalized_path)) {
    return Status::NotFound("directory not found: " + normalized_path);
  }
  return Status::OK();
}

} // namespace gritdb
