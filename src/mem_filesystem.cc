#include "mem_filesystem.h"
#include "status.h"
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
using namespace gritdb;
namespace fs = std::filesystem;
Status MemFileSystem::NewWritableFile(const std::string &path,
                                      std::unique_ptr<WritableFile> *result) {
  fs::path p = path;
  if (!dirs_.contains(p.parent_path().string())) {
    return Status::NotFound("parent directory not found.");
  } else if (state.contains(path)) {
    return Status::InvalidArgument("path already exists.");
  }
  std::lock_guard<std::mutex> lock(mu_);
  state[path] = std::make_shared<FileState>();
  return Status::OK();
}
Status MemFileSystem::RenameFile(const std::string &from,
                                 const std::string &to) {
  fs::path p = from;
  if (!dirs_.contains(p.parent_path().string())) {
    return Status::NotFound("parent directory not found.");
  } else if (!state.contains(from)) {
    return Status::NotFound("path already exists.");
  }
  state[to] = std::move(state[from]);
  state.erase(from);
  return Status::OK();
}
Status
MemFileSystem::NewRandomAccessFile(const std::string &path,
                                   std::unique_ptr<RandomAccessFile> *result) {}
Status MemFileSystem::DeleteFile(const std::string &path) {
  fs::path p = path;
  if (!dirs_.contains(p.parent_path().string())) {
    return Status::NotFound("parent directory not found.");
  } else if (!state.contains(path)) {
    return Status::NotFound("path already exists.");
  }
}
Status MemFileSystem::Dirlist(const std::string &path,
                              std::vector<std::string> *names) const {}
Status MemFileSystem::CreateDir(const std::string &path) {}
Status MemFileSystem::FileSize(const std::string &path, uint64_t *size) const {}
bool MemFileSystem::FileExists(const std::string &path) const {}
