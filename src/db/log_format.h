#pragma once

namespace db {
namespace log {

enum RecordType {
  kZeroType = 0,
  kFullType = 1,

  kFirstType = 2,
  kMiddleType = 3,
  kLastType = 4
};

static const int kMaxRecordType = kLastType;

static const int kBlockSize = 32768;

static const int kHeaderSize = 4 + 2 + 1;

}  // namespace log
}  // namespace db