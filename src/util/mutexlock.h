#pragma once

#include "port.h"

namespace db {

class MutexLock {
public:
  explicit MutexLock(port::Mutex* mu) : mu_(mu) {
    this->mu_->Lock();
  }
  ~MutexLock() {
    this->mu_->Unlock();
  }

  MutexLock(const MutexLock&) = delete;
  MutexLock& operator=(const MutexLock&) = delete;

private:
  port::Mutex* const mu_;
};

}  // namespace db
