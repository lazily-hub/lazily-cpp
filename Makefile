# lazily-cpp — build, test, and verification targets.

.PHONY: all configure build test test-interop-peer check fmt tidy clean \
	conformance conformance-coverage bench

BUILD_DIR ?= build

# Manifest of canonical lazily-spec fixtures each test binary actually opened.
# Every conformance binary APPENDS to it (tests/test_spec_fixture.hpp); the
# coverage guard audits the union. See scripts/check-conformance-coverage.sh.
CONFORMANCE_MANIFEST := $(abspath $(BUILD_DIR))/conformance-fixtures-loaded.txt

all: check

configure:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

build: configure
	cmake --build $(BUILD_DIR) --parallel

# The manifest is truncated first so it always describes THIS run — a stale file
# from a previous run would let the coverage guard pass on fixtures nobody read.
test: build
	rm -f $(CONFORMANCE_MANIFEST)
	LAZILY_CONFORMANCE_MANIFEST=$(CONFORMANCE_MANIFEST) \
	  ctest --test-dir $(BUILD_DIR) --output-on-failure

# Replay the shared lazily-spec conformance fixtures.
conformance: build
	rm -f $(CONFORMANCE_MANIFEST)
	LAZILY_CONFORMANCE_MANIFEST=$(CONFORMANCE_MANIFEST) \
	  ctest --test-dir $(BUILD_DIR) -R Conformance --output-on-failure

# Asserts the canonical corpus was actually replayed — not merely present on
# disk. Depends on a completed `test` run for the manifest.
conformance-coverage:
	./scripts/check-conformance-coverage.sh $(CONFORMANCE_MANIFEST)

test-interop-peer: build
	./$(BUILD_DIR)/lazily_interop_peer --self-check

bench: configure
	cmake --build $(BUILD_DIR) --target lazily_bench --parallel
	./$(BUILD_DIR)/benches/lazily_bench

# clang-format check (no-op if clang-format is unavailable).
fmt:
	@command -v clang-format >/dev/null 2>&1 && \
	  find include src tests -name '*.hpp' -o -name '*.cpp' | \
	  xargs clang-format -i || true

tidy:
	@command -v clang-tidy >/dev/null 2>&1 && \
	  cmake --build $(BUILD_DIR) --target tidy 2>/dev/null || true

clean:
	rm -rf $(BUILD_DIR)

# Full local gate — run before committing.
check: build test test-interop-peer conformance-coverage
	@echo "lazily-cpp: check OK"
