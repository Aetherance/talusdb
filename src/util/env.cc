#include "env.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <utility>

namespace db {

Env::Env() = default;
Env::~Env() = default;

Status Env::NewAppendableFile(const std::string& fname, WritableFile** result) {
  if (result != nullptr) {
    *result = nullptr;
  }
  return Status::NotSupported("NewAppendableFile", fname);
}

Status Env::RemoveFile(const std::string& fname) {
  return DeleteFile(fname);
}

Status Env::DeleteFile(const std::string& fname) {
  return RemoveFile(fname);
}

Status Env::RemoveDir(const std::string& dirname) {
  return DeleteDir(dirname);
}

Status Env::DeleteDir(const std::string& dirname) {
  return RemoveDir(dirname);
}

SequentialFile::~SequentialFile() = default;
RandomAccessFile::~RandomAccessFile() = default;
WritableFile::~WritableFile() = default;
Logger::~Logger() = default;
FileLock::~FileLock() = default;
EnvWrapper::~EnvWrapper() = default;

void Log(Logger* info_log, const char* format, ...) {
  if (info_log == nullptr) {
    return;
  }

  std::va_list ap;
  va_start(ap, format);
  info_log->Logv(format, ap);
  va_end(ap);
}

Status WriteStringToFile(Env* env, const Slice& data, const std::string& fname) {
  WritableFile* file = nullptr;
  Status status = env->NewWritableFile(fname, &file);
  if (!status.Ok()) {
    return status;
  }

  status = file->Append(data);
  if (status.Ok()) {
    status = file->Close();
  } else {
    file->Close();
  }
  delete file;

  if (!status.Ok()) {
    env->RemoveFile(fname);
  }
  return status;
}

Status ReadFileToString(Env* env, const std::string& fname, std::string* data) {
  SequentialFile* file = nullptr;
  Status status = env->NewSequentialFile(fname, &file);
  if (!status.Ok()) {
    return status;
  }

  data->clear();
  char scratch[8192];
  Slice fragment;
  while (true) {
    status = file->Read(sizeof(scratch), &fragment, scratch);
    if (!status.Ok()) {
      break;
    }
    data->append(fragment.Data(), fragment.Size());
    if (fragment.Empty()) {
      break;
    }
  }

  delete file;
  return status;
}

namespace {

int g_open_read_only_file_limit = -1;
constexpr int kDefaultMmapLimit = (sizeof(void*) >= 8) ? 1000 : 0;
int g_mmap_limit = kDefaultMmapLimit;

#ifdef O_CLOEXEC
constexpr int kOpenBaseFlags = O_CLOEXEC;
#else
constexpr int kOpenBaseFlags = 0;
#endif

constexpr size_t kWritableFileBufferSize = 65536;

Status PosixError(const std::string& context, int error_number) {
  if (error_number == ENOENT) {
    return Status::NotFound(context, std::strerror(error_number));
  }
  return Status::IOError(context, std::strerror(error_number));
}

class Limiter {
public:
  explicit Limiter(int max_acquires)
      :
#if !defined(NDEBUG)
        max_acquires_(max_acquires),
#endif
        acquires_allowed_(max_acquires) {
  }

  Limiter(const Limiter&) = delete;
  Limiter& operator=(const Limiter&) = delete;

  bool Acquire() {
    const int old_acquires_allowed = acquires_allowed_.fetch_sub(1, std::memory_order_relaxed);
    if (old_acquires_allowed > 0) {
      return true;
    }

    const int pre_increment_acquires_allowed =
        acquires_allowed_.fetch_add(1, std::memory_order_relaxed);
    (void)pre_increment_acquires_allowed;
#if !defined(NDEBUG)
    assert(pre_increment_acquires_allowed < max_acquires_);
#endif
    return false;
  }

  void Release() {
    const int old_acquires_allowed = acquires_allowed_.fetch_add(1, std::memory_order_relaxed);
    (void)old_acquires_allowed;
#if !defined(NDEBUG)
    assert(old_acquires_allowed < max_acquires_);
#endif
  }

private:
#if !defined(NDEBUG)
  const int max_acquires_;
#endif
  std::atomic<int> acquires_allowed_;
};

bool ConvertOffset(uint64_t value, off_t* result) {
  if (value > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) {
    return false;
  }
  *result = static_cast<off_t>(value);
  return true;
}

class PosixSequentialFile final : public SequentialFile {
public:
  PosixSequentialFile(std::string filename, int fd) : fd_(fd), filename_(std::move(filename)) {}

  ~PosixSequentialFile() override {
    ::close(fd_);
  }

  Status Read(size_t n, Slice* result, char* scratch) override {
    while (true) {
      const ssize_t read_size = ::read(fd_, scratch, n);
      if (read_size < 0) {
        if (errno == EINTR) {
          continue;
        }
        return PosixError(filename_, errno);
      }
      *result = Slice(scratch, static_cast<size_t>(read_size));
      return Status::OkStatus();
    }
  }

  Status Skip(uint64_t n) override {
    off_t offset = 0;
    if (!ConvertOffset(n, &offset)) {
      return PosixError(filename_, EINVAL);
    }
    if (::lseek(fd_, offset, SEEK_CUR) == static_cast<off_t>(-1)) {
      return PosixError(filename_, errno);
    }
    return Status::OkStatus();
  }

private:
  const int fd_;
  const std::string filename_;
};

class PosixRandomAccessFile final : public RandomAccessFile {
public:
  PosixRandomAccessFile(std::string filename, int fd, Limiter* fd_limiter)
      : has_permanent_fd_(fd_limiter->Acquire()),
        fd_(has_permanent_fd_ ? fd : -1),
        fd_limiter_(fd_limiter),
        filename_(std::move(filename)) {
    if (!has_permanent_fd_) {
      ::close(fd);
    }
  }

  ~PosixRandomAccessFile() override {
    if (has_permanent_fd_) {
      ::close(fd_);
      fd_limiter_->Release();
    }
  }

  Status Read(uint64_t offset, size_t n, Slice* result, char* scratch) const override {
    off_t read_offset = 0;
    if (!ConvertOffset(offset, &read_offset)) {
      *result = Slice();
      return PosixError(filename_, EINVAL);
    }

    int fd = fd_;
    if (!has_permanent_fd_) {
      fd = ::open(filename_.c_str(), O_RDONLY | kOpenBaseFlags);
      if (fd < 0) {
        *result = Slice();
        return PosixError(filename_, errno);
      }
    }

    ssize_t read_size = 0;
    while (true) {
      read_size = ::pread(fd, scratch, n, read_offset);
      if (read_size < 0 && errno == EINTR) {
        continue;
      }
      break;
    }
    if (!has_permanent_fd_) {
      ::close(fd);
    }

    if (read_size < 0) {
      *result = Slice();
      return PosixError(filename_, errno);
    }

    *result = Slice(scratch, static_cast<size_t>(read_size));
    return Status::OkStatus();
  }

private:
  const bool has_permanent_fd_;
  const int fd_;
  Limiter* const fd_limiter_;
  const std::string filename_;
};

class PosixMmapReadableFile final : public RandomAccessFile {
public:
  PosixMmapReadableFile(std::string filename, char* mmap_base, size_t length, Limiter* mmap_limiter)
      : mmap_base_(mmap_base),
        length_(length),
        mmap_limiter_(mmap_limiter),
        filename_(std::move(filename)) {}

  ~PosixMmapReadableFile() override {
    ::munmap(static_cast<void*>(mmap_base_), length_);
    mmap_limiter_->Release();
  }

  Status Read(uint64_t offset, size_t n, Slice* result, char* scratch) const override {
    (void)scratch;
    if (offset > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
      *result = Slice();
      return PosixError(filename_, EINVAL);
    }

    const size_t offset_size = static_cast<size_t>(offset);
    if (offset_size > length_ || n > length_ - offset_size) {
      *result = Slice();
      return PosixError(filename_, EINVAL);
    }

    *result = Slice(mmap_base_ + offset_size, n);
    return Status::OkStatus();
  }

private:
  char* const mmap_base_;
  const size_t length_;
  Limiter* const mmap_limiter_;
  const std::string filename_;
};

class PosixLogger final : public Logger {
public:
  explicit PosixLogger(std::FILE* file) : file_(file) {}

  ~PosixLogger() override {
    if (file_ != nullptr) {
      std::fclose(file_);
    }
  }

  void Logv(const char* format, std::va_list ap) override {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vfprintf(file_, format, ap);
    std::fputc('\n', file_);
    std::fflush(file_);
  }

private:
  std::mutex mutex_;
  std::FILE* file_;
};

class PosixWritableFile final : public WritableFile {
public:
  PosixWritableFile(std::string filename, int fd)
      : pos_(0),
        fd_(fd),
        is_manifest_(IsManifest(filename)),
        filename_(std::move(filename)),
        dirname_(Dirname(filename_)) {}

  ~PosixWritableFile() override {
    if (fd_ >= 0) {
      Close();
    }
  }

  Status Append(const Slice& data) override {
    size_t write_size = data.Size();
    const char* write_data = data.Data();

    const size_t copy_size = std::min(write_size, kWritableFileBufferSize - pos_);
    std::memcpy(buf_ + pos_, write_data, copy_size);
    write_data += copy_size;
    write_size -= copy_size;
    pos_ += copy_size;
    if (write_size == 0) {
      return Status::OkStatus();
    }

    Status status = FlushBuffer();
    if (!status.Ok()) {
      return status;
    }

    if (write_size < kWritableFileBufferSize) {
      std::memcpy(buf_, write_data, write_size);
      pos_ = write_size;
      return Status::OkStatus();
    }
    return WriteUnbuffered(write_data, write_size);
  }

  Status Close() override {
    Status status = FlushBuffer();
    if (::close(fd_) != 0 && status.Ok()) {
      status = PosixError(filename_, errno);
    }
    fd_ = -1;
    return status;
  }

  Status Flush() override {
    return FlushBuffer();
  }

  Status Sync() override {
    Status status = SyncDirIfManifest();
    if (!status.Ok()) {
      return status;
    }

    status = FlushBuffer();
    if (!status.Ok()) {
      return status;
    }

    return SyncFd(fd_, filename_);
  }

private:
  Status FlushBuffer() {
    const Status status = WriteUnbuffered(buf_, pos_);
    pos_ = 0;
    return status;
  }

  Status WriteUnbuffered(const char* data, size_t size) {
    while (size > 0) {
      const ssize_t write_result = ::write(fd_, data, size);
      if (write_result < 0) {
        if (errno == EINTR) {
          continue;
        }
        return PosixError(filename_, errno);
      }
      if (write_result == 0) {
        return Status::IOError(filename_, "write returned 0");
      }

      const size_t written = static_cast<size_t>(write_result);
      data += written;
      size -= written;
    }
    return Status::OkStatus();
  }

  Status SyncDirIfManifest() {
    if (!is_manifest_) {
      return Status::OkStatus();
    }

    const int fd = ::open(dirname_.c_str(), O_RDONLY | kOpenBaseFlags);
    if (fd < 0) {
      return PosixError(dirname_, errno);
    }

    const Status status = SyncFd(fd, dirname_);
    ::close(fd);
    return status;
  }

  static Status SyncFd(int fd, const std::string& fd_path) {
    if (::fsync(fd) == 0) {
      return Status::OkStatus();
    }
    return PosixError(fd_path, errno);
  }

  static std::string Dirname(const std::string& filename) {
    const std::string::size_type separator_pos = filename.rfind('/');
    if (separator_pos == std::string::npos) {
      return ".";
    }
    return filename.substr(0, separator_pos);
  }

  static Slice Basename(const std::string& filename) {
    const std::string::size_type separator_pos = filename.rfind('/');
    if (separator_pos == std::string::npos) {
      return Slice(filename);
    }
    return Slice(filename.data() + separator_pos + 1, filename.size() - separator_pos - 1);
  }

  static bool IsManifest(const std::string& filename) {
    return Basename(filename).StartsWith(Slice("MANIFEST"));
  }

  char buf_[kWritableFileBufferSize];
  size_t pos_;
  int fd_;
  const bool is_manifest_;
  const std::string filename_;
  const std::string dirname_;
};

int LockOrUnlock(int fd, bool lock) {
  errno = 0;
  struct ::flock file_lock_info;
  std::memset(&file_lock_info, 0, sizeof(file_lock_info));
  file_lock_info.l_type = lock ? static_cast<short>(F_WRLCK) : static_cast<short>(F_UNLCK);
  file_lock_info.l_whence = static_cast<short>(SEEK_SET);
  file_lock_info.l_start = 0;
  file_lock_info.l_len = 0;
  return ::fcntl(fd, F_SETLK, &file_lock_info);
}

class PosixFileLock final : public FileLock {
public:
  PosixFileLock(int fd, std::string filename) : fd_(fd), filename_(std::move(filename)) {}

  int fd() const {
    return fd_;
  }

  const std::string& filename() const {
    return filename_;
  }

private:
  const int fd_;
  const std::string filename_;
};

class PosixLockTable {
public:
  bool Insert(const std::string& fname) {
    std::lock_guard<std::mutex> lock(mutex_);
    return locked_files_.insert(fname).second;
  }

  void Remove(const std::string& fname) {
    std::lock_guard<std::mutex> lock(mutex_);
    locked_files_.erase(fname);
  }

private:
  std::mutex mutex_;
  std::set<std::string> locked_files_;
};

class PosixEnv final : public Env {
public:
  PosixEnv()
      : started_background_thread_(false), mmap_limiter_(MaxMmaps()), fd_limiter_(MaxOpenFiles()) {}

  Status NewSequentialFile(const std::string& filename, SequentialFile** result) override {
    const int fd = ::open(filename.c_str(), O_RDONLY | kOpenBaseFlags);
    if (fd < 0) {
      *result = nullptr;
      return PosixError(filename, errno);
    }

    *result = new PosixSequentialFile(filename, fd);
    return Status::OkStatus();
  }

  Status NewRandomAccessFile(const std::string& filename, RandomAccessFile** result) override {
    *result = nullptr;

    const int fd = ::open(filename.c_str(), O_RDONLY | kOpenBaseFlags);
    if (fd < 0) {
      return PosixError(filename, errno);
    }

    if (!mmap_limiter_.Acquire()) {
      *result = new PosixRandomAccessFile(filename, fd, &fd_limiter_);
      return Status::OkStatus();
    }

    uint64_t file_size = 0;
    Status status = GetFileSize(filename, &file_size);
    if (status.Ok() && file_size > 0 &&
        file_size <= static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
      void* mmap_base =
          ::mmap(nullptr, static_cast<size_t>(file_size), PROT_READ, MAP_SHARED, fd, 0);
      if (mmap_base != MAP_FAILED) {
        *result = new PosixMmapReadableFile(filename, reinterpret_cast<char*>(mmap_base),
                                            static_cast<size_t>(file_size), &mmap_limiter_);
      } else {
        status = PosixError(filename, errno);
      }
    } else if (status.Ok()) {
      *result = new PosixRandomAccessFile(filename, fd, &fd_limiter_);
      mmap_limiter_.Release();
      return Status::OkStatus();
    }

    ::close(fd);
    if (!status.Ok()) {
      mmap_limiter_.Release();
    }
    return status;
  }

  Status NewWritableFile(const std::string& filename, WritableFile** result) override {
    const int fd = ::open(filename.c_str(), O_TRUNC | O_WRONLY | O_CREAT | kOpenBaseFlags, 0644);
    if (fd < 0) {
      *result = nullptr;
      return PosixError(filename, errno);
    }

    *result = new PosixWritableFile(filename, fd);
    return Status::OkStatus();
  }

  Status NewAppendableFile(const std::string& filename, WritableFile** result) override {
    const int fd = ::open(filename.c_str(), O_APPEND | O_WRONLY | O_CREAT | kOpenBaseFlags, 0644);
    if (fd < 0) {
      *result = nullptr;
      return PosixError(filename, errno);
    }

    *result = new PosixWritableFile(filename, fd);
    return Status::OkStatus();
  }

  bool FileExists(const std::string& filename) override {
    return ::access(filename.c_str(), F_OK) == 0;
  }

  Status GetChildren(const std::string& directory_path, std::vector<std::string>* result) override {
    result->clear();
    ::DIR* dir = ::opendir(directory_path.c_str());
    if (dir == nullptr) {
      return PosixError(directory_path, errno);
    }

    while (true) {
      errno = 0;
      struct ::dirent* entry = ::readdir(dir);
      if (entry == nullptr) {
        break;
      }
      result->emplace_back(entry->d_name);
    }

    const int saved_errno = errno;
    ::closedir(dir);
    if (saved_errno != 0) {
      return PosixError(directory_path, saved_errno);
    }
    return Status::OkStatus();
  }

  Status RemoveFile(const std::string& filename) override {
    if (::unlink(filename.c_str()) != 0) {
      return PosixError(filename, errno);
    }
    return Status::OkStatus();
  }

  Status CreateDir(const std::string& dirname) override {
    if (::mkdir(dirname.c_str(), 0755) != 0) {
      return PosixError(dirname, errno);
    }
    return Status::OkStatus();
  }

  Status RemoveDir(const std::string& dirname) override {
    if (::rmdir(dirname.c_str()) != 0) {
      return PosixError(dirname, errno);
    }
    return Status::OkStatus();
  }

  Status GetFileSize(const std::string& filename, uint64_t* size) override {
    struct ::stat file_stat;
    if (::stat(filename.c_str(), &file_stat) != 0) {
      *size = 0;
      return PosixError(filename, errno);
    }
    if (file_stat.st_size < 0) {
      *size = 0;
      return PosixError(filename, EINVAL);
    }
    *size = static_cast<uint64_t>(file_stat.st_size);
    return Status::OkStatus();
  }

  Status RenameFile(const std::string& from, const std::string& to) override {
    if (::rename(from.c_str(), to.c_str()) != 0) {
      return PosixError(from, errno);
    }
    return Status::OkStatus();
  }

  Status LockFile(const std::string& filename, FileLock** lock) override {
    *lock = nullptr;

    const int fd = ::open(filename.c_str(), O_RDWR | O_CREAT | kOpenBaseFlags, 0644);
    if (fd < 0) {
      return PosixError(filename, errno);
    }

    if (!locks_.Insert(filename)) {
      ::close(fd);
      return Status::IOError("lock " + filename, "already held by process");
    }

    if (LockOrUnlock(fd, true) == -1) {
      const int lock_errno = errno;
      ::close(fd);
      locks_.Remove(filename);
      return PosixError("lock " + filename, lock_errno);
    }

    *lock = new PosixFileLock(fd, filename);
    return Status::OkStatus();
  }

  Status UnlockFile(FileLock* lock) override {
    auto* posix_file_lock = static_cast<PosixFileLock*>(lock);
    if (LockOrUnlock(posix_file_lock->fd(), false) == -1) {
      return PosixError("unlock " + posix_file_lock->filename(), errno);
    }

    locks_.Remove(posix_file_lock->filename());
    ::close(posix_file_lock->fd());
    delete posix_file_lock;
    return Status::OkStatus();
  }

  void Schedule(void (*background_work_function)(void* background_work_arg),
                void* background_work_arg) override {
    std::unique_lock<std::mutex> lock(background_work_mutex_);

    if (!started_background_thread_) {
      started_background_thread_ = true;
      std::thread background_thread(&PosixEnv::BackgroundThreadMain, this);
      background_thread.detach();
    }

    background_work_queue_.emplace(background_work_function, background_work_arg);
    lock.unlock();
    background_work_cv_.notify_one();
  }

  void StartThread(void (*thread_main)(void* thread_main_arg), void* thread_main_arg) override {
    std::thread new_thread(thread_main, thread_main_arg);
    new_thread.detach();
  }

  Status GetTestDirectory(std::string* result) override {
    const char* env = std::getenv("TEST_TMPDIR");
    if (env != nullptr && env[0] != '\0') {
      *result = env;
    } else {
      char buffer[100];
      std::snprintf(buffer, sizeof(buffer), "/tmp/dbtest-%d", static_cast<int>(::geteuid()));
      *result = buffer;
    }

    if (::mkdir(result->c_str(), 0755) != 0 && errno != EEXIST) {
      return PosixError(*result, errno);
    }
    return Status::OkStatus();
  }

  Status NewLogger(const std::string& filename, Logger** result) override {
    const int fd = ::open(filename.c_str(), O_APPEND | O_WRONLY | O_CREAT | kOpenBaseFlags, 0644);
    if (fd < 0) {
      *result = nullptr;
      return PosixError(filename, errno);
    }

    std::FILE* file = ::fdopen(fd, "w");
    if (file == nullptr) {
      ::close(fd);
      *result = nullptr;
      return PosixError(filename, errno);
    }

    *result = new PosixLogger(file);
    return Status::OkStatus();
  }

  uint64_t NowMicros() override {
    constexpr uint64_t kUsecondsPerSecond = 1000000;
    struct ::timeval tv;
    ::gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * kUsecondsPerSecond +
           static_cast<uint64_t>(tv.tv_usec);
  }

  void SleepForMicroseconds(int micros) override {
    if (micros <= 0) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(micros));
  }

private:
  struct BackgroundWorkItem {
    BackgroundWorkItem(void (*function)(void* arg), void* arg) : function(function), arg(arg) {}

    void (*function)(void*);
    void* arg;
  };

  void BackgroundThreadMain() {
    while (true) {
      BackgroundWorkItem item(nullptr, nullptr);
      {
        std::unique_lock<std::mutex> lock(background_work_mutex_);
        background_work_cv_.wait(lock, [this] { return !background_work_queue_.empty(); });
        item = background_work_queue_.front();
        background_work_queue_.pop();
      }
      item.function(item.arg);
    }
  }

  static int MaxMmaps() {
    return g_mmap_limit;
  }

  static int MaxOpenFiles() {
    if (g_open_read_only_file_limit >= 0) {
      return g_open_read_only_file_limit;
    }

    struct ::rlimit rlim;
    if (::getrlimit(RLIMIT_NOFILE, &rlim) != 0) {
      g_open_read_only_file_limit = 50;
      return g_open_read_only_file_limit;
    }

    if (rlim.rlim_cur == RLIM_INFINITY) {
      g_open_read_only_file_limit = std::numeric_limits<int>::max();
      return g_open_read_only_file_limit;
    }

    const rlim_t open_read_only_limit = rlim.rlim_cur / 5;
    if (open_read_only_limit > static_cast<rlim_t>(std::numeric_limits<int>::max())) {
      g_open_read_only_file_limit = std::numeric_limits<int>::max();
    } else {
      g_open_read_only_file_limit = static_cast<int>(open_read_only_limit);
    }
    return g_open_read_only_file_limit;
  }

  std::mutex background_work_mutex_;
  std::condition_variable background_work_cv_;
  bool started_background_thread_;
  std::queue<BackgroundWorkItem> background_work_queue_;
  PosixLockTable locks_;
  Limiter mmap_limiter_;
  Limiter fd_limiter_;
};

}  // namespace

Env* Env::Default() {
  static PosixEnv* default_env = new PosixEnv();
  return default_env;
}

}  // namespace db
