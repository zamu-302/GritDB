#include "filesystem.h"
#include <map>
#include <mutex>
#include <set>

namespace gritdb {
struct FileState {
  std::string data;
  std::mutex mu;
  uint64_t synced_size = 0;
};

class MemFileSystem : public FileSystem {
public:
  MemFileSystem() {
    dirs_.insert("");
    dirs_.insert("/");
  }
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
  std::set<std::string> dirs_{"/"};
};
} // namespace gritdb
