#include <filesystem.h>
namespace gritdb {

class PosixFileSystem : public FileSystem {
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
};

} // namespace gritdb
