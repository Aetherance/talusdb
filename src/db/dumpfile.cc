#include "dumpfile.h"

#include <cstdint>
#include <cstdio>
#include <string>

#include "db/dbformat.h"
#include "db/filename.h"
#include "db/log_reader.h"
#include "db/version_edit.h"
#include "db/write_batch_internal.h"
#include "env.h"
#include "iterator.h"
#include "options.h"
#include "status.h"
#include "table.h"
#include "util/logging.h"
#include "write_batch.h"

namespace db {
namespace {

bool GuessType(const std::string& fname, FileType* type) {
  const size_t pos = fname.rfind('/');
  const std::string basename = pos == std::string::npos
                                   ? fname
                                   : std::string(fname.data() + pos + 1, fname.size() - pos - 1);
  uint64_t ignored = 0;
  return ParseFileName(basename, &ignored, type);
}

class CorruptionReporter : public log::Reader::Reporter {
public:
  explicit CorruptionReporter(WritableFile* dst) : dst_(dst) {}

  void Corruption(size_t bytes, const Status& status) override {
    std::string line = "corruption: ";
    AppendNumberTo(&line, bytes);
    line += " bytes; ";
    line += status.ToString();
    line.push_back('\n');
    dst_->Append(line);
  }

private:
  WritableFile* const dst_;
};

Status PrintLogContents(Env* env, const std::string& fname,
                        void (*func)(uint64_t, Slice, WritableFile*), WritableFile* dst) {
  SequentialFile* file = nullptr;
  Status status = env->NewSequentialFile(fname, &file);
  if (!status.Ok()) {
    return status;
  }

  CorruptionReporter reporter(dst);
  log::Reader reader(file, &reporter, true, 0);
  Slice record;
  std::string scratch;
  while (reader.ReadRecord(&record, &scratch)) {
    (*func)(reader.LastRecordOffset(), record, dst);
  }
  delete file;
  return Status::OkStatus();
}

class WriteBatchItemPrinter : public WriteBatch::Handler {
public:
  explicit WriteBatchItemPrinter(WritableFile* dst) : dst_(dst) {}

  void Put(const Slice& key, const Slice& value) override {
    std::string line = "  put '";
    AppendEscapedStringTo(&line, key);
    line += "' '";
    AppendEscapedStringTo(&line, value);
    line += "'\n";
    dst_->Append(line);
  }

  void Delete(const Slice& key) override {
    std::string line = "  del '";
    AppendEscapedStringTo(&line, key);
    line += "'\n";
    dst_->Append(line);
  }

private:
  WritableFile* const dst_;
};

void WriteBatchPrinter(uint64_t pos, Slice record, WritableFile* dst) {
  std::string line = "--- offset ";
  AppendNumberTo(&line, pos);
  line += "; ";
  if (record.Size() < 12) {
    line += "log record length ";
    AppendNumberTo(&line, record.Size());
    line += " is too small\n";
    dst->Append(line);
    return;
  }

  WriteBatch batch;
  WriteBatchInternal::SetContents(&batch, record);
  line += "sequence ";
  AppendNumberTo(&line, WriteBatchInternal::Sequence(&batch));
  line.push_back('\n');
  dst->Append(line);

  WriteBatchItemPrinter printer(dst);
  Status status = batch.Iterate(&printer);
  if (!status.Ok()) {
    dst->Append("  error: " + status.ToString() + "\n");
  }
}

Status DumpLog(Env* env, const std::string& fname, WritableFile* dst) {
  return PrintLogContents(env, fname, WriteBatchPrinter, dst);
}

void VersionEditPrinter(uint64_t pos, Slice record, WritableFile* dst) {
  std::string line = "--- offset ";
  AppendNumberTo(&line, pos);
  line += "; ";

  VersionEdit edit;
  Status status = edit.DecodeFrom(record);
  if (!status.Ok()) {
    line += status.ToString();
    line.push_back('\n');
  } else {
    line += edit.DebugString();
  }
  dst->Append(line);
}

Status DumpDescriptor(Env* env, const std::string& fname, WritableFile* dst) {
  return PrintLogContents(env, fname, VersionEditPrinter, dst);
}

Status DumpTable(Env* env, const std::string& fname, WritableFile* dst) {
  uint64_t file_size = 0;
  RandomAccessFile* file = nullptr;
  Table* table = nullptr;
  Status status = env->GetFileSize(fname, &file_size);
  if (status.Ok()) {
    status = env->NewRandomAccessFile(fname, &file);
  }
  if (status.Ok()) {
    status = Table::Open(Options(), file, file_size, &table);
  }
  if (!status.Ok()) {
    delete table;
    delete file;
    return status;
  }

  ReadOptions read_options;
  read_options.fill_cache = false;
  Iterator* iter = table->NewIterator(read_options);
  std::string line;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    line.clear();
    ParsedInternalKey key;
    if (!ParseInternalKey(iter->Key(), &key)) {
      line = "badkey '";
      AppendEscapedStringTo(&line, iter->Key());
      line += "' => '";
      AppendEscapedStringTo(&line, iter->Value());
      line += "'\n";
    } else {
      line = "'";
      AppendEscapedStringTo(&line, key.user_key);
      line += "' @ ";
      AppendNumberTo(&line, key.sequence);
      line += " : ";
      if (key.type == kTypeDeletion) {
        line += "del";
      } else if (key.type == kTypeValue) {
        line += "val";
      } else {
        AppendNumberTo(&line, key.type);
      }
      line += " => '";
      AppendEscapedStringTo(&line, iter->Value());
      line += "'\n";
    }
    dst->Append(line);
  }

  status = iter->GetStatus();
  if (!status.Ok()) {
    dst->Append("iterator error: " + status.ToString() + "\n");
  }

  delete iter;
  delete table;
  delete file;
  return Status::OkStatus();
}

}  // namespace

Status DumpFile(Env* env, const std::string& fname, WritableFile* dst) {
  FileType file_type;
  if (!GuessType(fname, &file_type)) {
    return Status::InvalidArgument(fname + ": unknown file type");
  }

  switch (file_type) {
    case kLogFile:
      return DumpLog(env, fname, dst);
    case kDescriptorFile:
      return DumpDescriptor(env, fname, dst);
    case kTableFile:
      return DumpTable(env, fname, dst);
    default:
      break;
  }
  return Status::InvalidArgument(fname + ": not a dump-able file type");
}

}  // namespace db
