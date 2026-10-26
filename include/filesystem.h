#pragma once
#include "status.h"
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

namespace gritdb {
struct FileState {
  std::string data;
  std::mutex mu;
  uint64_t synced_size = 0;
};

class RandomAccessFile {
public:
  virtual Status Read(std::string_view *result, char *buff, size_t n,
                      uint64_t offset) const = 0;
  virtual ~RandomAccessFile() = default;
};
class WritableFile {
public:
  virtual Status Append(std::string_view to) = 0;
  virtual Status Flush() = 0;
  virtual Status Sync() = 0;
  virtual Status Close() = 0;
  virtual ~WritableFile() = default;
};

class FileSystem {
public:
  virtual Status NewWritableFile(const std::string &path,
                                 std::unique_ptr<WritableFile> *result) = 0;
  virtual Status RenameFile(const std::string &from, const std::string &to) = 0;
  virtual Status
  NewRandomAccessFile(const std::string &path,
                      std::unique_ptr<RandomAccessFile> *result) = 0;
  virtual Status DeleteFile(const std::string &path) = 0;
  virtual Status Dirlist(const std::string &path,
                         std::vector<std::string> *names) const = 0;
  virtual Status CreateDir(const std::string &path) = 0;
  virtual Status FileSize(const std::string &path, uint64_t *size) const = 0;
  virtual bool FileExists(const std::string &path) const = 0;
  virtual Status SyncDir(const std::string &path) = 0;

  virtual ~FileSystem() = default;
};

class MemFileSystem : public FileSystem {
public:
  Status NewWritableFile(const std::string &path,
                         std::unique_ptr<WritableFile> *result) override;
  Status RenameFile(const std::string &from, const std::string &to) override;
  Status
  NewRandomAccessFile(const std::string &path,
                      std::unique_ptr<RandomAccessFile> *result) override;
  Status DeleteFile(const std::string &path) override;
  Status Dirlist(const std::string &path,
                 std::vector<std::string> *names) const override;
  Status CreateDir(const std::string &path) override;
  Status FileSize(const std::string &path, uint64_t *size) const override;
  bool FileExists(const std::string &path) const override;
  Status SyncDir(const std::string &path) override;

private:
  std::map<std::string, std::shared_ptr<FileState>> state;
  mutable std::mutex mu_;
  std::set<std::string> dirs_;
};

} // namespace gritdb
