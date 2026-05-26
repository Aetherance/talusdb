#include <string>

#include "cache.h"
#include "iterator.h"
#include "options.h"
#include "table.h"

namespace db {
class Env;

class TableCache {
public:
  TableCache(const std::string& dbname, const Options& options, int entries);

  TableCache(const TableCache&) = delete;
  TableCache& operator=(const TableCache&) = delete;

  ~TableCache();

  Iterator* NewIterator(const ReadOptions& options, uint64_t file_number, uint64_t file_size,
                        Table** tableptr = nullptr);

  Status Get(const ReadOptions& options, uint64_t file_number, uint64_t file_size, const Slice& k,
             void* arg, void (*handle_result)(void*, const Slice&, const Slice&));

  void Evict(uint64_t file_number);

private:
  Status FindTable(uint64_t file_number, uint64_t file_size, Cache::Handle**);

  Env* const env_;
  const std::string dbname_;
  const Options& options_;
  Cache* cache_;
};
}  // namespace db