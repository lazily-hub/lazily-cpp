# lazily-cpp — build, test, and verification targets.

.PHONY: all configure build test check fmt tidy clean conformance bench

BUILD_DIR ?= build

all: check

configure:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

build: configure
	cmake --build $(BUILD_DIR) --parallel

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

# Replay the shared lazily-spec conformance fixtures.
conformance: build
	ctest --test-dir $(BUILD_DIR) -R Conformance --output-on-failure

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
check: build test
	@echo "lazily-cpp: check OK"
