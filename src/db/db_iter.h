#pragma once

#include <cstdint>

#include "db/dbformat.h"
#include "iterator.h"

namespace db {

class DBImpl;

Iterator* NewDBIterator(DBImpl* db, const Comparator* user_key_comparator, Iterator* internal_iter,
                        SequenceNumber sequence, uint32_t seed);

}  // namespace db
