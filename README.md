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

## Benchmark

The `db_bench` target is a TalusDB port of LevelDB's database benchmark. If LevelDB is checked
out at `../leveldb`, the following command builds both Release binaries and runs the same
workload against LevelDB first and TalusDB second:

```bash
make benchmark
```

Override `BENCHMARK_ARGS`, `LEVELDB_DIR`, `LEVELDB_BUILD_DIR`, `LEVELDB_BENCHMARK_DB`, or
`TALUSDB_BENCHMARK_DB` on the `make` command line when needed. Write benchmarks recreate their
separate database directories. Use paths on the same real filesystem instead of `/tmp` when
measuring storage performance.

## Directory Structure

```
include/      Public headers
src/
  db/         MemTable, SkipList, InternalKey
  table/      Block, SSTable, TableBuilder, Filter, Iterator
  util/       Comparator, Coding, CRC32C, Arena, Env, Bloom
tests/        Unit tests
benchmarks/   Performance benchmarks
```
