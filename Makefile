SHELL := bash

LEVELDB_DIR ?= ../leveldb
LEVELDB_BUILD_DIR ?= build-release/leveldb
BENCHMARK_ARGS ?= --benchmarks=fillseq,fillrandom,readseq,readrandom,stats --num=100000 --reads=100000 --compression=0
LEVELDB_BENCHMARK_DB ?= /tmp/leveldb-benchmark
TALUSDB_BENCHMARK_DB ?= /tmp/talusdb-benchmark

.PHONY: help configure build benchmark test ci-test fmt fmt-check check

help:
	@echo "Available targets:"
	@echo "  make configure  - Configure CMake with the dev preset"
	@echo "  make build      - Build with the dev preset"
	@echo "  make benchmark  - Compare LevelDB and TalusDB in Release mode"
	@echo "  make test       - Run gtest binaries directly"
	@echo "  make ci-test    - Run tests via CTest with the dev preset"
	@echo "  make fmt        - Format C/C++ sources with clang-format"
	@echo "  make fmt-check  - Verify C/C++ formatting with clang-format"
	@echo "  make check      - Run fmt-check, build, and ci-test"

configure:
	cmake --preset dev
	cmake -E rm -f compile_commands.json
	cmake -E create_symlink build/compile_commands.json compile_commands.json

build: configure
	cmake --build --preset dev

benchmark:
	@test -f "$(LEVELDB_DIR)/CMakeLists.txt" || { \
		echo "LevelDB not found at $(LEVELDB_DIR); set LEVELDB_DIR=/path/to/leveldb" >&2; \
		exit 1; \
	}
	cmake -S "$(LEVELDB_DIR)" -B "$(LEVELDB_BUILD_DIR)" \
		-DCMAKE_BUILD_TYPE=Release \
		-DLEVELDB_BUILD_BENCHMARKS=ON \
		-DLEVELDB_BUILD_TESTS=OFF
	cmake --build "$(LEVELDB_BUILD_DIR)" --target db_bench --parallel
	cmake -S . -B build-release \
		-DCMAKE_BUILD_TYPE=Release \
		-DDB_BUILD_BENCHMARKS=ON \
		-DBUILD_TESTING=OFF
	cmake --build build-release --target db_bench --parallel
	@printf '\n\n==> LevelDB (baseline)\n\n'
	"$(LEVELDB_BUILD_DIR)/db_bench" $(BENCHMARK_ARGS) --db="$(LEVELDB_BENCHMARK_DB)"
	@printf '\n\n==> TalusDB\n\n'
	./build-release/db_bench $(BENCHMARK_ARGS) --db="$(TALUSDB_BENCHMARK_DB)"
	@printf '\n\n'

test: build
	@set -euo pipefail; \
	shopt -s nullglob; \
	tests=(build/*_test); \
	if [ "$${#tests[@]}" -eq 0 ]; then \
	  echo "No gtest binaries found under build/." >&2; \
	  exit 1; \
	fi; \
	IFS=$$'\n' tests=($$(printf '%s\n' "$${tests[@]}" | sort)); \
	unset IFS; \
	for test_bin in "$${tests[@]}"; do \
	  echo "==> Running $$test_bin"; \
	  "$$test_bin" --gtest_color=yes; \
	done

ci-test: build
	ctest --preset dev

fmt:
	@command -v clang-format >/dev/null 2>&1 || { echo "clang-format not found in PATH." >&2; exit 1; }
	@set -euo pipefail; \
	files=(); \
	for dir in include src tests examples benchmarks tools; do \
	  [ -d "$$dir" ] || continue; \
	  while IFS= read -r -d '' file; do \
	    files+=("$$file"); \
	  done < <(find "$$dir" -type f \( \
	    -name '*.c' -o \
	    -name '*.cc' -o \
	    -name '*.cpp' -o \
	    -name '*.cxx' -o \
	    -name '*.h' -o \
	    -name '*.hh' -o \
	    -name '*.hpp' \
	  \) -print0); \
	done; \
	if [ "$${#files[@]}" -eq 0 ]; then \
	  echo "No C/C++ source files found to format."; \
	  exit 0; \
	fi; \
	clang-format -i "$${files[@]}"; \
	echo "Formatted $${#files[@]} file(s)."

fmt-check:
	@command -v clang-format >/dev/null 2>&1 || { echo "clang-format not found in PATH." >&2; exit 1; }
	@set -euo pipefail; \
	files=(); \
	for dir in include src tests examples benchmarks tools; do \
	  [ -d "$$dir" ] || continue; \
	  while IFS= read -r -d '' file; do \
	    files+=("$$file"); \
	  done < <(find "$$dir" -type f \( \
	    -name '*.c' -o \
	    -name '*.cc' -o \
	    -name '*.cpp' -o \
	    -name '*.cxx' -o \
	    -name '*.h' -o \
	    -name '*.hh' -o \
	    -name '*.hpp' \
	  \) -print0); \
	done; \
	if [ "$${#files[@]}" -eq 0 ]; then \
	  echo "No C/C++ source files found to check."; \
	  exit 0; \
	fi; \
	clang-format --dry-run --Werror "$${files[@]}"; \
	echo "Formatting check passed for $${#files[@]} file(s)."

check:
	$(MAKE) fmt-check
	$(MAKE) build
	$(MAKE) ci-test
