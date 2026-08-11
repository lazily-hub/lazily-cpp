# lazily-cpp — build, test, and verification targets.

# make invokes /bin/sh, which is dash on Ubuntu runners and bash on Arch. The
# wasm recipes below need `source` (not POSIX) and `pipefail` — without the
# latter, a recipe that pipes a failing build through `tee` reports TEE's exit
# code and a red run reads as green. That false-green is exactly what the
# fixture manifests exist to prevent one layer down, so pin the shell rather
# than rely on whatever the host provides.
SHELL := /bin/bash
.SHELLFLAGS := -eu -o pipefail -c

.PHONY: all configure build test test-interop-peer check fmt fmt-fix tidy clean \
conformance conformance-coverage assertion-ordering-check bench ci-reach \
wasm wasm-core wasm-threaded wasm-matrix wasm-bench wasm-corpus-column

BUILD_DIR ?= build

# The sibling lazily-spec checkout. Every gate below that reads it treats its
# absence as a hard FAILURE, never a skip (#lzcppsiblingskipvsfail): measured on
# this tree, a corpus-less run executes 25 of 62 ctest targets and skips 37,
# replays 0 of 139 canonical fixtures and 0 of 149 scenarios, and leaves 654 of
# the suite's 927 assertion sites unrun. `make check` is the pre-commit gate, so
# a green there over that state is a false green — the one that let 9b0ff08 pass
# locally and land red on CI. Named as a variable so the refusal itself is
# testable: CI points it at a non-existent path and asserts the gate reddens.
LAZILY_SPEC_DIR ?= ../lazily-spec

# Emscripten/wasm32 (`#lzcppwasm`). Separate build dirs per tier so a wasm
# configure never clobbers the native one and `make check` stays unaffected.
WASM_CORE_DIR ?= build_wasm_core
WASM_THREADED_DIR ?= build_wasm_threaded
EMSDK_ENV ?= $(HOME)/emsdk/emsdk_env.sh

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

# The corpus-derivable half of the WASM.md matrix audit (#lzcppwasmguardlocal).
# Needs no emsdk, no wasm build, and no tier manifest, so it runs here rather
# than only in the wasm job: the Corpus column and the family row set are
# functions of ../lazily-spec/conformance alone. 9b0ff08 passed `make check` and
# both native CI jobs and still landed red on the matrix; this is that failure
# made reachable locally. The per-tier columns stay in `make wasm`.
wasm-corpus-column:
	./scripts/check-wasm-corpus-column.sh

# Full local gate — run before committing.
# Assertion observation ordering (#lzassertordering). The checker itself lives in
# the sibling, so this gate has always been fail-closed — but only by accident,
# via python3's "can't open file" errno. State it (#lzcppsiblingskipvsfail): an
# absent sibling gets the same refusal wording as every other corpus guard rather
# than an interpreter traceback that reads like a broken toolchain.
assertion-ordering-check:
	@test -f $(LAZILY_SPEC_DIR)/scripts/check-assertion-ordering.py || { \
	  echo "ERROR: canonical conformance corpus not found at '$(LAZILY_SPEC_DIR)' (sibling checkout)."; \
	  echo "       git clone https://github.com/lazily-hub/lazily-spec.git ../lazily-spec"; \
	  echo "       (or override LAZILY_SPEC_DIR=/path/to/lazily-spec)"; \
	  echo "       This is a hard failure, not a skip: the ordering checker lives in"; \
	  echo "       the sibling, so without it this gate verifies nothing."; \
	  exit 1; }
	python3 $(LAZILY_SPEC_DIR)/scripts/check-assertion-ordering.py --binding cpp --root .

check: fmt build test test-interop-peer conformance-coverage ci-reach assertion-ordering-check wasm-corpus-column
	@echo "lazily-cpp: check OK"

# ── Emscripten/wasm32 (`#lzcppwasm`) ───────────────────────────────────────
#
# Each tier configures with emcmake, builds, and REPLAYS the conformance suite
# under Node against the same ../lazily-spec fixtures the native suite uses --
# NODERAWFS lets the unmodified loader read them. The manifest is truncated
# first so it always describes THIS run; a stale file would let the matrix
# report fixtures nobody read.
#
# Missing emsdk is a hard error, not a skip. A wasm gate that quietly does
# nothing when the toolchain is absent reports OK over an empty matrix.
define wasm_tier
	@if [ ! -f "$(EMSDK_ENV)" ]; then \
	  echo "make $@: emsdk not found at $(EMSDK_ENV)"; \
	  echo "  git clone https://github.com/emscripten-core/emsdk.git ~/emsdk"; \
	  echo "  cd ~/emsdk && ./emsdk install latest && ./emsdk activate latest"; \
	  echo "  (or override EMSDK_ENV=/path/to/emsdk_env.sh)"; \
	  exit 1; \
	fi
	source $(EMSDK_ENV) >/dev/null && \
	  emcmake cmake -S . -B $(2) -DLAZILY_WASM_TIER=$(1) -DCMAKE_BUILD_TYPE=Release && \
	  cmake --build $(2) --parallel && \
	  rm -f $(abspath $(2))/conformance-fixtures-loaded.txt && \
	  LAZILY_CONFORMANCE_MANIFEST=$(abspath $(2))/conformance-fixtures-loaded.txt \
	    ctest --test-dir $(2) --output-on-failure
endef

wasm-core:
	$(call wasm_tier,core,$(WASM_CORE_DIR))

wasm-threaded:
	$(call wasm_tier,threaded,$(WASM_THREADED_DIR))

# Regenerates the WASM.md matrix from the runtime manifests. Requires both
# tiers to have run; the guard fails on a missing manifest rather than emitting
# a table full of zeros.
wasm-matrix:
	./scripts/check-wasm-tiers.sh --write

# The full wasm gate: both tiers replayed, then the matrix audited against what
# actually ran. This is what CI runs.
wasm: wasm-core wasm-threaded
	./scripts/check-wasm-tiers.sh

# Wasm benchmarks. Results land in a SEPARATE section of BENCHMARKS.md with
# their own environment line -- native -O3 and wasm numbers are not comparable
# and must not share a table.
wasm-bench:
	@if [ ! -f "$(EMSDK_ENV)" ]; then echo "make wasm-bench: emsdk not found at $(EMSDK_ENV)"; exit 1; fi
	source $(EMSDK_ENV) >/dev/null && \
	  emcmake cmake -S . -B $(WASM_THREADED_DIR) -DLAZILY_WASM_TIER=threaded \
	    -DLAZILY_BUILD_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release && \
	  cmake --build $(WASM_THREADED_DIR) --target lazily_bench --parallel && \
	  node $(WASM_THREADED_DIR)/benches/lazily_bench.js
