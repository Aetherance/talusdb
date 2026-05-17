#include <cstddef>
#include <cstdint>

#include "env.h"
#include "log_format.h"
#include "status.h"
namespace db {
class WritableFile;
namespace log {

class Writer {
public:
  explicit Writer(WritableFile* dest);

  Writer(WritableFile* dest, uint64_t dest_length);

  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;

  ~Writer();

  Status AddRecord(const Slice& slice);

private:
  Status EmitPhysicalRecord(RecordType type, const char* ptr, size_t length);

  WritableFile* dest_;
  int block_offset_;

  uint32_t type_crc_[kMaxRecordType + 1];
};
}  // namespace log
}  // namespace db
