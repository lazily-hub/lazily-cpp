# Contributing to lazily-cpp

## Build and test

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Run the repository's complete local gate before submitting a change:

```bash
make check
```

## Benchmarks

```bash
cmake -S . -B build -DLAZILY_BUILD_BENCHMARKS=ON
cmake --build build --target lazily_bench
./build/benches/lazily_bench
```

See [BENCHMARKS.md](BENCHMARKS.md) for full results, scale benchmarks (≥1M
cells), and a cross-language comparison with lazily-rs and lazily-zig.

Consumer-facing CMake and wasm integration instructions remain in the
[README](README.md#build-and-integration).
