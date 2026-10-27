#include "posix_filesystem.h"
#include "filesystem.h"
#include "status.h"
#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace gritdb {
namespace {
Status ErrnoToStatus(const std::string &context, int err) {
  std::string msg = context + ": " + std::generic_category().message(err);
  if (err == ENOENT)
    return Status::NotFound(std::move(msg));
  return Status::IOError(std::move(msg));
}
} // namespace

class PosixWritableFile : public WritableFile {
public:
  PosixWritableFile(int fd, std::string filename)
      : fd{fd}, filename{std::move(filename)} {}

  Status Append(std::string_view to) override {
    // we want the append to continue from where it left of.

    if (fd < 0) {
      return Status::IOError("append to closed file");
    }
    size_t remaining = to.size();
    const char *buffer = to.data();
    while (remaining > 0) {
      ssize_t bytes_written = ::write(fd, buffer, remaining);
      if (bytes_written > 0) {
        remaining -= static_cast<size_t>(bytes_written);
        buffer += bytes_written;
      } else if (bytes_written == -1 && errno == EINTR) {
        continue;
      }

      else if (bytes_written == -1) {
        return Status::IOError("failed to write to file.");
      } else {
        return Status::IOError("Failed to write on file.");
      }
    }
    return Status::OK();
  }

  Status Flush() override {
    if (fd >= 0) {
      return Status::OK();
    }
    return Status::IOError("flused on closed file");
  }
  Status Sync() override {
    if (fd >= 0) {
      while (::fsync(fd) == -1) {
        const int err = errno;
        if (err == EINTR) {
          continue;
        }
        return ErrnoToStatus("fsync " + filename, err);
      }
      return Status::OK();
    }
    return Status::IOError("synced on closed file");
  }
  Status Close() override {
    if (fd == -1) {
      return Status::OK();
    }
    auto closed = ::close(fd);
    if (closed == -1) {
      const int err = errno;
      return ErrnoToStatus("close " + filename, err);
    }
    fd = -1;
    return Status::OK();
  }
  PosixWritableFile(const PosixWritableFile &) = delete;
  PosixWritableFile &operator=(const PosixWritableFile &) = delete;
  ~PosixWritableFile() override {
    if (fd >= 0) {
      ::close(fd);
    }
  }
  PosixWritableFile(PosixWritableFile &&other) noexcept
      : fd(other.fd), filename(std::move(other.filename)) {
    other.fd = -1;
  }

  PosixWritableFile &operator=(PosixWritableFile &&other) noexcept {
    if (this != &other) {
      if (fd >= 0)
        ::close(fd);
      fd = other.fd;
      filename = std::move(other.filename);
      other.fd = -1;
    }
    return *this;
  }

private:
  int fd;
  std::string filename;
};

Status PosixFileSystem::NewWritableFile(const std::string &path,
                                        std::unique_ptr<WritableFile> *result) {
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd == -1) {
    if (errno == ENOENT) {
      return Status::IOError("Path or Parent directory does not exist.");
    }
    return (errno == EACCES) ? Status::IOError("Permission denied.")
                             : Status::IOError("Failed to open file.");
  }
  *result = std::make_unique<PosixWritableFile>(fd, path);
  return Status::OK();
}
Status PosixFileSystem::RenameFile(const std::string &from,
                                   const std::string &to) {}
Status PosixFileSystem::NewRandomAccessFile(
    const std::string &path, std::unique_ptr<RandomAccessFile> *result) {}
Status PosixFileSystem::DeleteFile(const std::string &path) {}
Status PosixFileSystem::Dirlist(const std::string &path,
                                std::vector<std::string> *names) const {}
Status PosixFileSystem::CreateDir(const std::string &path) {}
Status PosixFileSystem::FileSize(const std::string &path,
                                 uint64_t *size) const {}
bool PosixFileSystem::FileExists(const std::string &path) const {}
Status PosixFileSystem::SyncDir(const std::string &path) {}

} // namespace gritdb
