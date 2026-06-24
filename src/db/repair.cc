#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "cache.h"
#include "db.h"
#include "db/builder.h"
#include "db/db_impl.h"
#include "db/dbformat.h"
#include "db/filename.h"
#include "db/log_reader.h"
#include "db/log_writer.h"
#include "db/memtable.h"
#include "db/table_cache.h"
#include "db/version_edit.h"
#include "db/write_batch_internal.h"
#include "env.h"
#include "iterator.h"
#include "table_builder.h"
#include "util/logging.h"
#include "write_batch.h"

namespace db {
namespace {

Env* RepairEnv(const Options& options) {
  return options.env != nullptr ? options.env : Env::Default();
}

const Comparator* RepairComparator(const Options& options) {
  return options.comparator != nullptr ? options.comparator : BytewiseComparator();
}

std::string LegacyTableFileName(const std::string& dbname, uint64_t number) {
  char buf[100];
  std::snprintf(buf, sizeof(buf), "/%06llu.ldb", static_cast<unsigned long long>(number));
  return dbname + buf;
}

class Repairer {
public:
  Repairer(const std::string& dbname, const Options& options)
      : dbname_(dbname),
        env_(RepairEnv(options)),
        icmp_(RepairComparator(options)),
        ipolicy_(options.filter_policy),
        options_(SanitizeOptions(dbname, &icmp_, &ipolicy_, options)),
        owns_info_log_(options_.info_log != options.info_log),
        owns_cache_(options_.block_cache != options.block_cache),
        table_cache_(new TableCache(dbname_, options_, 10)),
        next_file_number_(1) {}

  ~Repairer() {
    delete table_cache_;
    if (owns_info_log_) {
      delete options_.info_log;
    }
    if (owns_cache_) {
      delete options_.block_cache;
    }
  }

  Status Run() {
    Status status = FindFiles();
    if (status.Ok()) {
      ConvertLogFilesToTables();
      ExtractMetaData();
      status = WriteDescriptor();
    }
    if (status.Ok()) {
      uint64_t bytes = 0;
      for (const TableInfo& table : tables_) {
        bytes += table.meta.file_size;
      }
      Log(options_.info_log,
          "**** Repaired db %s; recovered %d files; %llu bytes. Some data may have been lost. ****",
          dbname_.c_str(), static_cast<int>(tables_.size()),
          static_cast<unsigned long long>(bytes));
    }
    return status;
  }

private:
  struct TableInfo {
    FileMetaData meta;
    SequenceNumber max_sequence;
  };

  Status FindFiles() {
    std::vector<std::string> filenames;
    Status status = env_->GetChildren(dbname_, &filenames);
    if (!status.Ok()) {
      return status;
    }
    if (filenames.empty()) {
      return Status::IOError(dbname_, "repair found no files");
    }

    uint64_t number = 0;
    FileType type = kLogFile;
    for (const std::string& filename : filenames) {
      if (ParseFileName(filename, &number, &type)) {
        if (type == kDescriptorFile) {
          manifests_.push_back(filename);
        } else {
          if (number + 1 > next_file_number_) {
            next_file_number_ = number + 1;
          }
          if (type == kLogFile) {
            logs_.push_back(number);
          } else if (type == kTableFile) {
            table_numbers_.push_back(number);
          }
        }
      }
    }
    return Status::OkStatus();
  }

  void ConvertLogFilesToTables() {
    for (uint64_t log_number : logs_) {
      const std::string log_name = LogFileName(dbname_, log_number);
      Status status = ConvertLogToTable(log_number);
      if (!status.Ok()) {
        Log(options_.info_log, "Log #%llu: ignoring conversion error: %s",
            static_cast<unsigned long long>(log_number), status.ToString().c_str());
      }
      ArchiveFile(log_name);
    }
  }

  Status ConvertLogToTable(uint64_t log_number) {
    struct LogReporter : public log::Reader::Reporter {
      Logger* info_log;
      uint64_t log_number;

      void Corruption(size_t bytes, const Status& status) override {
        Log(info_log, "Log #%llu: dropping %d bytes; %s",
            static_cast<unsigned long long>(log_number), static_cast<int>(bytes),
            status.ToString().c_str());
      }
    };

    const std::string log_name = LogFileName(dbname_, log_number);
    SequentialFile* log_file = nullptr;
    Status status = env_->NewSequentialFile(log_name, &log_file);
    if (!status.Ok()) {
      return status;
    }

    LogReporter reporter;
    reporter.info_log = options_.info_log;
    reporter.log_number = log_number;
    log::Reader reader(log_file, &reporter, false, 0);

    std::string scratch;
    Slice record;
    WriteBatch batch;
    MemTable* mem = new MemTable(icmp_);
    mem->Ref();
    int counter = 0;
    while (reader.ReadRecord(&record, &scratch)) {
      if (record.Size() < 12) {
        reporter.Corruption(record.Size(), Status::Corruption("log record too small"));
        continue;
      }
      WriteBatchInternal::SetContents(&batch, record);
      status = WriteBatchInternal::InsertInto(&batch, mem);
      if (status.Ok()) {
        counter += WriteBatchInternal::Count(&batch);
      } else {
        Log(options_.info_log, "Log #%llu: ignoring %s",
            static_cast<unsigned long long>(log_number), status.ToString().c_str());
        status = Status::OkStatus();
      }
    }
    delete log_file;

    FileMetaData meta;
    meta.number = next_file_number_++;
    Iterator* iter = mem->NewIterator();
    status = BuildTable(dbname_, env_, options_, table_cache_, iter, &meta);
    delete iter;
    mem->Unref();
    if (status.Ok() && meta.file_size > 0) {
      table_numbers_.push_back(meta.number);
    }

    Log(options_.info_log, "Log #%llu: %d ops saved to Table #%llu %s",
        static_cast<unsigned long long>(log_number), counter,
        static_cast<unsigned long long>(meta.number), status.ToString().c_str());
    return status;
  }

  void ExtractMetaData() {
    for (uint64_t table_number : table_numbers_) {
      ScanTable(table_number);
    }
  }

  Iterator* NewTableIterator(const FileMetaData& meta) {
    ReadOptions read_options;
    read_options.verify_checksums = options_.paranoid_checks;
    return table_cache_->NewIterator(read_options, meta.number, meta.file_size);
  }

  void ScanTable(uint64_t number) {
    TableInfo table;
    table.meta.number = number;
    table.max_sequence = 0;

    std::string table_name = TableFileName(dbname_, number);
    Status status = env_->GetFileSize(table_name, &table.meta.file_size);
    if (!status.Ok()) {
      const std::string legacy_name = LegacyTableFileName(dbname_, number);
      Status legacy_status = env_->GetFileSize(legacy_name, &table.meta.file_size);
      if (legacy_status.Ok()) {
        status = env_->RenameFile(legacy_name, table_name);
        if (status.Ok()) {
          status = env_->GetFileSize(table_name, &table.meta.file_size);
        }
      }
    }
    if (!status.Ok()) {
      ArchiveFile(TableFileName(dbname_, number));
      ArchiveFile(LegacyTableFileName(dbname_, number));
      Log(options_.info_log, "Table #%llu: dropped: %s", static_cast<unsigned long long>(number),
          status.ToString().c_str());
      return;
    }

    int counter = 0;
    Iterator* iter = NewTableIterator(table.meta);
    bool empty = true;
    ParsedInternalKey parsed;
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
      Slice key = iter->Key();
      if (!ParseInternalKey(key, &parsed)) {
        Log(options_.info_log, "Table #%llu: unparsable key %s",
            static_cast<unsigned long long>(number), EscapeString(key).c_str());
        continue;
      }

      counter++;
      if (empty) {
        empty = false;
        table.meta.smallest.DecodeFrom(key);
      }
      table.meta.largest.DecodeFrom(key);
      if (parsed.sequence > table.max_sequence) {
        table.max_sequence = parsed.sequence;
      }
    }
    if (!iter->GetStatus().Ok()) {
      status = iter->GetStatus();
    }
    delete iter;

    Log(options_.info_log, "Table #%llu: %d entries %s", static_cast<unsigned long long>(number),
        counter, status.ToString().c_str());

    if (status.Ok()) {
      tables_.push_back(table);
    } else {
      RepairTable(table_name, table);
    }
  }

  void RepairTable(const std::string& src, TableInfo table) {
    const std::string copy = TableFileName(dbname_, next_file_number_++);
    WritableFile* file = nullptr;
    Status status = env_->NewWritableFile(copy, &file);
    if (!status.Ok()) {
      return;
    }
    TableBuilder* builder = new TableBuilder(options_, file);

    Iterator* iter = NewTableIterator(table.meta);
    int counter = 0;
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
      builder->Add(iter->Key(), iter->Value());
      counter++;
    }
    delete iter;

    ArchiveFile(src);
    if (counter == 0) {
      builder->Abandon();
    } else {
      status = builder->Finish();
      if (status.Ok()) {
        table.meta.file_size = builder->FileSize();
      }
    }
    delete builder;

    if (status.Ok()) {
      status = file->Close();
    }
    delete file;

    if (counter > 0 && status.Ok()) {
      const std::string original = TableFileName(dbname_, table.meta.number);
      status = env_->RenameFile(copy, original);
      if (status.Ok()) {
        Log(options_.info_log, "Table #%llu: %d entries repaired",
            static_cast<unsigned long long>(table.meta.number), counter);
        tables_.push_back(table);
      }
    }
    if (!status.Ok()) {
      env_->RemoveFile(copy);
    }
  }

  Status WriteDescriptor() {
    const std::string tmp = TempFileName(dbname_, 1);
    WritableFile* file = nullptr;
    Status status = env_->NewWritableFile(tmp, &file);
    if (!status.Ok()) {
      return status;
    }

    SequenceNumber max_sequence = 0;
    for (const TableInfo& table : tables_) {
      if (max_sequence < table.max_sequence) {
        max_sequence = table.max_sequence;
      }
    }

    edit_.SetComparatorName(icmp_.user_comparator()->Name());
    edit_.SetLogNumber(0);
    edit_.SetNextFile(next_file_number_);
    edit_.SetLastSequence(max_sequence);
    for (const TableInfo& table : tables_) {
      edit_.AddFile(0, table.meta.number, table.meta.file_size, table.meta.smallest,
                    table.meta.largest);
    }

    {
      log::Writer log(file);
      std::string record;
      edit_.EncodeTo(&record);
      status = log.AddRecord(record);
    }
    if (status.Ok()) {
      status = file->Close();
    }
    delete file;

    if (!status.Ok()) {
      env_->RemoveFile(tmp);
      return status;
    }

    for (const std::string& manifest : manifests_) {
      ArchiveFile(dbname_ + "/" + manifest);
    }

    status = env_->RenameFile(tmp, DescriptorFileName(dbname_, 1));
    if (status.Ok()) {
      status = SetCurrentFile(env_, dbname_, 1);
    } else {
      env_->RemoveFile(tmp);
    }
    return status;
  }

  void ArchiveFile(const std::string& fname) {
    const char* slash = std::strrchr(fname.c_str(), '/');
    std::string new_dir;
    if (slash != nullptr) {
      new_dir.assign(fname.data(), static_cast<size_t>(slash - fname.data()));
    }
    new_dir.append("/lost");
    env_->CreateDir(new_dir);

    std::string new_file = new_dir;
    new_file.push_back('/');
    new_file.append(slash == nullptr ? fname.c_str() : slash + 1);
    Status status = env_->RenameFile(fname, new_file);
    Log(options_.info_log, "Archiving %s: %s\n", fname.c_str(), status.ToString().c_str());
  }

  const std::string dbname_;
  Env* const env_;
  const InternalKeyComparator icmp_;
  const InternalFilterPolicy ipolicy_;
  const Options options_;
  bool owns_info_log_;
  bool owns_cache_;
  TableCache* table_cache_;
  VersionEdit edit_;
  std::vector<std::string> manifests_;
  std::vector<uint64_t> table_numbers_;
  std::vector<uint64_t> logs_;
  std::vector<TableInfo> tables_;
  uint64_t next_file_number_;
};

}  // namespace

Status RepairDB(const std::string& dbname, const Options& options) {
  Repairer repairer(dbname, options);
  return repairer.Run();
}

}  // namespace db
