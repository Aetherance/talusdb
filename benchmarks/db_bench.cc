// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by the BSD-style license in
// benchmarks/LICENSE.leveldb.
//
// Adapted from LevelDB's benchmarks/db_bench.cc for TalusDB's public API.

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cache.h"
#include "comparator.h"
#include "db.h"
#include "env.h"
#include "filter_policy.h"
#include "iterator.h"
#include "options.h"
#include "port.h"
#include "slice.h"
#include "status.h"
#include "write_batch.h"

namespace {

const char* flags_benchmarks =
    "fillseq,fillsync,fillrandom,overwrite,readrandom,readrandom,readseq,"
    "readreverse,compact,readrandom,readseq,readreverse,fill100K";
int flags_num = 1'000'000;
int flags_reads = -1;
int flags_threads = 1;
int flags_value_size = 100;
double flags_compression_ratio = 0.5;
bool flags_histogram = false;
bool flags_comparisons = false;
int flags_write_buffer_size = 0;
int flags_max_file_size = 0;
int flags_block_size = 0;
int flags_cache_size = -1;
int flags_open_files = 0;
int flags_bloom_bits = -1;
int flags_key_prefix = 0;
bool flags_use_existing_db = false;
bool flags_reuse_logs = false;
bool flags_compression = true;
const char* flags_db = nullptr;

db::Env* env = nullptr;

class Random {
public:
  explicit Random(uint32_t seed) : seed_(seed & 0x7fffffffu) {
    if (seed_ == 0 || seed_ == 2147483647u) {
      seed_ = 1;
    }
  }

  uint32_t Next() {
    static constexpr uint32_t kModulus = 2147483647u;
    static constexpr uint64_t kMultiplier = 16807;
    const uint64_t product = seed_ * kMultiplier;
    seed_ = static_cast<uint32_t>((product >> 31u) + (product & kModulus));
    if (seed_ > kModulus) {
      seed_ -= kModulus;
    }
    return seed_;
  }

  uint32_t Uniform(int n) {
    assert(n > 0);
    return Next() % static_cast<uint32_t>(n);
  }

private:
  uint32_t seed_;
};

class RandomGenerator {
public:
  RandomGenerator() {
    Random random(301);
    const size_t target_size =
        std::max<size_t>(1'048'576, static_cast<size_t>(flags_value_size) + 1);
    while (data_.size() < target_size) {
      const int raw_size = std::max(1, static_cast<int>(100 * flags_compression_ratio));
      std::string raw_data(static_cast<size_t>(raw_size), ' ');
      for (char& character : raw_data) {
        character = static_cast<char>(' ' + random.Uniform(95));
      }
      std::string piece;
      while (piece.size() < 100) {
        piece.append(raw_data.data(), std::min(raw_data.size(), 100 - piece.size()));
      }
      data_.append(piece);
    }
  }

  db::Slice Generate(size_t length) {
    assert(length < data_.size());
    if (position_ + length > data_.size()) {
      position_ = 0;
    }
    const db::Slice result(data_.data() + position_, length);
    position_ += length;
    return result;
  }

private:
  std::string data_;
  size_t position_ = 0;
};

class KeyBuffer {
public:
  KeyBuffer() {
    assert(flags_key_prefix >= 0);
    assert(static_cast<size_t>(flags_key_prefix + 17) <= buffer_.size());
    std::fill_n(buffer_.begin(), flags_key_prefix, 'a');
  }

  void Set(int key) {
    const size_t prefix_size = static_cast<size_t>(flags_key_prefix);
    std::snprintf(buffer_.data() + prefix_size, buffer_.size() - prefix_size, "%016d", key);
  }

  db::Slice GetSlice() const {
    return db::Slice(buffer_.data(), static_cast<size_t>(flags_key_prefix + 16));
  }

private:
  std::array<char, 1024> buffer_{};
};

class Histogram {
public:
  void Clear() {
    buckets_.fill(0);
    count_ = 0;
    min_ = 0;
    max_ = 0;
    sum_ = 0;
    sum_squares_ = 0;
  }

  void Add(double value) {
    const size_t bucket = BucketFor(value);
    ++buckets_[bucket];
    if (count_ == 0 || value < min_) {
      min_ = value;
    }
    if (count_ == 0 || value > max_) {
      max_ = value;
    }
    ++count_;
    sum_ += value;
    sum_squares_ += value * value;
  }

  void Merge(const Histogram& other) {
    if (other.count_ == 0) {
      return;
    }
    if (count_ == 0 || other.min_ < min_) {
      min_ = other.min_;
    }
    if (count_ == 0 || other.max_ > max_) {
      max_ = other.max_;
    }
    count_ += other.count_;
    sum_ += other.sum_;
    sum_squares_ += other.sum_squares_;
    for (size_t i = 0; i < buckets_.size(); ++i) {
      buckets_[i] += other.buckets_[i];
    }
  }

  std::string ToString() const {
    if (count_ == 0) {
      return "Count: 0\n";
    }

    const double average = sum_ / static_cast<double>(count_);
    const double variance =
        std::max(0.0, sum_squares_ / static_cast<double>(count_) - average * average);
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer),
                  "Count: %llu  Average: %.4f  StdDev: %.2f\n"
                  "Min: %.4f  P50: %.4f  P95: %.4f  P99: %.4f  Max: %.4f\n",
                  static_cast<unsigned long long>(count_), average, std::sqrt(variance), min_,
                  Percentile(50), Percentile(95), Percentile(99), max_);
    return buffer;
  }

private:
  static constexpr size_t kBucketCount = 128;
  static constexpr double kBucketsPerPowerOfTwo = 4.0;

  static size_t BucketFor(double value) {
    if (value <= 1.0) {
      return 0;
    }
    const double index = std::ceil(std::log2(value) * kBucketsPerPowerOfTwo);
    return std::min(static_cast<size_t>(index), kBucketCount - 1);
  }

  static double BucketUpperBound(size_t bucket) {
    if (bucket == 0) {
      return 1.0;
    }
    return std::pow(2.0, static_cast<double>(bucket) / kBucketsPerPowerOfTwo);
  }

  double Percentile(int percentage) const {
    const uint64_t threshold = (count_ * static_cast<uint64_t>(percentage) + 99u) / 100u;
    uint64_t cumulative = 0;
    for (size_t i = 0; i < buckets_.size(); ++i) {
      cumulative += buckets_[i];
      if (cumulative >= threshold) {
        return std::clamp(BucketUpperBound(i), min_, max_);
      }
    }
    return max_;
  }

  std::array<uint64_t, kBucketCount> buckets_{};
  uint64_t count_ = 0;
  double min_ = 0;
  double max_ = 0;
  double sum_ = 0;
  double sum_squares_ = 0;
};

void AppendWithSpace(std::string* destination, const db::Slice& message) {
  if (message.Empty()) {
    return;
  }
  if (!destination->empty()) {
    destination->push_back(' ');
  }
  destination->append(message.Data(), message.Size());
}

class Stats {
public:
  Stats() {
    Start();
  }

  void Start() {
    next_report_ = 100;
    histogram_.Clear();
    done_ = 0;
    bytes_ = 0;
    seconds_ = 0;
    message_.clear();
    start_ = finish_ = last_op_finish_ = env->NowMicros();
  }

  void Stop() {
    finish_ = env->NowMicros();
    seconds_ = static_cast<double>(finish_ - start_) * 1e-6;
  }

  void Merge(const Stats& other) {
    histogram_.Merge(other.histogram_);
    done_ += other.done_;
    bytes_ += other.bytes_;
    seconds_ += other.seconds_;
    start_ = std::min(start_, other.start_);
    finish_ = std::max(finish_, other.finish_);
    if (message_.empty()) {
      message_ = other.message_;
    }
  }

  void AddMessage(const db::Slice& message) {
    AppendWithSpace(&message_, message);
  }

  void AddBytes(int64_t bytes) {
    bytes_ += bytes;
  }

  void FinishedSingleOp() {
    if (flags_histogram) {
      const uint64_t now = env->NowMicros();
      const double micros = static_cast<double>(now - last_op_finish_);
      histogram_.Add(micros);
      if (micros > 20'000) {
        std::fprintf(stderr, "long op: %.1f micros%30s\r", micros, "");
        std::fflush(stderr);
      }
      last_op_finish_ = now;
    }

    ++done_;
    if (done_ >= next_report_) {
      if (next_report_ < 1'000) {
        next_report_ += 100;
      } else if (next_report_ < 5'000) {
        next_report_ += 500;
      } else if (next_report_ < 10'000) {
        next_report_ += 1'000;
      } else if (next_report_ < 50'000) {
        next_report_ += 5'000;
      } else if (next_report_ < 100'000) {
        next_report_ += 10'000;
      } else if (next_report_ < 500'000) {
        next_report_ += 50'000;
      } else {
        next_report_ += 100'000;
      }
      std::fprintf(stderr, "... finished %d ops%30s\r", done_, "");
      std::fflush(stderr);
    }
  }

  void Report(const db::Slice& name) {
    done_ = std::max(done_, 1);
    std::string extra;
    const double elapsed = static_cast<double>(finish_ - start_) * 1e-6;
    if (bytes_ > 0 && elapsed > 0) {
      char rate[100];
      std::snprintf(rate, sizeof(rate), "%6.1f MB/s",
                    (static_cast<double>(bytes_) / 1'048'576.0) / elapsed);
      extra = rate;
    }
    AppendWithSpace(&extra, message_);

    std::fprintf(stdout, "%-12s : %11.3f micros/op;%s%s\n", name.ToString().c_str(),
                 seconds_ * 1e6 / static_cast<double>(done_), extra.empty() ? "" : " ",
                 extra.c_str());
    if (flags_histogram) {
      std::fprintf(stdout, "Microseconds per op:\n%s\n", histogram_.ToString().c_str());
    }
    std::fflush(stdout);
  }

private:
  uint64_t start_ = 0;
  uint64_t finish_ = 0;
  double seconds_ = 0;
  int done_ = 0;
  int next_report_ = 0;
  int64_t bytes_ = 0;
  uint64_t last_op_finish_ = 0;
  Histogram histogram_;
  std::string message_;
};

class CountComparator final : public db::Comparator {
public:
  explicit CountComparator(const db::Comparator* wrapped) : wrapped_(wrapped) {}

  int Compare(const db::Slice& lhs, const db::Slice& rhs) const override {
    count_.fetch_add(1, std::memory_order_relaxed);
    return wrapped_->Compare(lhs, rhs);
  }

  const char* Name() const override {
    return wrapped_->Name();
  }

  void FindShortestSeparator(std::string* start, const db::Slice& limit) const override {
    wrapped_->FindShortestSeparator(start, limit);
  }

  void FindShortSuccessor(std::string* key) const override {
    wrapped_->FindShortSuccessor(key);
  }

  size_t Comparisons() const {
    return count_.load(std::memory_order_relaxed);
  }

  void Reset() {
    count_.store(0, std::memory_order_relaxed);
  }

private:
  mutable std::atomic<size_t> count_{0};
  const db::Comparator* wrapped_;
};

struct SharedState {
  explicit SharedState(int thread_count) : total(thread_count) {}

  std::mutex mutex;
  std::condition_variable condition;
  int total;
  int initialized = 0;
  int done = 0;
  bool start = false;
};

struct ThreadState {
  ThreadState(int thread_id, uint32_t seed) : id(thread_id), random(seed) {}

  int id;
  Random random;
  Stats stats;
  SharedState* shared = nullptr;
};

class Benchmark {
public:
  Benchmark()
      : cache_(flags_cache_size >= 0 ? db::NewLRUCache(static_cast<size_t>(flags_cache_size))
                                     : nullptr),
        filter_policy_(flags_bloom_bits >= 0 ? db::NewBloomFilterPolicy(flags_bloom_bits)
                                             : nullptr),
        num_(flags_num),
        value_size_(flags_value_size),
        reads_(flags_reads < 0 ? flags_num : flags_reads),
        count_comparator_(db::BytewiseComparator()) {
    if (!flags_use_existing_db) {
      db::DestroyDB(flags_db, db::Options());
    }
  }

  ~Benchmark() {
    delete db_;
    delete cache_;
    delete filter_policy_;
  }

  void Run() {
    PrintHeader();
    Open();

    const char* current = flags_benchmarks;
    while (current != nullptr) {
      const char* separator = std::strchr(current, ',');
      const db::Slice name = separator == nullptr
                                 ? db::Slice(current)
                                 : db::Slice(current, static_cast<size_t>(separator - current));
      current = separator == nullptr ? nullptr : separator + 1;

      num_ = flags_num;
      reads_ = flags_reads < 0 ? flags_num : flags_reads;
      value_size_ = flags_value_size;
      entries_per_batch_ = 1;
      write_options_ = db::WriteOptions();

      Method method = nullptr;
      bool fresh_database = false;
      int thread_count = flags_threads;

      if (name == db::Slice("open")) {
        method = &Benchmark::OpenBench;
        num_ = std::max(1, num_ / 10'000);
        thread_count = 1;
      } else if (name == db::Slice("fillseq")) {
        fresh_database = true;
        method = &Benchmark::WriteSequential;
      } else if (name == db::Slice("fillbatch")) {
        fresh_database = true;
        entries_per_batch_ = 1'000;
        method = &Benchmark::WriteSequential;
      } else if (name == db::Slice("fillrandom")) {
        fresh_database = true;
        method = &Benchmark::WriteRandom;
      } else if (name == db::Slice("overwrite")) {
        method = &Benchmark::WriteRandom;
      } else if (name == db::Slice("fillsync")) {
        fresh_database = true;
        num_ = std::max(1, num_ / 1'000);
        write_options_.sync = true;
        method = &Benchmark::WriteRandom;
      } else if (name == db::Slice("fill100K")) {
        fresh_database = true;
        num_ = std::max(1, num_ / 1'000);
        value_size_ = 100'000;
        method = &Benchmark::WriteRandom;
      } else if (name == db::Slice("readseq")) {
        method = &Benchmark::ReadSequential;
      } else if (name == db::Slice("readreverse")) {
        method = &Benchmark::ReadReverse;
      } else if (name == db::Slice("readrandom")) {
        method = &Benchmark::ReadRandom;
      } else if (name == db::Slice("readmissing")) {
        method = &Benchmark::ReadMissing;
      } else if (name == db::Slice("readhot")) {
        method = &Benchmark::ReadHot;
      } else if (name == db::Slice("readrandomsmall")) {
        reads_ = std::max(1, reads_ / 1'000);
        method = &Benchmark::ReadRandom;
      } else if (name == db::Slice("seekrandom")) {
        method = &Benchmark::SeekRandom;
      } else if (name == db::Slice("seekordered")) {
        method = &Benchmark::SeekOrdered;
      } else if (name == db::Slice("deleteseq")) {
        method = &Benchmark::DeleteSequential;
      } else if (name == db::Slice("deleterandom")) {
        method = &Benchmark::DeleteRandom;
      } else if (name == db::Slice("readwhilewriting")) {
        ++thread_count;
        method = &Benchmark::ReadWhileWriting;
      } else if (name == db::Slice("compact")) {
        method = &Benchmark::Compact;
      } else if (name == db::Slice("stats")) {
        PrintStats("leveldb.stats");
      } else if (name == db::Slice("sstables")) {
        PrintStats("leveldb.sstables");
      } else if (!name.Empty()) {
        std::fprintf(stderr, "unknown benchmark '%s'\n", name.ToString().c_str());
      }

      if (fresh_database) {
        if (flags_use_existing_db) {
          std::fprintf(stdout, "%-12s : skipped (--use_existing_db is true)\n",
                       name.ToString().c_str());
          method = nullptr;
        } else {
          delete db_;
          db_ = nullptr;
          Check(db::DestroyDB(flags_db, db::Options()), "destroy");
          Open();
        }
      }

      if (method != nullptr) {
        RunBenchmark(thread_count, name, method);
      }
    }
  }

private:
  using Method = void (Benchmark::*)(ThreadState*);

  struct ThreadArgument {
    Benchmark* benchmark;
    SharedState* shared;
    ThreadState* thread;
    Method method;
  };

  static void Check(const db::Status& status, const char* operation) {
    if (!status.Ok()) {
      std::fprintf(stderr, "%s error: %s\n", operation, status.ToString().c_str());
      std::exit(1);
    }
  }

  void PrintHeader() const {
    PrintEnvironment();
    const int key_size = 16 + flags_key_prefix;
    std::fprintf(stdout, "Keys:       %d bytes each\n", key_size);
    std::fprintf(stdout, "Values:     %d bytes each (%d bytes after compression)\n",
                 flags_value_size,
                 static_cast<int>(flags_value_size * flags_compression_ratio + 0.5));
    std::fprintf(stdout, "Entries:    %d\n", num_);
    std::fprintf(stdout, "RawSize:    %.1f MB (estimated)\n",
                 static_cast<double>(static_cast<int64_t>(key_size + flags_value_size) * num_) /
                     1'048'576.0);
    std::fprintf(stdout, "FileSize:   %.1f MB (estimated)\n",
                 static_cast<double>(key_size + flags_value_size * flags_compression_ratio) * num_ /
                     1'048'576.0);
    if (flags_compression) {
      const char text[] = "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy";
      std::string compressed;
      if (!db::port::Snappy_Compress(text, sizeof(text), &compressed)) {
        std::fprintf(stdout, "WARNING: Snappy is unavailable; SSTables will use no compression\n");
      }
    }
#if defined(__GNUC__) && !defined(__OPTIMIZE__)
    std::fprintf(stdout, "WARNING: Optimization is disabled; benchmark results will be slow\n");
#endif
#ifndef NDEBUG
    std::fprintf(stdout, "WARNING: Assertions are enabled; benchmark results will be slow\n");
#endif
    std::fprintf(stdout, "------------------------------------------------\n");
    std::fflush(stdout);
  }

  static db::Slice TrimSpace(db::Slice input) {
    while (!input.Empty() && std::isspace(static_cast<unsigned char>(input[0])) != 0) {
      input.RemovePrefix(1);
    }
    while (!input.Empty() &&
           std::isspace(static_cast<unsigned char>(input[input.Size() - 1])) != 0) {
      input = db::Slice(input.Data(), input.Size() - 1);
    }
    return input;
  }

  static void PrintEnvironment() {
    const std::time_t now = std::time(nullptr);
    std::fprintf(stderr, "TalusDB benchmark\n");
    std::fprintf(stderr, "Date:       %s", std::ctime(&now));

#if defined(__linux__)
    FILE* cpu_info = std::fopen("/proc/cpuinfo", "r");
    if (cpu_info != nullptr) {
      char line[1000];
      int cpu_count = 0;
      std::string cpu_type;
      std::string cache_size;
      while (std::fgets(line, sizeof(line), cpu_info) != nullptr) {
        const char* separator = std::strchr(line, ':');
        if (separator == nullptr) {
          continue;
        }
        const db::Slice key = TrimSpace(db::Slice(line, static_cast<size_t>(separator - line)));
        const db::Slice value = TrimSpace(db::Slice(separator + 1));
        if (key == db::Slice("model name")) {
          ++cpu_count;
          cpu_type = value.ToString();
        } else if (key == db::Slice("cache size")) {
          cache_size = value.ToString();
        }
      }
      std::fclose(cpu_info);
      std::fprintf(stderr, "CPU:        %d * %s\n", cpu_count, cpu_type.c_str());
      std::fprintf(stderr, "CPUCache:   %s\n", cache_size.c_str());
    }
#endif
  }

  void Open() {
    assert(db_ == nullptr);
    db::Options options;
    options.env = env;
    options.create_if_missing = !flags_use_existing_db;
    options.block_cache = cache_;
    options.write_buffer_size = static_cast<size_t>(flags_write_buffer_size);
    options.max_file_size = static_cast<size_t>(flags_max_file_size);
    options.block_size = static_cast<size_t>(flags_block_size);
    options.max_open_files = flags_open_files;
    options.filter_policy = filter_policy_;
    options.reuse_logs = flags_reuse_logs;
    options.compression = flags_compression ? db::kSnappyCompression : db::kNoCompression;
    if (flags_comparisons) {
      options.comparator = &count_comparator_;
    }
    Check(db::DB::Open(options, flags_db, &db_), "open");
  }

  static void ThreadBody(ThreadArgument* argument) {
    SharedState* shared = argument->shared;
    {
      std::unique_lock lock(shared->mutex);
      ++shared->initialized;
      shared->condition.notify_all();
      shared->condition.wait(lock, [shared] { return shared->start; });
    }

    argument->thread->stats.Start();
    (argument->benchmark->*(argument->method))(argument->thread);
    argument->thread->stats.Stop();

    {
      std::lock_guard lock(shared->mutex);
      ++shared->done;
      shared->condition.notify_all();
    }
  }

  void RunBenchmark(int thread_count, const db::Slice& name, Method method) {
    SharedState shared(thread_count);
    std::vector<std::unique_ptr<ThreadState>> states;
    std::vector<ThreadArgument> arguments;
    std::vector<std::thread> threads;
    states.reserve(static_cast<size_t>(thread_count));
    arguments.reserve(static_cast<size_t>(thread_count));
    threads.reserve(static_cast<size_t>(thread_count));

    for (int i = 0; i < thread_count; ++i) {
      ++total_thread_count_;
      states.push_back(
          std::make_unique<ThreadState>(i, static_cast<uint32_t>(1000 + total_thread_count_)));
      states.back()->shared = &shared;
      arguments.push_back(ThreadArgument{this, &shared, states.back().get(), method});
    }
    for (ThreadArgument& argument : arguments) {
      threads.emplace_back(ThreadBody, &argument);
    }

    {
      std::unique_lock lock(shared.mutex);
      shared.condition.wait(lock, [&shared] { return shared.initialized == shared.total; });
      shared.start = true;
      shared.condition.notify_all();
      shared.condition.wait(lock, [&shared] { return shared.done == shared.total; });
    }
    for (std::thread& thread : threads) {
      thread.join();
    }

    for (int i = 1; i < thread_count; ++i) {
      states.front()->stats.Merge(states[static_cast<size_t>(i)]->stats);
    }
    states.front()->stats.Report(name);
    if (flags_comparisons) {
      std::fprintf(stdout, "Comparisons: %zu\n", count_comparator_.Comparisons());
      count_comparator_.Reset();
    }
  }

  void OpenBench(ThreadState* thread) {
    for (int i = 0; i < num_; ++i) {
      delete db_;
      db_ = nullptr;
      Open();
      thread->stats.FinishedSingleOp();
    }
  }

  void WriteSequential(ThreadState* thread) {
    DoWrite(thread, true);
  }

  void WriteRandom(ThreadState* thread) {
    DoWrite(thread, false);
  }

  void DoWrite(ThreadState* thread, bool sequential) {
    if (num_ != flags_num) {
      char message[100];
      std::snprintf(message, sizeof(message), "(%d ops)", num_);
      thread->stats.AddMessage(message);
    }

    RandomGenerator generator;
    db::WriteBatch batch;
    int64_t bytes = 0;
    KeyBuffer key;
    for (int i = 0; i < num_; i += entries_per_batch_) {
      batch.Clear();
      const int batch_limit = std::min(entries_per_batch_, num_ - i);
      for (int j = 0; j < batch_limit; ++j) {
        const int key_number =
            sequential ? i + j : static_cast<int>(thread->random.Uniform(flags_num));
        key.Set(key_number);
        const db::Slice key_slice = key.GetSlice();
        batch.Put(key_slice, generator.Generate(static_cast<size_t>(value_size_)));
        bytes += static_cast<int64_t>(key_slice.Size()) + value_size_;
        thread->stats.FinishedSingleOp();
      }
      Check(db_->Write(write_options_, &batch), "write");
    }
    thread->stats.AddBytes(bytes);
  }

  void ReadSequential(ThreadState* thread) {
    std::unique_ptr<db::Iterator> iterator(db_->NewIterator(db::ReadOptions()));
    int count = 0;
    int64_t bytes = 0;
    for (iterator->SeekToFirst(); count < reads_ && iterator->Valid(); iterator->Next()) {
      bytes += static_cast<int64_t>(iterator->Key().Size() + iterator->Value().Size());
      thread->stats.FinishedSingleOp();
      ++count;
    }
    Check(iterator->GetStatus(), "iterator");
    thread->stats.AddBytes(bytes);
  }

  void ReadReverse(ThreadState* thread) {
    std::unique_ptr<db::Iterator> iterator(db_->NewIterator(db::ReadOptions()));
    int count = 0;
    int64_t bytes = 0;
    for (iterator->SeekToLast(); count < reads_ && iterator->Valid(); iterator->Prev()) {
      bytes += static_cast<int64_t>(iterator->Key().Size() + iterator->Value().Size());
      thread->stats.FinishedSingleOp();
      ++count;
    }
    Check(iterator->GetStatus(), "iterator");
    thread->stats.AddBytes(bytes);
  }

  void ReadRandom(ThreadState* thread) {
    db::ReadOptions options;
    std::string value;
    int found = 0;
    KeyBuffer key;
    for (int i = 0; i < reads_; ++i) {
      key.Set(static_cast<int>(thread->random.Uniform(flags_num)));
      if (db_->Get(options, key.GetSlice(), &value).Ok()) {
        ++found;
      }
      thread->stats.FinishedSingleOp();
    }
    char message[100];
    std::snprintf(message, sizeof(message), "(%d of %d found)", found, reads_);
    thread->stats.AddMessage(message);
  }

  void ReadMissing(ThreadState* thread) {
    db::ReadOptions options;
    std::string value;
    KeyBuffer key;
    for (int i = 0; i < reads_; ++i) {
      key.Set(static_cast<int>(thread->random.Uniform(flags_num)));
      const db::Slice present = key.GetSlice();
      db_->Get(options, db::Slice(present.Data(), present.Size() - 1), &value);
      thread->stats.FinishedSingleOp();
    }
  }

  void ReadHot(ThreadState* thread) {
    db::ReadOptions options;
    std::string value;
    const int range = std::max(1, (flags_num + 99) / 100);
    KeyBuffer key;
    for (int i = 0; i < reads_; ++i) {
      key.Set(static_cast<int>(thread->random.Uniform(range)));
      db_->Get(options, key.GetSlice(), &value);
      thread->stats.FinishedSingleOp();
    }
  }

  void SeekRandom(ThreadState* thread) {
    db::ReadOptions options;
    int found = 0;
    KeyBuffer key;
    for (int i = 0; i < reads_; ++i) {
      std::unique_ptr<db::Iterator> iterator(db_->NewIterator(options));
      key.Set(static_cast<int>(thread->random.Uniform(flags_num)));
      iterator->Seek(key.GetSlice());
      if (iterator->Valid() && iterator->Key() == key.GetSlice()) {
        ++found;
      }
      Check(iterator->GetStatus(), "iterator");
      thread->stats.FinishedSingleOp();
    }
    char message[100];
    std::snprintf(message, sizeof(message), "(%d of %d found)", found, reads_);
    thread->stats.AddMessage(message);
  }

  void SeekOrdered(ThreadState* thread) {
    db::ReadOptions options;
    std::unique_ptr<db::Iterator> iterator(db_->NewIterator(options));
    int found = 0;
    int key_number = 0;
    KeyBuffer key;
    for (int i = 0; i < reads_; ++i) {
      key_number = (key_number + static_cast<int>(thread->random.Uniform(100))) % flags_num;
      key.Set(key_number);
      iterator->Seek(key.GetSlice());
      if (iterator->Valid() && iterator->Key() == key.GetSlice()) {
        ++found;
      }
      thread->stats.FinishedSingleOp();
    }
    Check(iterator->GetStatus(), "iterator");
    char message[100];
    std::snprintf(message, sizeof(message), "(%d of %d found)", found, reads_);
    thread->stats.AddMessage(message);
  }

  void DeleteSequential(ThreadState* thread) {
    DoDelete(thread, true);
  }

  void DeleteRandom(ThreadState* thread) {
    DoDelete(thread, false);
  }

  void DoDelete(ThreadState* thread, bool sequential) {
    db::WriteBatch batch;
    KeyBuffer key;
    for (int i = 0; i < num_; i += entries_per_batch_) {
      batch.Clear();
      const int batch_limit = std::min(entries_per_batch_, num_ - i);
      for (int j = 0; j < batch_limit; ++j) {
        const int key_number =
            sequential ? i + j : static_cast<int>(thread->random.Uniform(flags_num));
        key.Set(key_number);
        batch.Delete(key.GetSlice());
        thread->stats.FinishedSingleOp();
      }
      Check(db_->Write(write_options_, &batch), "delete");
    }
  }

  void ReadWhileWriting(ThreadState* thread) {
    if (thread->id > 0) {
      ReadRandom(thread);
      return;
    }

    RandomGenerator generator;
    KeyBuffer key;
    while (true) {
      {
        std::lock_guard lock(thread->shared->mutex);
        if (thread->shared->done + 1 >= thread->shared->initialized) {
          break;
        }
      }
      key.Set(static_cast<int>(thread->random.Uniform(flags_num)));
      Check(db_->Put(write_options_, key.GetSlice(),
                     generator.Generate(static_cast<size_t>(value_size_))),
            "put");
    }
    thread->stats.Start();
  }

  void Compact(ThreadState*) {
    db_->CompactRange(nullptr, nullptr);
  }

  void PrintStats(const char* property) const {
    std::string stats;
    if (!db_->GetProperty(property, &stats)) {
      stats = "(failed)";
    }
    std::fprintf(stdout, "\n%s\n", stats.c_str());
  }

  db::Cache* cache_;
  const db::FilterPolicy* filter_policy_;
  db::DB* db_ = nullptr;
  int num_;
  int value_size_;
  int entries_per_batch_ = 1;
  db::WriteOptions write_options_;
  int reads_;
  CountComparator count_comparator_;
  int total_thread_count_ = 0;
};

bool ParseBooleanFlag(const char* argument, const char* name, bool* value) {
  int parsed = 0;
  char trailing = '\0';
  std::string pattern = std::string("--") + name + "=%d%c";
  if (std::sscanf(argument, pattern.c_str(), &parsed, &trailing) == 1 &&
      (parsed == 0 || parsed == 1)) {
    *value = parsed != 0;
    return true;
  }
  return false;
}

bool ParseIntegerFlag(const char* argument, const char* name, int* value) {
  char trailing = '\0';
  std::string pattern = std::string("--") + name + "=%d%c";
  return std::sscanf(argument, pattern.c_str(), value, &trailing) == 1;
}

void ParseFlags(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    double ratio = 0;
    char trailing = '\0';
    if (db::Slice(argv[i]).StartsWith("--benchmarks=")) {
      flags_benchmarks = argv[i] + std::strlen("--benchmarks=");
    } else if (std::sscanf(argv[i], "--compression_ratio=%lf%c", &ratio, &trailing) == 1) {
      flags_compression_ratio = ratio;
    } else if (ParseBooleanFlag(argv[i], "histogram", &flags_histogram)) {
    } else if (ParseBooleanFlag(argv[i], "comparisons", &flags_comparisons)) {
    } else if (ParseBooleanFlag(argv[i], "use_existing_db", &flags_use_existing_db)) {
    } else if (ParseBooleanFlag(argv[i], "reuse_logs", &flags_reuse_logs)) {
    } else if (ParseBooleanFlag(argv[i], "compression", &flags_compression)) {
    } else if (ParseIntegerFlag(argv[i], "num", &flags_num)) {
    } else if (ParseIntegerFlag(argv[i], "reads", &flags_reads)) {
    } else if (ParseIntegerFlag(argv[i], "threads", &flags_threads)) {
    } else if (ParseIntegerFlag(argv[i], "value_size", &flags_value_size)) {
    } else if (ParseIntegerFlag(argv[i], "write_buffer_size", &flags_write_buffer_size)) {
    } else if (ParseIntegerFlag(argv[i], "max_file_size", &flags_max_file_size)) {
    } else if (ParseIntegerFlag(argv[i], "block_size", &flags_block_size)) {
    } else if (ParseIntegerFlag(argv[i], "key_prefix", &flags_key_prefix)) {
    } else if (ParseIntegerFlag(argv[i], "cache_size", &flags_cache_size)) {
    } else if (ParseIntegerFlag(argv[i], "bloom_bits", &flags_bloom_bits)) {
    } else if (ParseIntegerFlag(argv[i], "open_files", &flags_open_files)) {
    } else if (std::strncmp(argv[i], "--db=", 5) == 0) {
      flags_db = argv[i] + 5;
    } else {
      std::fprintf(stderr, "Invalid flag '%s'\n", argv[i]);
      std::exit(1);
    }
  }

  if (flags_num <= 0 || flags_threads <= 0 || flags_value_size < 0 || flags_reads < -1 ||
      flags_reads == 0 || flags_key_prefix < 0 || flags_key_prefix > 1000 ||
      !std::isfinite(flags_compression_ratio) || flags_compression_ratio <= 0 ||
      flags_compression_ratio > 1 || flags_write_buffer_size <= 0 || flags_max_file_size <= 0 ||
      flags_block_size <= 0 || flags_open_files <= 0) {
    std::fprintf(stderr, "Invalid numeric flag value\n");
    std::exit(1);
  }
}

}  // namespace

int main(int argc, char** argv) {
  const db::Options defaults;
  flags_write_buffer_size = static_cast<int>(defaults.write_buffer_size);
  flags_max_file_size = static_cast<int>(defaults.max_file_size);
  flags_block_size = static_cast<int>(defaults.block_size);
  flags_open_files = defaults.max_open_files;

  ParseFlags(argc, argv);
  env = db::Env::Default();

  std::string default_database_path;
  if (flags_db == nullptr) {
    const db::Status status = env->GetTestDirectory(&default_database_path);
    if (!status.Ok()) {
      std::fprintf(stderr, "test directory error: %s\n", status.ToString().c_str());
      return 1;
    }
    default_database_path += "/talusdb-bench";
    flags_db = default_database_path.c_str();
  }

  Benchmark benchmark;
  benchmark.Run();
  return 0;
}
