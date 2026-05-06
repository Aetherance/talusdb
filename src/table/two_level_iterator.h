#include "iterator.h"
#include "options.h"
namespace db {
struct ReadOptions;

Iterator* NewTwoLevelIterator(Iterator* index_iter,
                              Iterator* (*block_function)(void* arg, const ReadOptions& options,
                                                          const Slice& index_value),
                              void* arg, const ReadOptions& options);
}  // namespace db