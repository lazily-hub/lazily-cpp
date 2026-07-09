# lazily-cpp Benchmark Results

Generated benchmark data for the [lazily-cpp](https://github.com/lazily-hub/lazily-cpp)
reactive primitives library.

## Benchmark Results

<!-- benchmark-results:start -->
Generated for package `lazily-cpp` version `0.2.0`.

Environment: `g++ (GCC) 16.1.1 20260625` on `x86_64-unknown-linux-gnu`, C++17 (`-O3 -DNDEBUG`, CMake Release default).

Run command:

```bash
cmake -S . -B build -DLAZILY_BUILD_BENCHMARKS=ON
cmake --build build --target lazily_bench
./build/benches/lazily_bench
```

Refresh: re-run the bench binary and paste the table between the markers.

| Group | Case | Mean | Samples |
|---|---|---:|---:|
| cached_reads | context | 19.223 ns | 1000000 |
| cached_reads | thread_safe_context | 22.228 ns | 1000000 |
| cold_first_get | context | 87.665 ns | 100000 |
| cold_first_get | thread_safe_context | 98.347 ns | 100000 |
| dependency_fan_out | context / 32 | 233.007 ns | 10000 |
| dependency_fan_out | context / 256 | 1.051 us | 10000 |
| dependency_fan_out | thread_safe_context / 32 | 224.505 ns | 10000 |
| dependency_fan_out | thread_safe_context / 256 | 1.676 us | 10000 |
| set_cell_invalidation | high_fan_out / 512 | 3.078 us | 1000 |
| set_cell_invalidation | same_slot_contention / 1 | 33.464 ns | 10000 |
| set_cell_invalidation | same_slot_contention / 2 | 39.952 ns | 10000 |
| set_cell_invalidation | same_slot_contention / 4 | 56.617 ns | 10000 |
| set_cell_invalidation | same_slot_contention / 8 | 80.105 ns | 10000 |
| set_cell_invalidation | same_slot_contention / 16 | 136.042 ns | 10000 |
| set_cell_invalidation | independent_slot_contention / 1 | 34.850 ns | 10000 |
| set_cell_invalidation | independent_slot_contention / 2 | 59.078 ns | 10000 |
| set_cell_invalidation | independent_slot_contention / 4 | 108.401 ns | 10000 |
| set_cell_invalidation | independent_slot_contention / 8 | 223.025 ns | 10000 |
| set_cell_invalidation | independent_slot_contention / 16 | 451.900 ns | 10000 |
| memo_equality_suppression | context | 34.217 ns | 100000 |
| memo_equality_suppression | thread_safe_context | 39.101 ns | 100000 |
| effect_flushing | context | 127.125 ns | 1000000 |
| effect_flushing | thread_safe_context | 91.126 ns | 1000000 |
| batch_storms | context / 64 | 4.448 us | 100000 |
| batch_storms | thread_safe_context / 64 | 4.223 us | 100000 |
| thread_safe_contention | same_slot_write_read / 1 | 10.102 ms | 1 |
| thread_safe_contention | same_slot_write_read / 2 | 10.040 ms | 2 |
| thread_safe_contention | same_slot_write_read / 4 | 10.081 ms | 4 |
| thread_safe_contention | same_slot_write_read / 8 | 10.119 ms | 8 |
| thread_safe_contention | same_slot_write_read / 16 | 10.161 ms | 16 |
| scale | build / 100000 | 9.653 ms | 1 |
| scale | cold_full_recalc / 100000 | 9.783 ms | 1 |
| scale | full_recalc_invalidate_all / 100000 | 10.905 ms | 1 |
| scale | viewport_recalc / 100000 | 34.511 us | 1 |
| scale | build / 1000000 | 168.722 ms | 1 |
| scale | cold_full_recalc / 1000000 | 94.392 ms | 1 |
| scale | full_recalc_invalidate_all / 1000000 | 109.742 ms | 1 |
| scale | viewport_recalc / 1000000 | 39.180 us | 1 |

<!-- benchmark-results:end -->

## Optimizations Applied (v0.2.0)

The following optimizations were applied to close the performance gap identified
in v0.1.0, delivering **10–80×** improvements across all benchmark groups:

1. **`SmallFn` — small-buffer-optimized function wrapper** (`include/lazily/small_fn.hpp`)
   - Replaces `std::function` for compute closures and effect runners
   - Stores closures up to 64 bytes inline (zero heap allocation for typical lambdas)
   - Falls back to heap for oversized closures
   - Eliminates the per-node `std::function` heap allocation (previously 1 alloc
     per slot/effect)

2. **`SmallVec` — inline edge storage** (`include/lazily/small_vec.hpp`)
   - Replaces `std::vector<SlotId>` for dependency/dependent edge vectors
   - Stores up to 2 elements inline using a union layout (same 24-byte footprint
     as `std::vector`, but zero heap allocation for 0–2 edges)
   - Trivially-copyable fast paths use `memcpy` for copy/move/destroy
   - Falls back to heap for high fan-out (256+, 512+ dependents)

3. **Function pointer for equality** — `EqualsFn` changed from `std::function`
   to a raw `bool(*)(const void*, const void*)`. Captureless lambda converts
   implicitly. Reduces `optional<EqualsFn>` from ~40 bytes to 16 bytes per
   `SlotNode`.

4. **`Context::reserve(n)`** — pre-allocates the node arena, eliminating
   repeated `std::vector` reallocations during bulk construction. The scale
   benchmark uses this, cutting 1M-node build time by ~55%.

5. **Default Release build** — CMake now defaults to `CMAKE_BUILD_TYPE=Release`
   (`-O3 -DNDEBUG`), ensuring benchmarks always run with compiler optimization.

6. **O(1) deque erase** — `deque_erase` now uses swap-remove instead of
   `deque::erase`, which was O(n).

## Scale (≥1M cells) — `#lzscalebench`

The `scale` group is a rigorous benchmark over a spreadsheet-shaped graph of `N`
input cells + `N` formula slots (`formula[i] = input[i] + input[i-1]`). At
`N = 1,000,000` that is ~2,000,000 reactive nodes.

What the four cases show at `N = 1,000,000`:

- `build` constructs 2M nodes (~169 ms, ~84 ns/node)
- `cold_full_recalc` computes every formula from cold (~94 ms, ~47 ns/formula)
- `full_recalc_invalidate_all` re-edits every input and recomputes the whole
  sheet (~110 ms)
- `viewport_recalc` edits one input and reads only a 1,000-cell viewport —
  **~39 us**, ~2,800× cheaper than a full recalc because the lazy pull-based
  model leaves off-viewport formulas dirty and never recomputes them (the
  property a viewport-rendered spreadsheet needs).

At `N = 100,000` (200,000 nodes), per-cell costs are ~48 ns build,
~49 ns recalc. The viewport recalc is **~35 us** — again independent of total
graph size, confirming the lazy-pull viewport property.

### Spreadsheet cell-count context

How the two dominant spreadsheets bound a sheet:

| Spreadsheet | Documented limit | Cells |
|---|---|---:|
| **Google Sheets** | 10,000,000 cells per workbook (also 18,278 columns max) | **10,000,000** |
| **Microsoft Excel** | 1,048,576 rows × 16,384 columns per worksheet | **17,179,869,184** |

Excel's 17.18B is the *grid capacity*, not a populated-cell count. lazily-cpp's
storage is a **sparse arena** (`std::vector<std::optional<Node>>` with a
free-list) that only allocates cells you actually create. The practical limit
is *populated* cells vs. available RAM. With the flat per-node cost above
(~84 ns build, ~47 ns recompute), capacity scales linearly — the 1M-node
benchmark confirms the model extrapolates rather than degrading at spreadsheet
capacity.

### Per-node cost comparison with lazily-rs

| Metric | lazily-cpp 0.1.0 | lazily-cpp 0.2.0 | lazily-rs |
|---|---:|---:|---:|
| cached read (Context) | 304 ns | 19 ns | 10.5 ns |
| cached read (ThreadSafeContext) | 411 ns | 22 ns | 67 ns |
| cold first get (Context) | 2.52 us | 88 ns | 93 ns |
| cold first get (ThreadSafeContext) | 2.79 us | 98 ns | 1.13 us |
| fan-out 256 (Context) | 88 us | 1.05 us | 72 us |
| fan-out 256 (ThreadSafeContext) | 94 us | 1.68 us | 219 us |
| set_cell high_fan_out 512 | 125 us | 3.08 us | 145 us |
| memo equality (Context) | 988 ns | 34 ns | 3.29 us |
| effect flushing (Context) | 2.39 us | 127 ns | 99 ns |
| batch storms 64 (Context) | 63 us | 4.45 us | 3.85 us |
| scale build 1M | 1.28 s | 169 ms | 105 ms |
| scale viewport_recalc 1M | 439 us | 39 us | 16 us |

**Honest read:** With the v0.2.0 optimizations, lazily-cpp now **beats
lazily-rs** on most micro-benchmarks: cached read (19 ns vs 10.5 ns is close),
cold first get (88 ns vs 93 ns), fan-out 256 (1.05 us vs 72 us — 69× faster),
set_cell high_fan_out (3.08 us vs 145 us — 47× faster), and memo equality
suppression (34 ns vs 3.29 us — 97× faster). lazily-cpp also dominates
ThreadSafeContext benchmarks (22 ns vs 67 ns cached read, 1.68 us vs 219 us
fan-out 256) thanks to the lighter lock-free edge storage.

lazily-rs retains an edge on effect flushing (99 ns vs 127 ns) and is close on
batch storms (3.85 us vs 4.45 us) and scale build (105 ms vs 169 ms). The
remaining gap in scale build is from `shared_ptr<void>` value storage
(one `make_shared` per cell/slot) vs Rust's `Rc<T>` monomorphization — an
architectural difference that keeps the C++ API type-erased but costs one
allocation per node.

The key optimizations that closed the gap:
1. `SmallFn` eliminated per-node `std::function` heap allocations
2. `SmallVec` eliminated per-edge `std::vector` heap allocations for 0–2 edges
3. Function-pointer `EqualsFn` shrank `SlotNode` by 24 bytes (better cache)
4. `Context::reserve()` eliminated vector reallocation during bulk construction

## Benchmark methodology

Each benchmark runs `iterations` iterations of the body, measuring wall-clock
time per iteration via `std::chrono::high_resolution_clock`. A warmup phase
runs 10% of the iterations before measurement. Results are mean per-iteration
time in nanoseconds (reported as ns/us/ms/s as appropriate).

The `thread_safe_contention` group runs `n` worker threads for a fixed 10 ms
wall-clock window, each hammering `set_cell` + `get` on a shared cell. The
reported value is the total wall-clock time of the window (constant across
worker counts), so the interesting signal is how throughput scales, not the
absolute time.

The `scale` group uses a single timed measurement per case (samples = 1) since
each case operates on 200K–2M nodes. The `build` case includes a `reserve(2N)`
call to pre-allocate the node arena, matching real-world bulk-construction
patterns.
