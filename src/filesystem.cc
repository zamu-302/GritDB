#include "filesystem.h"
namespace gritdb {

Status ReadFileToString(FileSystem *fs, const std::string &path,
                        std::string *out) {
  out->clear();

  uint64_t size = 0;
  Status s = fs->FileSize(path, &size);
  if (!s.ok())
    return s;

  std::unique_ptr<RandomAccessFile> file;
  s = fs->NewRandomAccessFile(path, &file);
  if (!s.ok())
    return s;

  std::string buf(static_cast<size_t>(size), '\0');
  std::string_view result;
  s = file->Read(&result, buf.data(), buf.size(), 0);
  if (!s.ok())
    return s;
  if (result.size() != buf.size()) {
    return Status::IOError("short read: " + path);
  }

  // `result` may point into `buf`, so copy it out as a string the caller
  // owns.
  out->assign(result.data(), result.size());
  return Status::OK();
}
} // namespace gritdb
