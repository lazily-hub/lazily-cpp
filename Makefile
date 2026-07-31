# lazily-cpp — build, test, and verification targets.

.PHONY: all configure build test test-interop-peer check fmt fmt-fix tidy clean \
	conformance conformance-coverage bench ci-reach

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

# CI-reachability guard (#lzcheckcireachguard). Fails when a target above runs a
# gate no CI workflow step reaches — the drift that hid #lzinteroppeerci in every
# binding for months. It guards itself: `ci-reach` is in `check`, so CI has to run
# it too or this target reports itself missing.
ci-reach:
	./scripts/check-ci-reach.sh

test-interop-peer: build
	./$(BUILD_DIR)/lazily_interop_peer --self-check

bench: configure
	cmake --build $(BUILD_DIR) --target lazily_bench --parallel
	./$(BUILD_DIR)/benches/lazily_bench

# The formatting GATE (#lazilycppfmt).
#
# What this replaced could not fail. It ran `clang-format -i`, which REWRITES the
# tree it is judging and then exits 0 whatever it found; it ended in `|| true`, so
# even a formatter that errored reported success; and with no .clang-format in the
# repo it enforced whatever LLVM defaults the local binary happened to carry. Three
# independent reasons a green `make fmt` meant nothing.
#
# The version is pinned as well as the style, because they drift separately: the
# checked-in .clang-format pins WHAT the style is, CLANG_FORMAT_VERSION pins the
# implementation that interprets it. `uvx` fetches that exact version, so the gate
# gives the same verdict on a laptop and on a runner regardless of what the system
# clang-format is. Missing uv is a hard error rather than a skip — a gate that
# quietly does nothing when a tool is absent is the same defect in a smaller coat.
CLANG_FORMAT_VERSION ?= 18.1.8
CLANG_FORMAT ?= uvx clang-format@$(CLANG_FORMAT_VERSION)
FORMAT_SOURCES := $(shell find include src tests benches -type f \
	\( -name '*.hpp' -o -name '*.cpp' \) 2>/dev/null)

fmt:
	@command -v uvx >/dev/null 2>&1 || { \
	  echo "make fmt: uvx not found — it pins clang-format $(CLANG_FORMAT_VERSION);"; \
	  echo "  install uv (https://docs.astral.sh/uv/) or override CLANG_FORMAT="; \
	  exit 1; }
	@echo "clang-format $(CLANG_FORMAT_VERSION): checking $(words $(FORMAT_SOURCES)) file(s)"
	@$(CLANG_FORMAT) --dry-run -Werror $(FORMAT_SOURCES)

# The repairing form. Deliberately NOT in `check`.
fmt-fix:
	@$(CLANG_FORMAT) -i $(FORMAT_SOURCES)

tidy:
	@command -v clang-tidy >/dev/null 2>&1 && \
	  cmake --build $(BUILD_DIR) --target tidy 2>/dev/null || true

clean:
	rm -rf $(BUILD_DIR)

# Full local gate — run before committing.
check: fmt build test test-interop-peer conformance-coverage ci-reach
	@echo "lazily-cpp: check OK"
