#pragma once
#include <string>

namespace gritdb {

class [[nodiscard]] Status {
public:
  Status() : code_(Code::kOk) {}
  static Status OK() { return Status(); }
  static Status NotFound(std::string msg = {});
  static Status Corruption(std::string msg);
  static Status IOError(std::string msg);
  static Status InvalidArgument(std::string msg);

  bool ok() const { return code_ == Code::kOk; }
  bool IsNotFound() const { return code_ == Code::kNotFound; }
  bool IsCorruption() const { return code_ == Code::kCorruption; }
  bool IsIOError() const { return code_ == Code::kIOError; }

  std::string ToString() const;

private:
  enum class Code { kOk, kNotFound, kCorruption, kIOError, kInvalidArgument };

  Status(Code code, std::string msg) : code_(code), msg_(std::move(msg)) {}

  Code code_;
  std::string msg_;
};
} // namespace gritdb
