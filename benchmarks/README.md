# TalusDB benchmark

`db_bench` is adapted from LevelDB's database benchmark and runs against TalusDB's public API.
It does not require Google Benchmark.

## Workloads

- Writes: `fillseq`, `fillbatch`, `fillrandom`, `overwrite`, `fillsync`, `fill100K`
- Reads: `readseq`, `readreverse`, `readrandom`, `readmissing`, `readhot`,
  `readrandomsmall`
- Seeks: `seekrandom`, `seekordered`
- Deletes: `deleteseq`, `deleterandom`
- Concurrency: `readwhilewriting`
- Maintenance and inspection: `compact`, `stats`, `sstables`, `open`

Like LevelDB's original benchmark, `fillsync` runs `--num / 1000` operations, `fill100K` runs
`--num / 1000` operations with 100 KB values, and `readrandomsmall` runs `--reads / 1000`
operations.

The LevelDB-only `crc32c`, compression-codec, and heap-profiler microbenchmarks are not included.
They measure utility implementations rather than TalusDB database operations.

## Build and run

To build LevelDB from `../leveldb`, build TalusDB, and then run the same default workload against
LevelDB followed by TalusDB:

```bash
make benchmark
```

The default comparison uses 100,000 operations, separate database directories, and disables
compression on both implementations because TalusDB does not currently enable Snappy. It can be
customized without editing the Makefile:

```bash
make benchmark \
  BENCHMARK_ARGS='--benchmarks=fillseq,readrandom,stats --num=1000000 --reads=1000000' \
  LEVELDB_BENCHMARK_DB=/path/on/disk/leveldb-benchmark \
  TALUSDB_BENCHMARK_DB=/path/on/disk/talusdb-benchmark
```

Important flags include `--threads`, `--value_size`, `--histogram`, `--compression`,
`--write_buffer_size`, `--max_file_size`, `--block_size`, `--cache_size`, `--open_files`,
`--bloom_bits`, `--key_prefix`, `--reuse_logs`, and `--use_existing_db`.

Fresh write workloads recreate `--db` unless `--use_existing_db=1` is set. `/tmp` is commonly
RAM-backed; use a dedicated path on the target filesystem when measuring storage performance.
