# db

An embedded LSM-tree based key-value storage engine implemented in C++, inspired by LevelDB.

## [Roadmap](ROADMAP.md)

## Architecture

```
Write → WAL → MemTable (SkipList)
                 ↓ flush
           SSTable (Level 0..6)
                 ↓ compaction
             SSTable (Level 1..6)
Read → MemTable → Immutable MemTable → SSTable (levels)
```

## Build

```bash
make build
```

## Test

```bash
make test
```

## Directory Structure

```
include/      Public headers
src/
  db/         MemTable, SkipList, InternalKey
  table/      Block, SSTable, TableBuilder, Filter, Iterator
  util/       Comparator, Coding, CRC32C, Arena, Env, Bloom
tests/        Unit tests
```
