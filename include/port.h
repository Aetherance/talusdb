#ifndef DB_PORT_H_
#define DB_PORT_H_

#ifndef HAVE_CRC32C
#define HAVE_CRC32C 0
#endif

#ifndef HAVE_SNAPPY
#define HAVE_SNAPPY 0
#endif

#ifndef HAVE_ZSTD
#define HAVE_ZSTD 0
#endif

#if HAVE_SNAPPY
#include <snappy.h>
#endif  // HAVE_SNAPPY
#if HAVE_ZSTD
#define ZSTD_STATIC_LINKING_ONLY  // For ZSTD_compressionParameters.
#include <zstd.h>
#endif  // HAVE_ZSTD

#include <algorithm>
#include <cassert>
#include <condition_variable>  // NOLINT
#include <cstddef>
#include <cstdint>
#include <mutex>  // NOLINT
#include <string>

#if defined(__clang__)
#define DB_THREAD_ANNOTATION_ATTRIBUTE__(x) __attribute__((x))
#else
#define DB_THREAD_ANNOTATION_ATTRIBUTE__(x)
#endif

#define GUARDED_BY(x) DB_THREAD_ANNOTATION_ATTRIBUTE__(guarded_by(x))
#define PT_GUARDED_BY(x) DB_THREAD_ANNOTATION_ATTRIBUTE__(pt_guarded_by(x))
#define ACQUIRED_AFTER(...) DB_THREAD_ANNOTATION_ATTRIBUTE__(acquired_after(__VA_ARGS__))
#define ACQUIRED_BEFORE(...) DB_THREAD_ANNOTATION_ATTRIBUTE__(acquired_before(__VA_ARGS__))
#define EXCLUSIVE_LOCKS_REQUIRED(...) \
  DB_THREAD_ANNOTATION_ATTRIBUTE__(exclusive_locks_required(__VA_ARGS__))
#define SHARED_LOCKS_REQUIRED(...) \
  DB_THREAD_ANNOTATION_ATTRIBUTE__(shared_locks_required(__VA_ARGS__))
#define LOCKS_EXCLUDED(...) DB_THREAD_ANNOTATION_ATTRIBUTE__(locks_excluded(__VA_ARGS__))
#define LOCK_RETURNED(x) DB_THREAD_ANNOTATION_ATTRIBUTE__(lock_returned(x))
#define LOCKABLE DB_THREAD_ANNOTATION_ATTRIBUTE__(lockable)
#define SCOPED_LOCKABLE DB_THREAD_ANNOTATION_ATTRIBUTE__(scoped_lockable)
#define EXCLUSIVE_LOCK_FUNCTION(...) \
  DB_THREAD_ANNOTATION_ATTRIBUTE__(exclusive_lock_function(__VA_ARGS__))
#define SHARED_LOCK_FUNCTION(...) \
  DB_THREAD_ANNOTATION_ATTRIBUTE__(shared_lock_function(__VA_ARGS__))
#define EXCLUSIVE_TRYLOCK_FUNCTION(...) \
  DB_THREAD_ANNOTATION_ATTRIBUTE__(exclusive_trylock_function(__VA_ARGS__))
#define SHARED_TRYLOCK_FUNCTION(...) \
  DB_THREAD_ANNOTATION_ATTRIBUTE__(shared_trylock_function(__VA_ARGS__))
#define UNLOCK_FUNCTION(...) DB_THREAD_ANNOTATION_ATTRIBUTE__(unlock_function(__VA_ARGS__))
#define ASSERT_EXCLUSIVE_LOCK(...) DB_THREAD_ANNOTATION_ATTRIBUTE__(assert_capability(__VA_ARGS__))
#define ASSERT_SHARED_LOCK(...) \
  DB_THREAD_ANNOTATION_ATTRIBUTE__(assert_shared_capability(__VA_ARGS__))
#define NO_THREAD_SAFETY_ANALYSIS DB_THREAD_ANNOTATION_ATTRIBUTE__(no_thread_safety_analysis)

namespace db {
namespace port {

class CondVar;

class LOCKABLE Mutex {
public:
  Mutex() = default;
  ~Mutex() = default;

  Mutex(const Mutex&) = delete;
  Mutex& operator=(const Mutex&) = delete;

  void Lock() EXCLUSIVE_LOCK_FUNCTION() {
    mu_.lock();
  }
  void Unlock() UNLOCK_FUNCTION() {
    mu_.unlock();
  }
  void AssertHeld() ASSERT_EXCLUSIVE_LOCK() {}

private:
  friend class CondVar;
  std::mutex mu_;
};

class CondVar {
public:
  explicit CondVar(Mutex* mu) : mu_(mu) {
    assert(mu != nullptr);
  }
  ~CondVar() = default;

  CondVar(const CondVar&) = delete;
  CondVar& operator=(const CondVar&) = delete;

  void Wait() {
    std::unique_lock<std::mutex> lock(mu_->mu_, std::adopt_lock);
    cv_.wait(lock);
    lock.release();
  }

  void Signal() {
    cv_.notify_one();
  }
  void SignalAll() {
    cv_.notify_all();
  }

private:
  std::condition_variable cv_;
  Mutex* const mu_;
};

inline bool Snappy_Compress(const char* input, size_t length, std::string* output) {
#if HAVE_SNAPPY
  output->resize(snappy::MaxCompressedLength(length));
  size_t outlen = 0;
  snappy::RawCompress(input, length, &(*output)[0], &outlen);
  output->resize(outlen);
  return true;
#else
  (void)input;
  (void)length;
  (void)output;
  return false;
#endif  // HAVE_SNAPPY
}

inline bool Snappy_GetUncompressedLength(const char* input, size_t length, size_t* result) {
#if HAVE_SNAPPY
  return snappy::GetUncompressedLength(input, length, result);
#else
  (void)input;
  (void)length;
  (void)result;
  return false;
#endif  // HAVE_SNAPPY
}

inline bool Snappy_Uncompress(const char* input, size_t length, char* output) {
#if HAVE_SNAPPY
  return snappy::RawUncompress(input, length, output);
#else
  (void)input;
  (void)length;
  (void)output;
  return false;
#endif  // HAVE_SNAPPY
}

inline bool Zstd_Compress(int level, const char* input, size_t length, std::string* output) {
#if HAVE_ZSTD
  const size_t max_outlen = ZSTD_compressBound(length);
  if (ZSTD_isError(max_outlen)) {
    return false;
  }

  output->resize(max_outlen);
  ZSTD_CCtx* const ctx = ZSTD_createCCtx();
  if (ctx == nullptr) {
    return false;
  }

  const ZSTD_compressionParameters parameters =
      ZSTD_getCParams(level, std::max(length, size_t{1}), 0);
  const size_t set_params_result = ZSTD_CCtx_setCParams(ctx, parameters);
  if (ZSTD_isError(set_params_result)) {
    ZSTD_freeCCtx(ctx);
    return false;
  }

  size_t outlen = ZSTD_compress2(ctx, &(*output)[0], output->size(), input, length);
  ZSTD_freeCCtx(ctx);
  if (ZSTD_isError(outlen)) {
    return false;
  }

  output->resize(outlen);
  return true;
#else
  (void)level;
  (void)input;
  (void)length;
  (void)output;
  return false;
#endif  // HAVE_ZSTD
}

inline bool Zstd_GetUncompressedLength(const char* input, size_t length, size_t* result) {
#if HAVE_ZSTD
  const unsigned long long size = ZSTD_getFrameContentSize(input, length);
  if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN) {
    return false;
  }

  *result = static_cast<size_t>(size);
  return true;
#else
  (void)input;
  (void)length;
  (void)result;
  return false;
#endif  // HAVE_ZSTD
}

inline bool Zstd_Uncompress(const char* input, size_t length, char* output) {
#if HAVE_ZSTD
  size_t outlen = 0;
  if (!Zstd_GetUncompressedLength(input, length, &outlen)) {
    return false;
  }

  ZSTD_DCtx* const ctx = ZSTD_createDCtx();
  if (ctx == nullptr) {
    return false;
  }

  const size_t result = ZSTD_decompressDCtx(ctx, output, outlen, input, length);
  ZSTD_freeDCtx(ctx);
  if (ZSTD_isError(result)) {
    return false;
  }

  return true;
#else
  (void)input;
  (void)length;
  (void)output;
  return false;
#endif  // HAVE_ZSTD
}

inline bool GetHeapProfile(void (*func)(void*, const char*, int), void* arg) {
  (void)func;
  (void)arg;
  return false;
}

}  // namespace port
}  // namespace db

#endif  // DB_PORT_H_
