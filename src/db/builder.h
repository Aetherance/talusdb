#pragma once

#include <string>

#include "status.h"

namespace db {

struct FileMetaData;
struct Options;

class Env;
class Iterator;
class TableCache;

Status BuildTable(const std::string& dbname, Env* env, const Options& options,
                  TableCache* table_cache, Iterator* iter, FileMetaData* meta);

}  // namespace db
