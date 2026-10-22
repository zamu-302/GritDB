#include "../include/status.h"

namespace gritdb {

Status Status::NotFound(std::string msg) {
  return Status(Code::kNotFound, std::move(msg));
}
Status Status::Corruption(std::string msg) {
  return Status(Code::kCorruption, std::move(msg));
}
Status Status::IOError(std::string msg) {
  return Status(Code::kIOError, std::move(msg));
}
Status Status::InvalidArgument(std::string msg) {
  return Status(Code::kInvalidArgument, std::move(msg));
}

std::string Status::ToString() const {
  const char *name = "";
  switch (code_) {
  case Code::kOk:
    return "OK";
  case Code::kNotFound:
    name = "NotFound";
    break;
  case Code::kCorruption:
    name = "Corruption";
    break;
  case Code::kIOError:
    name = "IO error";
    break;
  case Code::kInvalidArgument:
    name = "Invalid argument";
    break;
  }
  return msg_.empty() ? std::string(name) : std::string(name) + ": " + msg_;
}

} // namespace gritdb
