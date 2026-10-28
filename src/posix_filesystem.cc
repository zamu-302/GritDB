#include "posix_filesystem.h"
#include "filesystem.h"
#include "status.h"
#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
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
class PosixRandomAccessFile : public RandomAccessFile {
public:
  PosixRandomAccessFile(std::string filename, int fd)
      : fd_(fd), filename_(std::move(filename)) {}
  ~PosixRandomAccessFile() override { ::close(fd_); }
  PosixRandomAccessFile(const PosixRandomAccessFile &) = delete;
  PosixRandomAccessFile &operator=(const PosixRandomAccessFile &) = delete;

  Status Read(std::string_view *result, char *buff, size_t n,
              uint64_t offset) const override {
    if (offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
      return Status::InvalidArgument("offset too large: " + filename_);
    }
    size_t total = 0;
    while (total < n) {
      const ssize_t r = ::pread(fd_, buff + total, n - total,
                                static_cast<off_t>(offset + total));
      if (r < 0) {
        const int err = errno;
        if (err == EINTR)
          continue;
        return ErrnoToStatus("pread " + filename_, err);
      }
      if (r == 0)
        break; // end of file: a short read, not an error
      total += static_cast<size_t>(r);
    }
    *result = std::string_view(buff, total);
    return Status::OK();
  }

private:
  int fd_;
  std::string filename_;
};

} // namespace

Status PosixFileSystem::NewWritableFile(const std::string &path,
                                        std::unique_ptr<WritableFile> *result) {
  const int fd =
      ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd == -1) {
    const int err = errno; // capture before building any string
    return ErrnoToStatus("open " + path, err);
  }
  *result = std::make_unique<PosixWritableFile>(fd, path);
  return Status::OK();
}

Status PosixFileSystem::NewRandomAccessFile(
    const std::string &path, std::unique_ptr<RandomAccessFile> *result) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd == -1) {
    const int err = errno;
    return ErrnoToStatus("open " + path, err);
  }
  *result = std::make_unique<PosixRandomAccessFile>(path, fd);
  return Status::OK();
}

Status PosixFileSystem::RenameFile(const std::string &from,
                                   const std::string &to) {
  if (::rename(from.c_str(), to.c_str()) == -1) {
    const int err = errno;
    return ErrnoToStatus("rename " + from + " -> " + to, err);
  }
  return Status::OK();
}

Status PosixFileSystem::DeleteFile(const std::string &path) {
  if (::unlink(path.c_str()) == -1) {
    const int err = errno;
    return ErrnoToStatus("unlink " + path, err);
  }
  return Status::OK();
}

Status PosixFileSystem::Dirlist(const std::string &path,
                                std::vector<std::string> *names) const {
  DIR *dir = ::opendir(path.c_str());
  if (dir == nullptr) {
    const int err = errno;
    return ErrnoToStatus("opendir " + path, err);
  }
  std::unique_ptr<DIR, int (*)(DIR *)> guard(dir, &::closedir); // always closed

  std::vector<std::string> found;
  while (true) {
    errno = 0; // readdir returns null for both "end" and "error"
    const dirent *entry = ::readdir(dir);
    if (entry == nullptr) {
      const int err = errno;
      if (err != 0)
        return ErrnoToStatus("readdir " + path, err);
      break;
    }
    const std::string_view name = entry->d_name;
    if (name == "." || name == "..")
      continue;
    found.emplace_back(name);
  }
  std::sort(found.begin(), found.end()); // match Mem's ordering
  *names = std::move(found);             // output touched only on success
  return Status::OK();
}

Status PosixFileSystem::CreateDir(const std::string &path) {
  if (::mkdir(path.c_str(), 0755) == 0)
    return Status::OK();
  const int err = errno;
  if (err == EEXIST) { // idempotent for directories, error for files
    struct stat st;
    if (::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
      return Status::OK();
  }
  return ErrnoToStatus("mkdir " + path, err);
}

Status PosixFileSystem::FileSize(const std::string &path,
                                 uint64_t *size) const {
  struct stat st;
  if (::stat(path.c_str(), &st) == -1) {
    const int err = errno;
    return ErrnoToStatus("stat " + path, err);
  }
  if (!S_ISREG(st.st_mode)) { // matches Mem: directories aren't files
    return Status::NotFound("not a regular file: " + path);
  }
  *size = static_cast<uint64_t>(st.st_size);
  return Status::OK();
}

bool PosixFileSystem::FileExists(const std::string &path) const {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

Status PosixFileSystem::SyncDir(const std::string &path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd == -1) {
    const int err = errno;
    return ErrnoToStatus("open dir " + path, err);
  }
  int sync_err = 0;
  while (::fsync(fd) == -1) {
    if (errno == EINTR)
      continue;
    sync_err = errno;
    break;
  }
  ::close(fd);
  if (sync_err != 0)
    return ErrnoToStatus("fsync dir " + path, sync_err);
  return Status::OK();
}
} // namespace gritdb
