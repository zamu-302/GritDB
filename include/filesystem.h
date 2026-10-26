#include "status.h"
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <vector>

namespace gritdb {
class RandomAccessFile;
class WritableFile {
public:
  WritableFile(const std::string &path) : path{path} {}

  std::string path;
};

class FileSystem {
public:
  virtual Status NewWritableFile(const std::string &path,
                                 std::unique_ptr<WritableFile> *result) = 0;
  virtual Status RenameFile(const std::string &from, const std::string &to) = 0;
  virtual Status
  NewRandomAccessFile(const std::string &path,
                      std::unique_ptr<RandomAccessFile> *result) = 0;
  virtual Status DeleteFile() = 0;
  virtual Status RenameFile() = 0;
  virtual std::vector<std::string> Dirlist() = 0;
  virtual Status CreateDir() = 0;
  virtual uint64_t FileSize() = 0;
  virtual bool FileExists() = 0;

  virtual ~FileSystem() = default;
};

} // namespace gritdb
