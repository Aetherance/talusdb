# Roadmap

## Phase 1 — Storage Primitives
- [x] Slice (`include/slice.h`)
- [x] Status (`include/status.h`)
- [x] Comparator / BytewiseComparator (`include/comparator.h`, `src/util/comparator.cc`)
- [x] Coding (varint / fixed) (`src/util/coding.h/cc`)
- [x] CRC32C (`src/util/crc32c.h/cc`)
- [x] Arena (`src/util/arena.h/cc`)
- [x] Random (`src/util/random.h`)
- [x] Block / BlockBuilder (`src/table/*`)
- [x] SSTable / TableBuilder (`include/table.h`, `include/table_builder.h`)
- [x] TwoLevelIterator (`src/table/two_level_iterator.h/cc`)
- [x] FilterPolicy / Bloom (`include/filter_policy.h`, `src/util/bloom.cc`)
- [x] FilterBlock (`src/table/filter_block.h/cc`)
- [x] InternalKey / LookupKey / InternalKeyComparator (`src/db/dbformat.h/cc`)
- [x] SkipList (`src/db/skiplist.h`)
- [x] MemTable / MemTableIterator (`src/db/memtable.h/cc`)
- [x] Env / PosixEnv (`include/env.h`, `src/util/env.cc`)
- [x] Mutex / CondVar / Snappy / Zstd / CRC32C stubs (`include/port.h`)

## Phase 2 — Foundation Utilities
- [x] `include/cache.h` + `src/util/cache.cc` — sharded LRU cache (for block cache + table cache)
- [x] `src/db/filename.h/cc` — DB file naming (CURRENT, MANIFEST-*, *.ldb, *.log, LOCK)

## Phase 3 — Write-Ahead Log
- [x] `src/db/log_format.h` — record types (kFull/kFirst/kMiddle/kLast), block size 32KB
- [x] `src/db/log_writer.h/cc` — append records to WAL file, CRC per record
- [x] `src/db/log_reader.h/cc` — read records from WAL, skip to initial block, CRC verify

## Phase 4 — Version Management
- [x] `include/write_batch.h` + `src/db/write_batch.cc` — atomic batch write
- [x] `src/db/snapshot.h` — MVCC snapshot list (doubly-linked list, ~80 lines)
- [ ] `src/db/version_edit.h/cc` — delta encoding (MANIFEST record serialize/deserialize)
- [ ] `src/table/merger.h/cc` — merge N sorted iterators (min-heap)
- [ ] `src/db/table_cache.h/cc` — LRU cache of open SSTable readers
- [ ] `src/db/version_set.h/cc` — version tree, compaction picking, MANIFEST I/O

## Phase 5 — Read Path
- [ ] `include/db.h` — public DB interface (Open / Get / Put / Delete / Write / NewIterator / GetSnapshot / ReleaseSnapshot / CompactRange / DestroyDB / RepairDB / GetProperty / GetApproximateSizes)
- [ ] `src/db/db_iter.h/cc` — DBIter: merge memtable + SSTable with snapshot isolation
- [ ] DBImpl::Get() — check memtable → check immutable memtable → check SST levels

## Phase 6 — Write Path
- [ ] `src/db/builder.h/cc` — BuildTable(): iterator → SSTable (uses existing TableBuilder)
- [ ] `src/db/db_impl.h/cc`:
  - [ ] Open() / Recover(): read CURRENT → MANIFEST → replay WAL → rebuild memtable
  - [ ] Put() / Delete(): append to WAL → insert into memtable
  - [ ] Write() (batch): group commit, WAL + memtable
  - [ ] MemTable flush: freezing → BuildTable → install new version

## Phase 7 — Compaction
- [ ] `db_impl.cc` — MaybeScheduleCompaction()
- [ ] `version_set.cc` — PickCompaction() (level-0 size ratio + level-N size ratio)
- [ ] `db_impl.cc` — DoCompactionWork(): open inputs → MergingIterator → BuildTable → install edit
- [ ] `db_impl.cc` — BackgroundCompaction() main loop

## Phase 8 — Utilities
- [ ] `src/db/dumpfile.cc` + `include/dumpfile.h` — dump SSTable contents for debugging
- [ ] `src/db/repair.cc` — RepairDB(): recover as much data as possible from corrupted DB
- [ ] `src/db/c.cc` + `include/c.h` — C language bindings (optional)