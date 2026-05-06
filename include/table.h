#include <cstdint>

#include "env.h"
#include "iterator.h"
#include "options.h"
#include "slice.h"
#include "status.h"
#include "table/block.h"
#include "table/format.h"

namespace db {

class Table {
public:
  static Status Open(const Options& options, RandomAccessFile* file, uint64_t file_size,
                     Table** table);

  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;

  ~Table();

  Iterator* NewIterator(const ReadOptions&) const;

  uint64_t ApproximateOffsetOf(const Slice& key) const;

private:
  friend class TableCache;
  struct Rep;

  static Iterator* BlockReader(void*, const ReadOptions&, const Slice&);

  explicit Table(Rep* rep) : rep_(rep) {}

  Status InternalGet(const ReadOptions&, const Slice& key, void* arg,
                     void (*handle_result)(void* arg, const Slice& k, const Slice& v));

  void ReadMeta(const Footer& footer);
  void ReadFilter(const Slice& filter_handle_value);

  Rep* const rep_;
};

}  // namespace db