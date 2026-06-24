#include "db/builder.h"

#include <cassert>
#include <string>

#include "db/filename.h"
#include "db/table_cache.h"
#include "db/version_edit.h"
#include "env.h"
#include "iterator.h"
#include "options.h"
#include "table_builder.h"

namespace db {

Status BuildTable(const std::string& dbname, Env* env, const Options& options,
                  TableCache* table_cache, Iterator* iter, FileMetaData* meta) {
  Status status;
  meta->file_size = 0;
  iter->SeekToFirst();

  const std::string filename = TableFileName(dbname, meta->number);
  if (iter->Valid()) {
    WritableFile* file = nullptr;
    status = env->NewWritableFile(filename, &file);
    if (!status.Ok()) {
      return status;
    }

    TableBuilder* builder = new TableBuilder(options, file);
    meta->smallest.DecodeFrom(iter->Key());
    Slice key;
    for (; iter->Valid(); iter->Next()) {
      key = iter->Key();
      builder->Add(key, iter->Value());
    }
    if (!key.Empty()) {
      meta->largest.DecodeFrom(key);
    }

    status = builder->Finish();
    if (status.Ok()) {
      meta->file_size = builder->FileSize();
      assert(meta->file_size > 0);
    }
    delete builder;

    if (status.Ok()) {
      status = file->Sync();
    }
    if (status.Ok()) {
      status = file->Close();
    }
    delete file;

    if (status.Ok()) {
      Iterator* table_iter = table_cache->NewIterator(ReadOptions(), meta->number, meta->file_size);
      status = table_iter->GetStatus();
      delete table_iter;
    }
  }

  if (!iter->GetStatus().Ok()) {
    status = iter->GetStatus();
  }

  if (!status.Ok() || meta->file_size == 0) {
    env->RemoveFile(filename);
  }
  return status;
}

}  // namespace db
