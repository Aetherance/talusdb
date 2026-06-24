#pragma once

#include <string>

#include "slice.h"

namespace db {

class Status {
public:
  enum class Code {
    kOk = 0,
    kNotFound,
    kCorruption,
    kNotSupported,
    kInvalidArgument,
    kIOError,
  };

  Status() : code_(Code::kOk) {}

  static Status OkStatus() {
    return Status();
  }

  static Status NotFound(const Slice& message, const Slice& detail = Slice()) {
    return Status(Code::kNotFound, message, detail);
  }

  static Status Corruption(const Slice& message, const Slice& detail = Slice()) {
    return Status(Code::kCorruption, message, detail);
  }

  static Status NotSupported(const Slice& message, const Slice& detail = Slice()) {
    return Status(Code::kNotSupported, message, detail);
  }

  static Status InvalidArgument(const Slice& message, const Slice& detail = Slice()) {
    return Status(Code::kInvalidArgument, message, detail);
  }

  static Status IOError(const Slice& message, const Slice& detail = Slice()) {
    return Status(Code::kIOError, message, detail);
  }

  bool Ok() const {
    return code_ == Code::kOk;
  }

  bool IsNotFound() const {
    return code_ == Code::kNotFound;
  }

  Code GetCode() const {
    return code_;
  }

  std::string Message() const {
    return message_;
  }

  std::string ToString() const {
    if (Ok()) {
      return "OK";
    }

    if (message_.empty()) {
      return CodeString(code_);
    }

    return std::string(CodeString(code_)) + ": " + message_;
  }

private:
  Status(Code code, const Slice& message, const Slice& detail)
      : code_(code), message_(JoinMessage(message, detail)) {}

  static const char* CodeString(Code code) {
    switch (code) {
      case Code::kOk:
        return "OK";
      case Code::kNotFound:
        return "NotFound";
      case Code::kCorruption:
        return "Corruption";
      case Code::kNotSupported:
        return "NotSupported";
      case Code::kInvalidArgument:
        return "InvalidArgument";
      case Code::kIOError:
        return "IOError";
    }
    return "Unknown";
  }

  static std::string JoinMessage(const Slice& message, const Slice& detail) {
    if (detail.Empty()) {
      return message.ToString();
    }

    if (message.Empty()) {
      return detail.ToString();
    }

    return message.ToString() + ": " + detail.ToString();
  }

  Code code_;
  std::string message_;
};

inline bool operator==(const Status& lhs, const Status& rhs) {
  return lhs.GetCode() == rhs.GetCode() && lhs.Message() == rhs.Message();
}

inline bool operator!=(const Status& lhs, const Status& rhs) {
  return !(lhs == rhs);
}

}  // namespace db
