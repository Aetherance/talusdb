#include "c.h"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "cache.h"
#include "comparator.h"
#include "db.h"
#include "env.h"
#include "filter_policy.h"
#include "iterator.h"
#include "options.h"
#include "slice.h"
#include "status.h"
#include "write_batch.h"

using db::Cache;
using db::Comparator;
using db::CompressionType;
using db::DB;
using db::Env;
using db::FileLock;
using db::FilterPolicy;
using db::Iterator;
using db::Logger;
using db::NewBloomFilterPolicy;
using db::NewLRUCache;
using db::Options;
using db::RandomAccessFile;
using db::Range;
using db::ReadOptions;
using db::SequentialFile;
using db::Slice;
using db::Snapshot;
using db::Status;
using db::WritableFile;
using db::WriteBatch;
using db::WriteOptions;

extern "C" {

struct leveldb_t {
  DB* rep;
};
struct leveldb_iterator_t {
  Iterator* rep;
};
struct leveldb_writebatch_t {
  WriteBatch rep;
};
struct leveldb_snapshot_t {
  const Snapshot* rep;
};
struct leveldb_readoptions_t {
  ReadOptions rep;
};
struct leveldb_writeoptions_t {
  WriteOptions rep;
};
struct leveldb_options_t {
  Options rep;
};
struct leveldb_cache_t {
  Cache* rep;
};
struct leveldb_seqfile_t {
  SequentialFile* rep;
};
struct leveldb_randomfile_t {
  RandomAccessFile* rep;
};
struct leveldb_writablefile_t {
  WritableFile* rep;
};
struct leveldb_logger_t {
  Logger* rep;
};
struct leveldb_filelock_t {
  FileLock* rep;
};

struct leveldb_comparator_t : public Comparator {
  ~leveldb_comparator_t() override {
    (*destructor_)(state_);
  }

  int Compare(const Slice& a, const Slice& b) const override {
    return (*compare_)(state_, a.Data(), a.Size(), b.Data(), b.Size());
  }

  const char* Name() const override {
    return (*name_)(state_);
  }

  void FindShortestSeparator(std::string*, const Slice&) const override {}

  void FindShortSuccessor(std::string*) const override {}

  void* state_;
  void (*destructor_)(void*);
  int (*compare_)(void*, const char* a, size_t alen, const char* b, size_t blen);
  const char* (*name_)(void*);
};

struct leveldb_filterpolicy_t : public FilterPolicy {
  ~leveldb_filterpolicy_t() override {
    (*destructor_)(state_);
  }

  const char* Name() const override {
    return (*name_)(state_);
  }

  void CreateFilter(const Slice* keys, int n, std::string* dst) const override {
    std::vector<const char*> key_pointers(static_cast<size_t>(n));
    std::vector<size_t> key_sizes(static_cast<size_t>(n));
    for (int i = 0; i < n; i++) {
      key_pointers[static_cast<size_t>(i)] = keys[i].Data();
      key_sizes[static_cast<size_t>(i)] = keys[i].Size();
    }
    size_t len = 0;
    char* filter = (*create_)(state_, key_pointers.data(), key_sizes.data(), n, &len);
    dst->append(filter, len);
    std::free(filter);
  }

  bool KeyMayMatch(const Slice& key, const Slice& filter) const override {
    return (*key_match_)(state_, key.Data(), key.Size(), filter.Data(), filter.Size());
  }

  void* state_;
  void (*destructor_)(void*);
  const char* (*name_)(void*);
  char* (*create_)(void*, const char* const* key_array, const size_t* key_length_array,
                   int num_keys, size_t* filter_length);
  uint8_t (*key_match_)(void*, const char* key, size_t length, const char* filter,
                        size_t filter_length);
};

struct leveldb_env_t {
  Env* rep;
  bool is_default;
};

namespace {

char* CopyBytes(const char* data, size_t size) {
  char* result = reinterpret_cast<char*>(std::malloc(size == 0 ? 1 : size));
  if (size > 0) {
    std::memcpy(result, data, size);
  }
  return result;
}

char* CopyCString(const std::string& value) {
  char* result = reinterpret_cast<char*>(std::malloc(value.size() + 1));
  std::memcpy(result, value.data(), value.size());
  result[value.size()] = '\0';
  return result;
}

bool SaveError(char** errptr, const Status& status) {
  assert(errptr != nullptr);
  if (status.Ok()) {
    return false;
  }
  if (*errptr != nullptr) {
    std::free(*errptr);
  }
  *errptr = CopyCString(status.ToString());
  return true;
}

}  // namespace

leveldb_t* leveldb_open(const leveldb_options_t* options, const char* name, char** errptr) {
  DB* db = nullptr;
  if (SaveError(errptr, DB::Open(options->rep, std::string(name), &db))) {
    return nullptr;
  }
  leveldb_t* result = new leveldb_t;
  result->rep = db;
  return result;
}

void leveldb_close(leveldb_t* db) {
  delete db->rep;
  delete db;
}

void leveldb_put(leveldb_t* db, const leveldb_writeoptions_t* options, const char* key,
                 size_t keylen, const char* val, size_t vallen, char** errptr) {
  SaveError(errptr, db->rep->Put(options->rep, Slice(key, keylen), Slice(val, vallen)));
}

void leveldb_delete(leveldb_t* db, const leveldb_writeoptions_t* options, const char* key,
                    size_t keylen, char** errptr) {
  SaveError(errptr, db->rep->Delete(options->rep, Slice(key, keylen)));
}

void leveldb_write(leveldb_t* db, const leveldb_writeoptions_t* options,
                   leveldb_writebatch_t* batch, char** errptr) {
  SaveError(errptr, db->rep->Write(options->rep, &batch->rep));
}

char* leveldb_get(leveldb_t* db, const leveldb_readoptions_t* options, const char* key,
                  size_t keylen, size_t* vallen, char** errptr) {
  std::string value;
  Status status = db->rep->Get(options->rep, Slice(key, keylen), &value);
  if (status.Ok()) {
    *vallen = value.size();
    return CopyBytes(value.data(), value.size());
  }

  *vallen = 0;
  if (!status.IsNotFound()) {
    SaveError(errptr, status);
  }
  return nullptr;
}

leveldb_iterator_t* leveldb_create_iterator(leveldb_t* db, const leveldb_readoptions_t* options) {
  leveldb_iterator_t* result = new leveldb_iterator_t;
  result->rep = db->rep->NewIterator(options->rep);
  return result;
}

const leveldb_snapshot_t* leveldb_create_snapshot(leveldb_t* db) {
  leveldb_snapshot_t* result = new leveldb_snapshot_t;
  result->rep = db->rep->GetSnapshot();
  return result;
}

void leveldb_release_snapshot(leveldb_t* db, const leveldb_snapshot_t* snapshot) {
  db->rep->ReleaseSnapshot(snapshot->rep);
  delete snapshot;
}

char* leveldb_property_value(leveldb_t* db, const char* propname) {
  std::string value;
  if (!db->rep->GetProperty(Slice(propname), &value)) {
    return nullptr;
  }
  return CopyCString(value);
}

void leveldb_approximate_sizes(leveldb_t* db, int num_ranges, const char* const* range_start_key,
                               const size_t* range_start_key_len,
                               const char* const* range_limit_key,
                               const size_t* range_limit_key_len, uint64_t* sizes) {
  std::vector<Range> ranges(static_cast<size_t>(num_ranges));
  for (int i = 0; i < num_ranges; i++) {
    ranges[static_cast<size_t>(i)].start =
        Slice(range_start_key[i], range_start_key_len[static_cast<size_t>(i)]);
    ranges[static_cast<size_t>(i)].limit =
        Slice(range_limit_key[i], range_limit_key_len[static_cast<size_t>(i)]);
  }
  db->rep->GetApproximateSizes(ranges.data(), num_ranges, sizes);
}

void leveldb_compact_range(leveldb_t* db, const char* start_key, size_t start_key_len,
                           const char* limit_key, size_t limit_key_len) {
  Slice start;
  Slice limit;
  db->rep->CompactRange(
      start_key != nullptr ? &(start = Slice(start_key, start_key_len)) : nullptr,
      limit_key != nullptr ? &(limit = Slice(limit_key, limit_key_len)) : nullptr);
}

void leveldb_destroy_db(const leveldb_options_t* options, const char* name, char** errptr) {
  SaveError(errptr, db::DestroyDB(name, options->rep));
}

void leveldb_repair_db(const leveldb_options_t* options, const char* name, char** errptr) {
  SaveError(errptr, db::RepairDB(name, options->rep));
}

void leveldb_iter_destroy(leveldb_iterator_t* iter) {
  delete iter->rep;
  delete iter;
}

uint8_t leveldb_iter_valid(const leveldb_iterator_t* iter) {
  return iter->rep->Valid();
}

void leveldb_iter_seek_to_first(leveldb_iterator_t* iter) {
  iter->rep->SeekToFirst();
}

void leveldb_iter_seek_to_last(leveldb_iterator_t* iter) {
  iter->rep->SeekToLast();
}

void leveldb_iter_seek(leveldb_iterator_t* iter, const char* k, size_t klen) {
  iter->rep->Seek(Slice(k, klen));
}

void leveldb_iter_next(leveldb_iterator_t* iter) {
  iter->rep->Next();
}

void leveldb_iter_prev(leveldb_iterator_t* iter) {
  iter->rep->Prev();
}

const char* leveldb_iter_key(const leveldb_iterator_t* iter, size_t* klen) {
  Slice key = iter->rep->Key();
  *klen = key.Size();
  return key.Data();
}

const char* leveldb_iter_value(const leveldb_iterator_t* iter, size_t* vlen) {
  Slice value = iter->rep->Value();
  *vlen = value.Size();
  return value.Data();
}

void leveldb_iter_get_error(const leveldb_iterator_t* iter, char** errptr) {
  SaveError(errptr, iter->rep->GetStatus());
}

leveldb_writebatch_t* leveldb_writebatch_create() {
  return new leveldb_writebatch_t;
}

void leveldb_writebatch_destroy(leveldb_writebatch_t* batch) {
  delete batch;
}

void leveldb_writebatch_clear(leveldb_writebatch_t* batch) {
  batch->rep.Clear();
}

void leveldb_writebatch_put(leveldb_writebatch_t* batch, const char* key, size_t klen,
                            const char* val, size_t vlen) {
  batch->rep.Put(Slice(key, klen), Slice(val, vlen));
}

void leveldb_writebatch_delete(leveldb_writebatch_t* batch, const char* key, size_t klen) {
  batch->rep.Delete(Slice(key, klen));
}

void leveldb_writebatch_iterate(const leveldb_writebatch_t* batch, void* state,
                                void (*put)(void*, const char* k, size_t klen, const char* v,
                                            size_t vlen),
                                void (*deleted)(void*, const char* k, size_t klen)) {
  class Handler : public WriteBatch::Handler {
  public:
    void Put(const Slice& key, const Slice& value) override {
      (*put_)(state_, key.Data(), key.Size(), value.Data(), value.Size());
    }

    void Delete(const Slice& key) override {
      (*deleted_)(state_, key.Data(), key.Size());
    }

    void* state_;
    void (*put_)(void*, const char* k, size_t klen, const char* v, size_t vlen);
    void (*deleted_)(void*, const char* k, size_t klen);
  };

  Handler handler;
  handler.state_ = state;
  handler.put_ = put;
  handler.deleted_ = deleted;
  batch->rep.Iterate(&handler);
}

void leveldb_writebatch_append(leveldb_writebatch_t* destination,
                               const leveldb_writebatch_t* source) {
  destination->rep.Append(source->rep);
}

leveldb_options_t* leveldb_options_create() {
  return new leveldb_options_t;
}

void leveldb_options_destroy(leveldb_options_t* options) {
  delete options;
}

void leveldb_options_set_comparator(leveldb_options_t* options, leveldb_comparator_t* comparator) {
  options->rep.comparator = comparator;
}

void leveldb_options_set_filter_policy(leveldb_options_t* options, leveldb_filterpolicy_t* policy) {
  options->rep.filter_policy = policy;
}

void leveldb_options_set_create_if_missing(leveldb_options_t* options, uint8_t value) {
  options->rep.create_if_missing = value != 0;
}

void leveldb_options_set_error_if_exists(leveldb_options_t* options, uint8_t value) {
  options->rep.error_if_exists = value != 0;
}

void leveldb_options_set_paranoid_checks(leveldb_options_t* options, uint8_t value) {
  options->rep.paranoid_checks = value != 0;
}

void leveldb_options_set_env(leveldb_options_t* options, leveldb_env_t* env) {
  options->rep.env = env != nullptr ? env->rep : nullptr;
}

void leveldb_options_set_info_log(leveldb_options_t* options, leveldb_logger_t* logger) {
  options->rep.info_log = logger != nullptr ? logger->rep : nullptr;
}

void leveldb_options_set_write_buffer_size(leveldb_options_t* options, size_t size) {
  options->rep.write_buffer_size = size;
}

void leveldb_options_set_max_open_files(leveldb_options_t* options, int value) {
  options->rep.max_open_files = value;
}

void leveldb_options_set_cache(leveldb_options_t* options, leveldb_cache_t* cache) {
  options->rep.block_cache = cache != nullptr ? cache->rep : nullptr;
}

void leveldb_options_set_block_size(leveldb_options_t* options, size_t size) {
  options->rep.block_size = size;
}

void leveldb_options_set_block_restart_interval(leveldb_options_t* options, int value) {
  options->rep.block_restart_interval = value;
}

void leveldb_options_set_max_file_size(leveldb_options_t* options, size_t size) {
  options->rep.max_file_size = size;
}

void leveldb_options_set_compression(leveldb_options_t* options, int value) {
  options->rep.compression = static_cast<CompressionType>(value);
}

leveldb_comparator_t* leveldb_comparator_create(void* state, void (*destructor)(void*),
                                                int (*compare)(void*, const char* a, size_t alen,
                                                               const char* b, size_t blen),
                                                const char* (*name)(void*)) {
  leveldb_comparator_t* result = new leveldb_comparator_t;
  result->state_ = state;
  result->destructor_ = destructor;
  result->compare_ = compare;
  result->name_ = name;
  return result;
}

void leveldb_comparator_destroy(leveldb_comparator_t* comparator) {
  delete comparator;
}

leveldb_filterpolicy_t* leveldb_filterpolicy_create(
    void* state, void (*destructor)(void*),
    char* (*create_filter)(void*, const char* const* key_array, const size_t* key_length_array,
                           int num_keys, size_t* filter_length),
    uint8_t (*key_may_match)(void*, const char* key, size_t length, const char* filter,
                             size_t filter_length),
    const char* (*name)(void*)) {
  leveldb_filterpolicy_t* result = new leveldb_filterpolicy_t;
  result->state_ = state;
  result->destructor_ = destructor;
  result->create_ = create_filter;
  result->key_match_ = key_may_match;
  result->name_ = name;
  return result;
}

void leveldb_filterpolicy_destroy(leveldb_filterpolicy_t* filter) {
  delete filter;
}

leveldb_filterpolicy_t* leveldb_filterpolicy_create_bloom(int bits_per_key) {
  struct Wrapper : public leveldb_filterpolicy_t {
    static void DoNothing(void*) {}

    ~Wrapper() override {
      delete rep_;
    }

    const char* Name() const override {
      return rep_->Name();
    }

    void CreateFilter(const Slice* keys, int n, std::string* dst) const override {
      return rep_->CreateFilter(keys, n, dst);
    }

    bool KeyMayMatch(const Slice& key, const Slice& filter) const override {
      return rep_->KeyMayMatch(key, filter);
    }

    const FilterPolicy* rep_;
  };

  Wrapper* wrapper = new Wrapper;
  wrapper->rep_ = NewBloomFilterPolicy(bits_per_key);
  wrapper->state_ = nullptr;
  wrapper->destructor_ = &Wrapper::DoNothing;
  return wrapper;
}

leveldb_readoptions_t* leveldb_readoptions_create() {
  return new leveldb_readoptions_t;
}

void leveldb_readoptions_destroy(leveldb_readoptions_t* options) {
  delete options;
}

void leveldb_readoptions_set_verify_checksums(leveldb_readoptions_t* options, uint8_t value) {
  options->rep.verify_checksums = value != 0;
}

void leveldb_readoptions_set_fill_cache(leveldb_readoptions_t* options, uint8_t value) {
  options->rep.fill_cache = value != 0;
}

void leveldb_readoptions_set_snapshot(leveldb_readoptions_t* options,
                                      const leveldb_snapshot_t* snapshot) {
  options->rep.snapshot = snapshot != nullptr ? snapshot->rep : nullptr;
}

leveldb_writeoptions_t* leveldb_writeoptions_create() {
  return new leveldb_writeoptions_t;
}

void leveldb_writeoptions_destroy(leveldb_writeoptions_t* options) {
  delete options;
}

void leveldb_writeoptions_set_sync(leveldb_writeoptions_t* options, uint8_t value) {
  options->rep.sync = value != 0;
}

leveldb_cache_t* leveldb_cache_create_lru(size_t capacity) {
  leveldb_cache_t* cache = new leveldb_cache_t;
  cache->rep = NewLRUCache(capacity);
  return cache;
}

void leveldb_cache_destroy(leveldb_cache_t* cache) {
  delete cache->rep;
  delete cache;
}

leveldb_env_t* leveldb_create_default_env() {
  leveldb_env_t* result = new leveldb_env_t;
  result->rep = Env::Default();
  result->is_default = true;
  return result;
}

void leveldb_env_destroy(leveldb_env_t* env) {
  if (!env->is_default) {
    delete env->rep;
  }
  delete env;
}

char* leveldb_env_get_test_directory(leveldb_env_t* env) {
  std::string result;
  if (!env->rep->GetTestDirectory(&result).Ok()) {
    return nullptr;
  }
  return CopyCString(result);
}

void leveldb_free(void* ptr) {
  std::free(ptr);
}

int leveldb_major_version() {
  return 0;
}

int leveldb_minor_version() {
  return 1;
}

}  // extern "C"
