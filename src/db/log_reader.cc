#include "log_reader.h"

#include <cstddef>
#include <cstdint>
#include <string>

#include "env.h"
#include "log_format.h"
#include "slice.h"
#include "status.h"
#include "util/coding.h"
#include "util/crc32c.h"
namespace db {
namespace log {
Reader::Reporter::~Reporter() = default;

// why wal need a fixed 32kb block size

Reader::Reader(SequentialFile* file, Reporter* reporter, bool checksum, uint64_t initial_offset)
    : file_(file),
      reporter_(reporter),
      checksum_(checksum),
      backing_store_(new char[kBlockSize]),
      buffer_(),
      eof_(false),
      last_record_offset_(0),
      end_of_buffer_offset_(0),
      initial_offset_(initial_offset),
      resyncing_(initial_offset > 0) {}

Reader::~Reader() {
  delete[] backing_store_;
}

bool Reader::SkipToInitialBlock() {
  const size_t offset_in_block = initial_offset_ % kBlockSize;
  uint64_t block_start_location = initial_offset_ - offset_in_block;

  if (offset_in_block > kBlockSize - 6) {
    block_start_location += kBlockSize;
  }

  end_of_buffer_offset_ = block_start_location;

  if (block_start_location > 0) {
    Status skip_status = file_->Skip(block_start_location);
    if (!skip_status.Ok()) {
      ReportDrop(block_start_location, skip_status);
      return false;
    }
  }

  return true;
}

bool Reader::ReadRecord(Slice* record, std::string* scratch) {
  if (last_record_offset_ < initial_offset_) {
    if (!SkipToInitialBlock()) {
      return false;
    }
  }

  scratch->clear();
  record->Clear();
  bool in_fragmented_record = false;
  uint64_t prospective_record_offset = 0;

  Slice fragment;
  while (true) {
    const unsigned int record_type = ReadPhysocalRecord(&fragment);

    uint64_t physical_record_offset =
        end_of_buffer_offset_ - buffer_.Size() - kHeaderSize - fragment.Size();

    if (resyncing_) {
      if (record_type == kMiddleType) {
        continue;
      } else if (record_type == kLastType) {
        resyncing_ = false;
        continue;
      } else {
        resyncing_ = false;
      }
    }

    switch (record_type) {
      case kFullType:
        if (in_fragmented_record) {
          if (!scratch->empty()) {
            ReportCorruption(scratch->size(), "partial record without end(1)");
          }
        }
        prospective_record_offset = physical_record_offset;
        scratch->clear();
        *record = fragment;
        last_record_offset_ = prospective_record_offset;
        return true;
      case kFirstType:
        if (in_fragmented_record) {
          if (!scratch->empty()) {
            ReportCorruption(scratch->size(), "partial record without end(2)");
          }
        }
        prospective_record_offset = physical_record_offset;
        scratch->assign(fragment.Data(), fragment.Size());
        in_fragmented_record = true;
        break;
      case kMiddleType:
        if (!in_fragmented_record) {
          ReportCorruption(fragment.Size(), "missing start of fragmented record(1)");
        } else {
          scratch->append(fragment.Data(), fragment.Size());
        }
        break;
      case kLastType:
        if (!in_fragmented_record) {
          ReportCorruption(fragment.Size(), "missing start of fragmented record(2)");
        } else {
          scratch->append(fragment.Data(), fragment.Size());
          *record = Slice(*scratch);
          last_record_offset_ = prospective_record_offset;
          return true;
        }
        break;
      case kEof:
        if (in_fragmented_record) {
          scratch->clear();
        }
        return false;
      case kBadRecord:
        if (in_fragmented_record) {
          ReportCorruption(scratch->size(), "error in middle of record");
          in_fragmented_record = false;
          scratch->clear();
        }
        break;

      default:
        char buf[40];
        std::snprintf(buf, sizeof(buf), "unknown record type %u", record_type);
        ReportCorruption((fragment.Size() + (in_fragmented_record ? scratch->size() : 0)), buf);
        in_fragmented_record = false;
        scratch->clear();
        break;
    }
  }
}

unsigned int Reader::ReadPhysocalRecord(Slice* result) {
  while (true) {
    if (buffer_.Size() < kHeaderSize) {
      if (!eof_) {
        buffer_.Clear();
        Status status = file_->Read(kBlockSize, &buffer_, backing_store_);
        end_of_buffer_offset_ += buffer_.Size();
        if (!status.Ok()) {
          buffer_.Clear();
          ReportDrop(kBlockSize, status);
          eof_ = true;
          return kEof;
        } else if (buffer_.Size() < kBlockSize) {
          eof_ = true;
        }
        continue;
      } else {
        buffer_.Clear();
        return kEof;
      }
    }

    const char* header = buffer_.Data();
    const uint32_t a = static_cast<uint32_t>(header[4]) & 0xff;
    const uint32_t b = static_cast<uint32_t>(header[5]) & 0xff;
    const unsigned int type = header[6];
    const uint32_t length = a | (b << 8);
    if (kHeaderSize + length > buffer_.Size()) {
      size_t drop_size = buffer_.Size();
      buffer_.Clear();
      if (!eof_) {
        ReportCorruption(drop_size, "bad record length");
        return kBadRecord;
      }

      return kEof;
    }

    if (type == kZeroType && length == 0) {
      buffer_.Clear();
      return kBadRecord;
    }

    if (checksum_) {
      uint32_t expected_crc = crc32c::Unmask(DecodeFixed32(header));
      uint32_t actual_crc = crc32c::Value(header + 6, 1 + length);
      if (actual_crc != expected_crc) {
        size_t drop_size = buffer_.Size();
        buffer_.Clear();
        ReportCorruption(drop_size, "checksum mismatch");
        return kBadRecord;
      }
    }

    buffer_.RemovePrefix(kHeaderSize + length);

    if (end_of_buffer_offset_ - buffer_.Size() - kHeaderSize - length < initial_offset_) {
      result->Clear();
      return kBadRecord;
    }

    *result = Slice(header + kHeaderSize, length);
    return type;
  }
}

uint64_t Reader::LastRecordOffset() {
  return last_record_offset_;
}

void Reader::ReportCorruption(uint64_t bytes, const char* reason) {
  ReportDrop(bytes, Status::Corruption(reason));
}

void Reader::ReportDrop(uint64_t bytes, const Status& reason) {
  if (reporter_ != nullptr && end_of_buffer_offset_ - buffer_.Size() - bytes >= initial_offset_) {
    reporter_->Corruption(static_cast<size_t>(bytes), reason);
  }
}

}  // namespace log
}  // namespace db