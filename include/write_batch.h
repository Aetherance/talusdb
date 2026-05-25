#pragma once

#include <cstddef>
#include <string>

#include "status.h"

namespace db {
class Slice;

class WriteBatch {
public:
  class Handler {
  public:
    virtual ~Handler();
    virtual void Put(const Slice& key, const Slice& value) = 0;
    virtual void Delete(const Slice& key) = 0;
  };

  WriteBatch();

  WriteBatch(const WriteBatch&) = default;
  WriteBatch& operator=(const WriteBatch&) = default;

  ~WriteBatch();

  void Put(const Slice& key, const Slice& value);

  void Delete(const Slice& key);

  void Clear();

  size_t ApproximateSize() const;

  void Append(const WriteBatch& source);

  Status Iterate(Handler* handler) const;

private:
  friend class WriteBatchInternal;

  std::string rep_;
};
}  // namespace db