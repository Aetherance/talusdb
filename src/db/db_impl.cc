#include "db/db_impl.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <set>
#include <string>
#include <vector>

#include "cache.h"
#include "db/builder.h"
#include "db/db_iter.h"
#include "db/filename.h"
#include "db/log_reader.h"
#include "db/log_writer.h"
#include "db/memtable.h"
#include "db/table_cache.h"
#include "db/version_edit.h"
#include "db/version_set.h"
#include "db/write_batch_internal.h"
#include "env.h"
#include "table/merger.h"
#include "table_builder.h"
#include "util/logging.h"
#include "util/mutexlock.h"
#include "write_batch.h"

namespace db {

namespace {

constexpr int kNumNonTableCacheFiles = 10;

Env* OptionsEnv(const Options& options) {
  return options.env != nullptr ? options.env : Env::Default();
}

const Comparator* OptionsComparator(const Options& options) {
  return options.comparator != nullptr ? options.comparator : BytewiseComparator();
}

template <class T, class V>
void ClipToRange(T* ptr, V min_value, V max_value) {
  if (static_cast<V>(*ptr) > max_value) {
    *ptr = max_value;
  }
  if (static_cast<V>(*ptr) < min_value) {
    *ptr = min_value;
  }
}

int TableCacheSize(const Options& sanitized_options) {
  return sanitized_options.max_open_files - kNumNonTableCacheFiles;
}

struct IterState {
  port::Mutex* const mu;
  Version* const version GUARDED_BY(mu);
  MemTable* const mem GUARDED_BY(mu);
  MemTable* const imm GUARDED_BY(mu);

  IterState(port::Mutex* mutex, MemTable* memtable, MemTable* immutable, Version* current)
      : mu(mutex), version(current), mem(memtable), imm(immutable) {}
};

void CleanupIteratorState(void* arg1, void* arg2) {
  (void)arg2;
  IterState* state = reinterpret_cast<IterState*>(arg1);
  state->mu->Lock();
  state->mem->Unref();
  if (state->imm != nullptr) {
    state->imm->Unref();
  }
  state->version->Unref();
  state->mu->Unlock();
  delete state;
}

}  // namespace

struct DBImpl::Writer {
  explicit Writer(port::Mutex* mu) : batch(nullptr), sync(false), done(false), cv(mu) {}

  Status status;
  WriteBatch* batch;
  bool sync;
  bool done;
  port::CondVar cv;
};

struct DBImpl::CompactionState {
  struct Output {
    uint64_t number;
    uint64_t file_size;
    InternalKey smallest;
    InternalKey largest;
  };

  explicit CompactionState(Compaction* compaction)
      : compaction(compaction),
        smallest_snapshot(0),
        outfile(nullptr),
        builder(nullptr),
        total_bytes(0) {}

  Output* current_output() {
    return &outputs[outputs.size() - 1];
  }

  Compaction* const compaction;
  SequenceNumber smallest_snapshot;
  std::vector<Output> outputs;
  WritableFile* outfile;
  TableBuilder* builder;
  uint64_t total_bytes;
};

Options SanitizeOptions(const std::string& dbname, const InternalKeyComparator* icmp,
                        const InternalFilterPolicy* ipolicy, const Options& src) {
  Options result = src;
  result.env = OptionsEnv(src);
  result.comparator = icmp;
  result.filter_policy = (src.filter_policy != nullptr) ? ipolicy : nullptr;
  ClipToRange(&result.max_open_files, 64 + kNumNonTableCacheFiles, 50000);
  ClipToRange(&result.write_buffer_size, size_t{64} << 10, size_t{1} << 30);
  ClipToRange(&result.max_file_size, size_t{1} << 20, size_t{1} << 30);
  ClipToRange(&result.block_size, size_t{1} << 10, size_t{4} << 20);

  if (result.info_log == nullptr) {
    result.env->CreateDir(dbname);
    result.env->RenameFile(InfoLogFileName(dbname), OldInfoLogFileName(dbname));
    Status status = result.env->NewLogger(InfoLogFileName(dbname), &result.info_log);
    if (!status.Ok()) {
      result.info_log = nullptr;
    }
  }

  if (result.block_cache == nullptr) {
    result.block_cache = NewLRUCache(8 << 20);
  }
  return result;
}

DBImpl::DBImpl(const Options& raw_options, const std::string& dbname)
    : env_(OptionsEnv(raw_options)),
      internal_comparator_(OptionsComparator(raw_options)),
      internal_filter_policy_(raw_options.filter_policy),
      options_(
          SanitizeOptions(dbname, &internal_comparator_, &internal_filter_policy_, raw_options)),
      owns_info_log_(options_.info_log != raw_options.info_log),
      owns_cache_(options_.block_cache != raw_options.block_cache),
      dbname_(dbname),
      table_cache_(new TableCache(dbname_, options_, TableCacheSize(options_))),
      db_lock_(nullptr),
      shutting_down_(false),
      background_work_finished_signal_(&mutex_),
      mem_(nullptr),
      imm_(nullptr),
      has_imm_(false),
      logfile_(nullptr),
      logfile_number_(0),
      log_(nullptr),
      seed_(0),
      tmp_batch_(new WriteBatch),
      background_compaction_scheduled_(false),
      manual_compaction_(nullptr),
      versions_(new VersionSet(dbname_, &options_, table_cache_, &internal_comparator_)) {}

DBImpl::~DBImpl() {
  mutex_.Lock();
  shutting_down_.store(true, std::memory_order_release);
  while (background_compaction_scheduled_) {
    background_work_finished_signal_.Wait();
  }
  mutex_.Unlock();

  if (db_lock_ != nullptr) {
    env_->UnlockFile(db_lock_);
  }
  if (mem_ != nullptr) {
    mem_->Unref();
  }
  if (imm_ != nullptr) {
    imm_->Unref();
  }
  delete tmp_batch_;
  delete log_;
  delete logfile_;
  delete versions_;
  delete table_cache_;

  if (owns_info_log_) {
    delete options_.info_log;
  }
  if (owns_cache_) {
    delete options_.block_cache;
  }
}

Status DBImpl::Put(const WriteOptions& options, const Slice& key, const Slice& value) {
  return DB::Put(options, key, value);
}

Status DBImpl::Delete(const WriteOptions& options, const Slice& key) {
  return DB::Delete(options, key);
}

Status DBImpl::NewDB() {
  VersionEdit new_db;
  new_db.SetComparatorName(user_comparator()->Name());
  new_db.SetLogNumber(0);
  new_db.SetNextFile(2);
  new_db.SetLastSequence(0);

  const std::string manifest = DescriptorFileName(dbname_, 1);
  WritableFile* file = nullptr;
  Status status = env_->NewWritableFile(manifest, &file);
  if (!status.Ok()) {
    return status;
  }

  {
    log::Writer log(file);
    std::string record;
    new_db.EncodeTo(&record);
    status = log.AddRecord(record);
    if (status.Ok()) {
      status = file->Sync();
    }
    if (status.Ok()) {
      status = file->Close();
    }
  }
  delete file;

  if (status.Ok()) {
    status = SetCurrentFile(env_, dbname_, 1);
  } else {
    env_->RemoveFile(manifest);
  }
  return status;
}

void DBImpl::MaybeIgnoreError(Status* status) const {
  if (status->Ok() || options_.paranoid_checks) {
    return;
  }

  Log(options_.info_log, "Ignoring error %s", status->ToString().c_str());
  *status = Status::OkStatus();
}

void DBImpl::RemoveObsoleteFiles() {
  mutex_.AssertHeld();

  if (!bg_error_.Ok()) {
    return;
  }

  std::set<uint64_t> live = pending_outputs_;
  versions_->AddLiveFiles(&live);

  std::vector<std::string> filenames;
  env_->GetChildren(dbname_, &filenames);
  std::vector<std::string> files_to_delete;
  uint64_t number = 0;
  FileType type = kLogFile;
  for (const std::string& filename : filenames) {
    if (!ParseFileName(filename, &number, &type)) {
      continue;
    }

    bool keep = true;
    switch (type) {
      case kLogFile:
        keep = (number >= versions_->LogNumber()) || (number == versions_->PrevLogNumber());
        break;
      case kDescriptorFile:
        keep = number >= versions_->ManifestFileNumber();
        break;
      case kTableFile:
        keep = live.find(number) != live.end();
        break;
      case kTempFile:
        keep = live.find(number) != live.end();
        break;
      case kCurrentFile:
      case kDBLockFile:
      case kInfoLogFile:
        keep = true;
        break;
    }

    if (!keep) {
      files_to_delete.push_back(filename);
      if (type == kTableFile) {
        table_cache_->Evict(number);
      }
      Log(options_.info_log, "Delete type=%d #%llu\n", static_cast<int>(type),
          static_cast<unsigned long long>(number));
    }
  }

  mutex_.Unlock();
  for (const std::string& filename : files_to_delete) {
    env_->RemoveFile(dbname_ + "/" + filename);
  }
  mutex_.Lock();
}

Status DBImpl::Recover(VersionEdit* edit, bool* save_manifest) {
  mutex_.AssertHeld();

  env_->CreateDir(dbname_);
  assert(db_lock_ == nullptr);
  Status status = env_->LockFile(LockFileName(dbname_), &db_lock_);
  if (!status.Ok()) {
    return status;
  }

  if (!env_->FileExists(CurrentFileName(dbname_))) {
    if (options_.create_if_missing) {
      Log(options_.info_log, "Creating DB %s since it was missing.", dbname_.c_str());
      status = NewDB();
      if (!status.Ok()) {
        return status;
      }
    } else {
      return Status::InvalidArgument(dbname_, "does not exist (create_if_missing is false)");
    }
  } else if (options_.error_if_exists) {
    return Status::InvalidArgument(dbname_, "exists (error_if_exists is true)");
  }

  status = versions_->Recover(save_manifest);
  if (!status.Ok()) {
    return status;
  }

  SequenceNumber max_sequence = 0;
  const uint64_t min_log = versions_->LogNumber();
  const uint64_t prev_log = versions_->PrevLogNumber();

  std::vector<std::string> filenames;
  status = env_->GetChildren(dbname_, &filenames);
  if (!status.Ok()) {
    return status;
  }

  std::set<uint64_t> expected;
  versions_->AddLiveFiles(&expected);
  uint64_t number = 0;
  FileType type = kLogFile;
  std::vector<uint64_t> logs;
  for (const std::string& filename : filenames) {
    if (ParseFileName(filename, &number, &type)) {
      expected.erase(number);
      if (type == kLogFile && (number >= min_log || number == prev_log)) {
        logs.push_back(number);
      }
    }
  }

  if (!expected.empty()) {
    char buf[50];
    std::snprintf(buf, sizeof(buf), "%d missing files; e.g.", static_cast<int>(expected.size()));
    return Status::Corruption(buf, TableFileName(dbname_, *expected.begin()));
  }

  std::sort(logs.begin(), logs.end());
  for (size_t i = 0; i < logs.size(); i++) {
    status = RecoverLogFile(logs[i], i == logs.size() - 1, save_manifest, edit, &max_sequence);
    if (!status.Ok()) {
      return status;
    }
    versions_->MarkFileNumberUsed(logs[i]);
  }

  if (versions_->LastSequence() < max_sequence) {
    versions_->SetLastSequence(max_sequence);
  }
  return Status::OkStatus();
}

Status DBImpl::RecoverLogFile(uint64_t log_number, bool last_log, bool* save_manifest,
                              VersionEdit* edit, SequenceNumber* max_sequence) {
  struct LogReporter : public log::Reader::Reporter {
    Logger* info_log;
    const char* filename;
    Status* status;

    void Corruption(size_t bytes, const Status& corruption) override {
      Log(info_log, "%s%s: dropping %d bytes; %s", status == nullptr ? "(ignoring error) " : "",
          filename, static_cast<int>(bytes), corruption.ToString().c_str());
      if (status != nullptr && status->Ok()) {
        *status = corruption;
      }
    }
  };

  mutex_.AssertHeld();

  const std::string filename = LogFileName(dbname_, log_number);
  SequentialFile* file = nullptr;
  Status status = env_->NewSequentialFile(filename, &file);
  if (!status.Ok()) {
    MaybeIgnoreError(&status);
    return status;
  }

  LogReporter reporter;
  reporter.info_log = options_.info_log;
  reporter.filename = filename.c_str();
  reporter.status = options_.paranoid_checks ? &status : nullptr;
  log::Reader reader(file, &reporter, true, 0);
  Log(options_.info_log, "Recovering log #%llu", static_cast<unsigned long long>(log_number));

  std::string scratch;
  Slice record;
  WriteBatch batch;
  int compactions = 0;
  MemTable* mem = nullptr;
  while (reader.ReadRecord(&record, &scratch) && status.Ok()) {
    if (record.Size() < 12) {
      reporter.Corruption(record.Size(), Status::Corruption("log record too small"));
      continue;
    }

    WriteBatchInternal::SetContents(&batch, record);
    if (mem == nullptr) {
      mem = new MemTable(internal_comparator_);
      mem->Ref();
    }

    status = WriteBatchInternal::InsertInto(&batch, mem);
    MaybeIgnoreError(&status);
    if (!status.Ok()) {
      break;
    }

    const SequenceNumber last_sequence =
        WriteBatchInternal::Sequence(&batch) + WriteBatchInternal::Count(&batch) - 1;
    if (last_sequence > *max_sequence) {
      *max_sequence = last_sequence;
    }

    if (mem->ApproximateMemoryUsage() > options_.write_buffer_size) {
      compactions++;
      *save_manifest = true;
      status = WriteLevel0Table(mem, edit, nullptr);
      mem->Unref();
      mem = nullptr;
      if (!status.Ok()) {
        break;
      }
    }
  }

  delete file;

  if (status.Ok() && options_.reuse_logs && last_log && compactions == 0) {
    assert(logfile_ == nullptr);
    assert(log_ == nullptr);
    assert(mem_ == nullptr);
    uint64_t log_file_size = 0;
    if (env_->GetFileSize(filename, &log_file_size).Ok() &&
        env_->NewAppendableFile(filename, &logfile_).Ok()) {
      Log(options_.info_log, "Reusing old log %s\n", filename.c_str());
      log_ = new log::Writer(logfile_, log_file_size);
      logfile_number_ = log_number;
      if (mem != nullptr) {
        mem_ = mem;
        mem = nullptr;
      } else {
        mem_ = new MemTable(internal_comparator_);
        mem_->Ref();
      }
    }
  }

  if (mem != nullptr) {
    if (status.Ok()) {
      *save_manifest = true;
      status = WriteLevel0Table(mem, edit, nullptr);
    }
    mem->Unref();
  }
  return status;
}

Status DBImpl::WriteLevel0Table(MemTable* mem, VersionEdit* edit, Version* base) {
  mutex_.AssertHeld();
  const uint64_t start_micros = env_->NowMicros();

  FileMetaData meta;
  meta.number = versions_->NewFileNumber();
  pending_outputs_.insert(meta.number);

  Iterator* iter = mem->NewIterator();
  Log(options_.info_log, "Level-0 table #%llu: started",
      static_cast<unsigned long long>(meta.number));

  Status status;
  {
    mutex_.Unlock();
    status = BuildTable(dbname_, env_, options_, table_cache_, iter, &meta);
    mutex_.Lock();
  }

  Log(options_.info_log, "Level-0 table #%llu: %llu bytes %s",
      static_cast<unsigned long long>(meta.number), static_cast<unsigned long long>(meta.file_size),
      status.ToString().c_str());
  delete iter;
  pending_outputs_.erase(meta.number);

  int level = 0;
  if (status.Ok() && meta.file_size > 0) {
    const Slice min_user_key = meta.smallest.user_key();
    const Slice max_user_key = meta.largest.user_key();
    if (base != nullptr) {
      level = base->PickLevelForMemTableOutput(min_user_key, max_user_key);
    }
    edit->AddFile(level, meta.number, meta.file_size, meta.smallest, meta.largest);
  }

  CompactionStats stats;
  stats.micros = static_cast<int64_t>(env_->NowMicros() - start_micros);
  stats.bytes_written = static_cast<int64_t>(meta.file_size);
  stats_[level].Add(stats);
  return status;
}

void DBImpl::CompactMemTable() {
  mutex_.AssertHeld();
  assert(imm_ != nullptr);

  VersionEdit edit;
  Version* base = versions_->current();
  base->Ref();
  Status status = WriteLevel0Table(imm_, &edit, base);
  base->Unref();

  if (status.Ok()) {
    edit.SetPrevLogNumber(0);
    edit.SetLogNumber(logfile_number_);
    status = versions_->LogAndApply(&edit, &mutex_);
  }

  if (status.Ok()) {
    imm_->Unref();
    imm_ = nullptr;
    has_imm_.store(false, std::memory_order_release);
    RemoveObsoleteFiles();
  } else {
    RecordBackgroundError(status);
  }
}

Status DBImpl::Write(const WriteOptions& options, WriteBatch* updates) {
  Writer writer(&mutex_);
  writer.batch = updates;
  writer.sync = options.sync;
  writer.done = false;

  MutexLock lock(&mutex_);
  writers_.push_back(&writer);
  while (!writer.done && &writer != writers_.front()) {
    writer.cv.Wait();
  }
  if (writer.done) {
    return writer.status;
  }

  Status status = MakeRoomForWrite(updates == nullptr);
  uint64_t last_sequence = versions_->LastSequence();
  Writer* last_writer = &writer;
  if (status.Ok() && updates != nullptr) {
    WriteBatch* write_batch = BuildBatchGroup(&last_writer);
    WriteBatchInternal::SetSequence(write_batch, last_sequence + 1);
    last_sequence += WriteBatchInternal::Count(write_batch);

    {
      mutex_.Unlock();
      status = log_->AddRecord(WriteBatchInternal::Contents(write_batch));
      bool sync_error = false;
      if (status.Ok() && options.sync) {
        status = logfile_->Sync();
        if (!status.Ok()) {
          sync_error = true;
        }
      }
      if (status.Ok()) {
        status = WriteBatchInternal::InsertInto(write_batch, mem_);
      }
      mutex_.Lock();

      if (sync_error) {
        RecordBackgroundError(status);
      }
    }

    if (write_batch == tmp_batch_) {
      tmp_batch_->Clear();
    }
    versions_->SetLastSequence(last_sequence);
  } else if (status.Ok() && updates == nullptr) {
    while (imm_ != nullptr && bg_error_.Ok() && !shutting_down_.load(std::memory_order_acquire)) {
      background_work_finished_signal_.Wait();
    }
    if (imm_ != nullptr) {
      status = bg_error_;
    }
  }

  while (true) {
    Writer* ready = writers_.front();
    writers_.pop_front();
    if (ready != &writer) {
      ready->status = status;
      ready->done = true;
      ready->cv.Signal();
    }
    if (ready == last_writer) {
      break;
    }
  }

  if (!writers_.empty()) {
    writers_.front()->cv.Signal();
  }
  return status;
}

WriteBatch* DBImpl::BuildBatchGroup(Writer** last_writer) {
  mutex_.AssertHeld();
  assert(!writers_.empty());
  Writer* first = writers_.front();
  WriteBatch* result = first->batch;
  assert(result != nullptr);

  size_t size = WriteBatchInternal::ByteSize(first->batch);
  size_t max_size = 1 << 20;
  if (size <= (128 << 10)) {
    max_size = size + (128 << 10);
  }

  *last_writer = first;
  auto iter = writers_.begin();
  ++iter;
  for (; iter != writers_.end(); ++iter) {
    Writer* writer = *iter;
    if (writer->sync && !first->sync) {
      break;
    }

    if (writer->batch != nullptr) {
      size += WriteBatchInternal::ByteSize(writer->batch);
      if (size > max_size) {
        break;
      }

      if (result == first->batch) {
        result = tmp_batch_;
        assert(WriteBatchInternal::Count(result) == 0);
        WriteBatchInternal::Append(result, first->batch);
      }
      WriteBatchInternal::Append(result, writer->batch);
    }
    *last_writer = writer;
  }
  return result;
}

Status DBImpl::MakeRoomForWrite(bool force) {
  mutex_.AssertHeld();
  assert(!writers_.empty());
  assert(mem_ != nullptr);

  bool allow_delay = !force;
  Status status;
  while (true) {
    if (!bg_error_.Ok()) {
      status = bg_error_;
      break;
    }

    if (allow_delay && versions_->NumLevelFiles(0) >= config::kL0SlowdownWritesTrigger) {
      mutex_.Unlock();
      env_->SleepForMicroseconds(1000);
      allow_delay = false;
      mutex_.Lock();
      continue;
    }

    if (!force && mem_->ApproximateMemoryUsage() <= options_.write_buffer_size) {
      break;
    }

    if (imm_ != nullptr) {
      Log(options_.info_log, "Current memtable full; waiting...\n");
      background_work_finished_signal_.Wait();
      continue;
    }

    if (versions_->NumLevelFiles(0) >= config::kL0_StopWritesTrigger) {
      Log(options_.info_log, "Too many L0 files; waiting...\n");
      background_work_finished_signal_.Wait();
      continue;
    }

    assert(imm_ == nullptr);
    assert(versions_->PrevLogNumber() == 0);
    const uint64_t new_log_number = versions_->NewFileNumber();
    WritableFile* new_log_file = nullptr;
    status = env_->NewWritableFile(LogFileName(dbname_, new_log_number), &new_log_file);
    if (!status.Ok()) {
      versions_->ReuseFileNumber(new_log_number);
      break;
    }

    delete log_;
    log_ = nullptr;

    if (logfile_ != nullptr) {
      Status close_status = logfile_->Close();
      if (!close_status.Ok()) {
        RecordBackgroundError(close_status);
      }
      delete logfile_;
    }

    logfile_ = new_log_file;
    logfile_number_ = new_log_number;
    log_ = new log::Writer(logfile_);
    imm_ = mem_;
    has_imm_.store(true, std::memory_order_release);
    mem_ = new MemTable(internal_comparator_);
    mem_->Ref();
    force = false;
    MaybeScheduleCompaction();
  }
  return status;
}

void DBImpl::RecordBackgroundError(const Status& status) {
  mutex_.AssertHeld();
  if (bg_error_.Ok()) {
    bg_error_ = status;
    background_work_finished_signal_.SignalAll();
  }
}

void DBImpl::MaybeScheduleCompaction() {
  mutex_.AssertHeld();
  if (background_compaction_scheduled_) {
  } else if (shutting_down_.load(std::memory_order_acquire)) {
  } else if (!bg_error_.Ok()) {
  } else if (imm_ == nullptr && manual_compaction_ == nullptr && !versions_->NeedsCompaction()) {
  } else {
    background_compaction_scheduled_ = true;
    env_->Schedule(&DBImpl::BGWork, this);
  }
}

void DBImpl::BGWork(void* db) {
  reinterpret_cast<DBImpl*>(db)->BackgroundCall();
}

void DBImpl::BackgroundCall() {
  MutexLock lock(&mutex_);
  assert(background_compaction_scheduled_);
  if (shutting_down_.load(std::memory_order_acquire)) {
  } else if (!bg_error_.Ok()) {
  } else {
    BackgroundCompaction();
  }

  background_compaction_scheduled_ = false;
  MaybeScheduleCompaction();
  background_work_finished_signal_.SignalAll();
}

void DBImpl::BackgroundCompaction() {
  mutex_.AssertHeld();

  if (imm_ != nullptr) {
    CompactMemTable();
    return;
  }

  Compaction* compaction = nullptr;
  const bool is_manual = manual_compaction_ != nullptr;
  InternalKey manual_end;
  if (is_manual) {
    ManualCompaction* manual = manual_compaction_;
    compaction = versions_->CompactRange(manual->level, manual->begin, manual->end);
    manual->done = compaction == nullptr;
    if (compaction != nullptr) {
      manual_end = compaction->input(0, compaction->num_input_files(0) - 1)->largest;
    }
    Log(options_.info_log, "Manual compaction at level-%d from %s .. %s; will stop at %s\n",
        manual->level, manual->begin ? manual->begin->DebugString().c_str() : "(begin)",
        manual->end ? manual->end->DebugString().c_str() : "(end)",
        manual->done ? "(end)" : manual_end.DebugString().c_str());
  } else {
    compaction = versions_->PickCompaction();
  }

  Status status;
  if (compaction == nullptr) {
  } else if (!is_manual && compaction->IsTrivialMove()) {
    assert(compaction->num_input_files(0) == 1);
    FileMetaData* file = compaction->input(0, 0);
    compaction->edit()->RemoveFile(compaction->level(), file->number);
    compaction->edit()->AddFile(compaction->level() + 1, file->number, file->file_size,
                                file->smallest, file->largest);
    status = versions_->LogAndApply(compaction->edit(), &mutex_);
    if (!status.Ok()) {
      RecordBackgroundError(status);
    }
    VersionSet::LevelSummaryStorage tmp;
    Log(options_.info_log, "Moved #%llu to level-%d %llu bytes %s: %s\n",
        static_cast<unsigned long long>(file->number), compaction->level() + 1,
        static_cast<unsigned long long>(file->file_size), status.ToString().c_str(),
        versions_->LevelSummary(&tmp));
  } else {
    CompactionState* compact = new CompactionState(compaction);
    status = DoCompactionWork(compact);
    if (!status.Ok()) {
      RecordBackgroundError(status);
    }
    CleanupCompaction(compact);
    compaction->ReleaseInputs();
    RemoveObsoleteFiles();
  }
  delete compaction;

  if (status.Ok()) {
  } else if (shutting_down_.load(std::memory_order_acquire)) {
  } else {
    Log(options_.info_log, "Compaction error: %s", status.ToString().c_str());
  }

  if (is_manual) {
    ManualCompaction* manual = manual_compaction_;
    if (!status.Ok()) {
      manual->done = true;
    }
    if (!manual->done) {
      manual->tmp_storage = manual_end;
      manual->begin = &manual->tmp_storage;
    }
    manual_compaction_ = nullptr;
  }
}

void DBImpl::CleanupCompaction(CompactionState* compact) {
  mutex_.AssertHeld();
  if (compact->builder != nullptr) {
    compact->builder->Abandon();
    delete compact->builder;
  } else {
    assert(compact->outfile == nullptr);
  }
  delete compact->outfile;
  for (size_t i = 0; i < compact->outputs.size(); i++) {
    pending_outputs_.erase(compact->outputs[i].number);
  }
  delete compact;
}

Status DBImpl::OpenCompactionOutputFile(CompactionState* compact) {
  assert(compact != nullptr);
  assert(compact->builder == nullptr);

  uint64_t file_number = 0;
  {
    mutex_.Lock();
    file_number = versions_->NewFileNumber();
    pending_outputs_.insert(file_number);
    CompactionState::Output output;
    output.number = file_number;
    output.file_size = 0;
    output.smallest.Clear();
    output.largest.Clear();
    compact->outputs.push_back(output);
    mutex_.Unlock();
  }

  const std::string filename = TableFileName(dbname_, file_number);
  Status status = env_->NewWritableFile(filename, &compact->outfile);
  if (status.Ok()) {
    compact->builder = new TableBuilder(options_, compact->outfile);
  }
  return status;
}

Status DBImpl::FinishCompactionOutputFile(CompactionState* compact, Iterator* input) {
  assert(compact != nullptr);
  assert(compact->outfile != nullptr);
  assert(compact->builder != nullptr);

  const uint64_t output_number = compact->current_output()->number;
  assert(output_number != 0);

  Status status = input->GetStatus();
  const uint64_t current_entries = compact->builder->NumEntries();
  if (status.Ok()) {
    status = compact->builder->Finish();
  } else {
    compact->builder->Abandon();
  }
  const uint64_t current_bytes = compact->builder->FileSize();
  compact->current_output()->file_size = current_bytes;
  compact->total_bytes += current_bytes;
  delete compact->builder;
  compact->builder = nullptr;

  if (status.Ok()) {
    status = compact->outfile->Sync();
  }
  if (status.Ok()) {
    status = compact->outfile->Close();
  }
  delete compact->outfile;
  compact->outfile = nullptr;

  if (status.Ok() && current_entries > 0) {
    Iterator* iter = table_cache_->NewIterator(ReadOptions(), output_number, current_bytes);
    status = iter->GetStatus();
    delete iter;
    if (status.Ok()) {
      Log(options_.info_log, "Generated table #%llu@%d: %llu keys, %llu bytes",
          static_cast<unsigned long long>(output_number), compact->compaction->level(),
          static_cast<unsigned long long>(current_entries),
          static_cast<unsigned long long>(current_bytes));
    }
  }
  return status;
}

Status DBImpl::InstallCompactionResults(CompactionState* compact) {
  mutex_.AssertHeld();
  Log(options_.info_log, "Compacted %d@%d + %d@%d files => %llu bytes",
      compact->compaction->num_input_files(0), compact->compaction->level(),
      compact->compaction->num_input_files(1), compact->compaction->level() + 1,
      static_cast<unsigned long long>(compact->total_bytes));

  compact->compaction->AddInputDeletions(compact->compaction->edit());
  const int level = compact->compaction->level();
  for (size_t i = 0; i < compact->outputs.size(); i++) {
    const CompactionState::Output& output = compact->outputs[i];
    compact->compaction->edit()->AddFile(level + 1, output.number, output.file_size,
                                         output.smallest, output.largest);
  }
  return versions_->LogAndApply(compact->compaction->edit(), &mutex_);
}

Status DBImpl::DoCompactionWork(CompactionState* compact) {
  const uint64_t start_micros = env_->NowMicros();
  int64_t imm_micros = 0;

  Log(options_.info_log, "Compacting %d@%d + %d@%d files", compact->compaction->num_input_files(0),
      compact->compaction->level(), compact->compaction->num_input_files(1),
      compact->compaction->level() + 1);

  assert(versions_->NumLevelFiles(compact->compaction->level()) > 0);
  assert(compact->builder == nullptr);
  assert(compact->outfile == nullptr);
  if (snapshots_.empty()) {
    compact->smallest_snapshot = versions_->LastSequence();
  } else {
    compact->smallest_snapshot = snapshots_.oldest()->sequence_number();
  }

  Iterator* input = versions_->MakeInputIterator(compact->compaction);
  mutex_.Unlock();

  input->SeekToFirst();
  Status status;
  ParsedInternalKey ikey;
  std::string current_user_key;
  bool has_current_user_key = false;
  SequenceNumber last_sequence_for_key = kMaxSequenceNumber;
  while (input->Valid() && !shutting_down_.load(std::memory_order_acquire)) {
    if (has_imm_.load(std::memory_order_relaxed)) {
      const uint64_t imm_start = env_->NowMicros();
      mutex_.Lock();
      if (imm_ != nullptr) {
        CompactMemTable();
        background_work_finished_signal_.SignalAll();
      }
      mutex_.Unlock();
      imm_micros += static_cast<int64_t>(env_->NowMicros() - imm_start);
    }

    Slice key = input->Key();
    if (compact->compaction->ShouldStopBefore(key) && compact->builder != nullptr) {
      status = FinishCompactionOutputFile(compact, input);
      if (!status.Ok()) {
        break;
      }
    }

    bool drop = false;
    if (!ParseInternalKey(key, &ikey)) {
      current_user_key.clear();
      has_current_user_key = false;
      last_sequence_for_key = kMaxSequenceNumber;
    } else {
      if (!has_current_user_key ||
          user_comparator()->Compare(ikey.user_key, Slice(current_user_key)) != 0) {
        current_user_key.assign(ikey.user_key.Data(), ikey.user_key.Size());
        has_current_user_key = true;
        last_sequence_for_key = kMaxSequenceNumber;
      }

      if (last_sequence_for_key <= compact->smallest_snapshot) {
        drop = true;
      } else if (ikey.type == kTypeDeletion && ikey.sequence <= compact->smallest_snapshot &&
                 compact->compaction->IsBaseLevelForKey(ikey.user_key)) {
        drop = true;
      }
      last_sequence_for_key = ikey.sequence;
    }

    if (!drop) {
      if (compact->builder == nullptr) {
        status = OpenCompactionOutputFile(compact);
        if (!status.Ok()) {
          break;
        }
      }
      if (compact->builder->NumEntries() == 0) {
        compact->current_output()->smallest.DecodeFrom(key);
      }
      compact->current_output()->largest.DecodeFrom(key);
      compact->builder->Add(key, input->Value());

      if (compact->builder->FileSize() >= compact->compaction->MaxOutputFileSize()) {
        status = FinishCompactionOutputFile(compact, input);
        if (!status.Ok()) {
          break;
        }
      }
    }

    input->Next();
  }

  if (status.Ok() && shutting_down_.load(std::memory_order_acquire)) {
    status = Status::IOError("Deleting DB during compaction");
  }
  if (status.Ok() && compact->builder != nullptr) {
    status = FinishCompactionOutputFile(compact, input);
  }
  if (status.Ok()) {
    status = input->GetStatus();
  }
  delete input;

  CompactionStats stats;
  stats.micros = static_cast<int64_t>(env_->NowMicros() - start_micros) - imm_micros;
  for (int which = 0; which < 2; which++) {
    for (int i = 0; i < compact->compaction->num_input_files(which); i++) {
      stats.bytes_read += static_cast<int64_t>(compact->compaction->input(which, i)->file_size);
    }
  }
  for (size_t i = 0; i < compact->outputs.size(); i++) {
    stats.bytes_written += static_cast<int64_t>(compact->outputs[i].file_size);
  }

  mutex_.Lock();
  stats_[compact->compaction->level() + 1].Add(stats);

  if (status.Ok()) {
    status = InstallCompactionResults(compact);
  }
  if (!status.Ok()) {
    RecordBackgroundError(status);
  }
  VersionSet::LevelSummaryStorage tmp;
  Log(options_.info_log, "compacted to: %s", versions_->LevelSummary(&tmp));
  return status;
}

Status DBImpl::Get(const ReadOptions& options, const Slice& key, std::string* value) {
  Status status;
  MutexLock lock(&mutex_);
  SequenceNumber snapshot = versions_->LastSequence();
  if (options.snapshot != nullptr) {
    snapshot = static_cast<const SnapshotImpl*>(options.snapshot)->sequence_number();
  }

  MemTable* mem = mem_;
  MemTable* imm = imm_;
  Version* current = versions_->current();
  mem->Ref();
  if (imm != nullptr) {
    imm->Ref();
  }
  current->Ref();

  bool have_stat_update = false;
  Version::GetStats stats;

  {
    mutex_.Unlock();
    LookupKey lookup_key(key, snapshot);
    if (mem->Get(lookup_key, value, &status)) {
    } else if (imm != nullptr && imm->Get(lookup_key, value, &status)) {
    } else {
      status = current->Get(options, lookup_key, value, &stats);
      have_stat_update = true;
    }
    mutex_.Lock();
  }

  if (have_stat_update && current->UpdateStats(stats)) {
    MaybeScheduleCompaction();
  }
  mem->Unref();
  if (imm != nullptr) {
    imm->Unref();
  }
  current->Unref();
  return status;
}

Iterator* DBImpl::NewInternalIterator(const ReadOptions& options, SequenceNumber* latest_snapshot,
                                      uint32_t* seed) {
  mutex_.Lock();
  *latest_snapshot = versions_->LastSequence();

  std::vector<Iterator*> list;
  list.push_back(mem_->NewIterator());
  mem_->Ref();
  if (imm_ != nullptr) {
    list.push_back(imm_->NewIterator());
    imm_->Ref();
  }
  versions_->current()->AddIterators(options, &list);
  Iterator* internal_iter =
      NewMergingIterator(&internal_comparator_, list.data(), static_cast<int>(list.size()));
  versions_->current()->Ref();

  IterState* cleanup = new IterState(&mutex_, mem_, imm_, versions_->current());
  internal_iter->RegisterCleanup(CleanupIteratorState, cleanup, nullptr);

  *seed = ++seed_;
  mutex_.Unlock();
  return internal_iter;
}

Iterator* DBImpl::TEST_NewInternalIterator() {
  SequenceNumber ignored_snapshot = 0;
  uint32_t ignored_seed = 0;
  return NewInternalIterator(ReadOptions(), &ignored_snapshot, &ignored_seed);
}

int64_t DBImpl::TEST_MaxNextLevelOverlappingBytes() {
  MutexLock lock(&mutex_);
  return versions_->MaxNextLevelOverlappingBytes();
}

Iterator* DBImpl::NewIterator(const ReadOptions& options) {
  SequenceNumber latest_snapshot = 0;
  uint32_t seed = 0;
  Iterator* iter = NewInternalIterator(options, &latest_snapshot, &seed);
  const SequenceNumber sequence =
      options.snapshot != nullptr
          ? static_cast<const SnapshotImpl*>(options.snapshot)->sequence_number()
          : latest_snapshot;
  return NewDBIterator(this, user_comparator(), iter, sequence, seed);
}

void DBImpl::RecordReadSample(Slice key) {
  MutexLock lock(&mutex_);
  if (versions_->current()->RecordReadSample(key)) {
    MaybeScheduleCompaction();
  }
}

const Snapshot* DBImpl::GetSnapshot() {
  MutexLock lock(&mutex_);
  return snapshots_.New(versions_->LastSequence());
}

void DBImpl::ReleaseSnapshot(const Snapshot* snapshot) {
  MutexLock lock(&mutex_);
  snapshots_.Delete(static_cast<const SnapshotImpl*>(snapshot));
}

bool DBImpl::GetProperty(const Slice& property, std::string* value) {
  value->clear();

  MutexLock lock(&mutex_);
  Slice input = property;
  Slice prefix("leveldb.");
  if (!input.StartsWith(prefix)) {
    return false;
  }
  input.RemovePrefix(prefix.Size());

  if (input.StartsWith("num-files-at-level")) {
    input.RemovePrefix(std::strlen("num-files-at-level"));
    uint64_t level = 0;
    const bool ok = ConsumeDecimalNumber(&input, &level) && input.Empty();
    if (!ok || level >= config::kNumLevels) {
      return false;
    }

    char buf[100];
    std::snprintf(buf, sizeof(buf), "%d", versions_->NumLevelFiles(static_cast<int>(level)));
    *value = buf;
    return true;
  }

  if (input == Slice("stats")) {
    char buf[200];
    std::snprintf(buf, sizeof(buf),
                  "                               Compactions\n"
                  "Level  Files Size(MB) Time(sec) Read(MB) Write(MB)\n"
                  "--------------------------------------------------\n");
    value->append(buf);
    for (int level = 0; level < config::kNumLevels; level++) {
      const int files = versions_->NumLevelFiles(level);
      if (stats_[level].micros > 0 || files > 0) {
        std::snprintf(buf, sizeof(buf), "%3d %8d %8.0f %9.0f %8.0f %9.0f\n", level, files,
                      versions_->NumLevelBytes(level) / 1048576.0, stats_[level].micros / 1e6,
                      stats_[level].bytes_read / 1048576.0,
                      stats_[level].bytes_written / 1048576.0);
        value->append(buf);
      }
    }
    return true;
  }

  if (input == Slice("sstables")) {
    *value = versions_->current()->DebugString();
    return true;
  }

  if (input == Slice("approximate-memory-usage")) {
    size_t total_usage = options_.block_cache->TotalCharge();
    if (mem_ != nullptr) {
      total_usage += mem_->ApproximateMemoryUsage();
    }
    if (imm_ != nullptr) {
      total_usage += imm_->ApproximateMemoryUsage();
    }
    char buf[50];
    std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(total_usage));
    value->append(buf);
    return true;
  }

  return false;
}

void DBImpl::GetApproximateSizes(const Range* range, int n, uint64_t* sizes) {
  MutexLock lock(&mutex_);
  Version* current = versions_->current();
  current->Ref();
  for (int i = 0; i < n; i++) {
    InternalKey start(range[i].start, kMaxSequenceNumber, kValueTypeForSeek);
    InternalKey limit(range[i].limit, kMaxSequenceNumber, kValueTypeForSeek);
    const uint64_t start_offset = versions_->ApproximateOffsetOf(current, start);
    const uint64_t limit_offset = versions_->ApproximateOffsetOf(current, limit);
    sizes[i] = (limit_offset >= start_offset) ? (limit_offset - start_offset) : 0;
  }
  current->Unref();
}

void DBImpl::CompactRange(const Slice* begin, const Slice* end) {
  int max_level_with_files = 1;
  {
    MutexLock lock(&mutex_);
    Version* base = versions_->current();
    for (int level = 1; level < config::kNumLevels; level++) {
      if (base->OverlapInLevel(level, begin, end)) {
        max_level_with_files = level;
      }
    }
  }

  Status status = TEST_CompactMemTable();
  if (!status.Ok()) {
    return;
  }

  for (int level = 0; level < max_level_with_files; level++) {
    TEST_CompactRange(level, begin, end);
  }
}

void DBImpl::TEST_CompactRange(int level, const Slice* begin, const Slice* end) {
  CompactRangeAtLevel(level, begin, end);
}

Status DBImpl::TEST_CompactMemTable() {
  Status status = Write(WriteOptions(), nullptr);
  if (status.Ok()) {
    MutexLock lock(&mutex_);
    while (imm_ != nullptr && bg_error_.Ok() && !shutting_down_.load(std::memory_order_acquire)) {
      background_work_finished_signal_.Wait();
    }
    if (imm_ != nullptr) {
      status = bg_error_;
    }
  }
  return status;
}

void DBImpl::CompactRangeAtLevel(int level, const Slice* begin, const Slice* end) {
  assert(level >= 0);
  assert(level + 1 < config::kNumLevels);

  InternalKey begin_storage;
  InternalKey end_storage;
  ManualCompaction manual;
  manual.level = level;
  manual.done = false;
  if (begin == nullptr) {
    manual.begin = nullptr;
  } else {
    begin_storage = InternalKey(*begin, kMaxSequenceNumber, kValueTypeForSeek);
    manual.begin = &begin_storage;
  }
  if (end == nullptr) {
    manual.end = nullptr;
  } else {
    end_storage = InternalKey(*end, 0, static_cast<ValueType>(0));
    manual.end = &end_storage;
  }

  MutexLock lock(&mutex_);
  while (!manual.done && !shutting_down_.load(std::memory_order_acquire) && bg_error_.Ok()) {
    if (manual_compaction_ == nullptr) {
      manual_compaction_ = &manual;
      MaybeScheduleCompaction();
    } else {
      background_work_finished_signal_.Wait();
    }
  }

  while (background_compaction_scheduled_) {
    background_work_finished_signal_.Wait();
  }

  if (manual_compaction_ == &manual) {
    manual_compaction_ = nullptr;
  }
}

Status DB::Put(const WriteOptions& options, const Slice& key, const Slice& value) {
  WriteBatch batch;
  batch.Put(key, value);
  return Write(options, &batch);
}

Status DB::Delete(const WriteOptions& options, const Slice& key) {
  WriteBatch batch;
  batch.Delete(key);
  return Write(options, &batch);
}

DB::~DB() = default;

Status DB::Open(const Options& options, const std::string& name, DB** dbptr) {
  *dbptr = nullptr;

  DBImpl* impl = new DBImpl(options, name);
  impl->mutex_.Lock();

  VersionEdit edit;
  bool save_manifest = false;
  Status status = impl->Recover(&edit, &save_manifest);
  if (status.Ok() && impl->mem_ == nullptr) {
    const uint64_t new_log_number = impl->versions_->NewFileNumber();
    WritableFile* logfile = nullptr;
    status = impl->env_->NewWritableFile(LogFileName(name, new_log_number), &logfile);
    if (status.Ok()) {
      edit.SetLogNumber(new_log_number);
      impl->logfile_ = logfile;
      impl->logfile_number_ = new_log_number;
      impl->log_ = new log::Writer(logfile);
      impl->mem_ = new MemTable(impl->internal_comparator_);
      impl->mem_->Ref();
    }
  }

  if (status.Ok() && save_manifest) {
    edit.SetPrevLogNumber(0);
    edit.SetLogNumber(impl->logfile_number_);
    status = impl->versions_->LogAndApply(&edit, &impl->mutex_);
  }

  if (status.Ok()) {
    impl->RemoveObsoleteFiles();
    impl->MaybeScheduleCompaction();
  }

  impl->mutex_.Unlock();
  if (status.Ok()) {
    assert(impl->mem_ != nullptr);
    *dbptr = impl;
  } else {
    delete impl;
  }
  return status;
}

Snapshot::~Snapshot() = default;

Status DestroyDB(const std::string& dbname, const Options& options) {
  Env* env = OptionsEnv(options);
  std::vector<std::string> filenames;
  Status result = env->GetChildren(dbname, &filenames);
  if (!result.Ok()) {
    return Status::OkStatus();
  }

  FileLock* lock = nullptr;
  const std::string lockname = LockFileName(dbname);
  result = env->LockFile(lockname, &lock);
  if (result.Ok()) {
    uint64_t number = 0;
    FileType type = kLogFile;
    for (const std::string& filename : filenames) {
      if (ParseFileName(filename, &number, &type) && type != kDBLockFile) {
        Status delete_status = env->RemoveFile(dbname + "/" + filename);
        if (result.Ok() && !delete_status.Ok()) {
          result = delete_status;
        }
      }
    }
    env->UnlockFile(lock);
    env->RemoveFile(lockname);
    env->RemoveDir(dbname);
  }
  return result;
}

}  // namespace db
