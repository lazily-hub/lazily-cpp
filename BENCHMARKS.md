# lazily-cpp Benchmark Results

Generated benchmark data for the [lazily-cpp](https://github.com/lazily-hub/lazily-cpp)
reactive primitives library.

## Benchmark Results

<!-- benchmark-results:start -->
Generated for package `lazily-cpp` version `0.1.0`.

Environment: `g++ (GCC) 16.1.1 20260625` on `x86_64-unknown-linux-gnu`, C++17 (`-O2` default).

Run command:

```bash
cmake -S . -B build -DLAZILY_BUILD_BENCHMARKS=ON
cmake --build build --target lazily_bench
./build/benches/lazily_bench
```

Refresh: re-run the bench binary and paste the table between the markers.

| Group | Case | Mean | Samples |
|---|---|---:|---:|
| cached_reads | context | 304.232 ns | 1000000 |
| cached_reads | thread_safe_context | 410.566 ns | 1000000 |
| cold_first_get | context | 2.520 us | 100000 |
| cold_first_get | thread_safe_context | 2.792 us | 100000 |
| dependency_fan_out | context / 32 | 9.544 us | 10000 |
| dependency_fan_out | context / 256 | 87.878 us | 10000 |
| dependency_fan_out | thread_safe_context / 32 | 8.954 us | 10000 |
| dependency_fan_out | thread_safe_context / 256 | 93.980 us | 10000 |
| set_cell_invalidation | high_fan_out / 512 | 124.655 us | 1000 |
| set_cell_invalidation | same_slot_contention / 1 | 860.606 ns | 10000 |
| set_cell_invalidation | same_slot_contention / 2 | 1.149 us | 10000 |
| set_cell_invalidation | same_slot_contention / 4 | 1.224 us | 10000 |
| set_cell_invalidation | same_slot_contention / 8 | 1.981 us | 10000 |
| set_cell_invalidation | same_slot_contention / 16 | 3.478 us | 10000 |
| set_cell_invalidation | independent_slot_contention / 1 | 640.655 ns | 10000 |
| set_cell_invalidation | independent_slot_contention / 2 | 1.294 us | 10000 |
| set_cell_invalidation | independent_slot_contention / 4 | 2.548 us | 10000 |
| set_cell_invalidation | independent_slot_contention / 8 | 5.311 us | 10000 |
| set_cell_invalidation | independent_slot_contention / 16 | 12.980 us | 10000 |
| memo_equality_suppression | context | 988.263 ns | 100000 |
| memo_equality_suppression | thread_safe_context | 911.639 ns | 100000 |
| effect_flushing | context | 2.387 us | 1000000 |
| effect_flushing | thread_safe_context | 2.384 us | 1000000 |
| batch_storms | context / 64 | 62.663 us | 100000 |
| batch_storms | thread_safe_context / 64 | 63.155 us | 100000 |
| thread_safe_contention | same_slot_write_read / 1 | 10.093 ms | 1 |
| thread_safe_contention | same_slot_write_read / 2 | 10.078 ms | 2 |
| thread_safe_contention | same_slot_write_read / 4 | 10.170 ms | 4 |
| thread_safe_contention | same_slot_write_read / 8 | 10.129 ms | 8 |
| thread_safe_contention | same_slot_write_read / 16 | 10.245 ms | 16 |
| scale | build / 100000 | 138.220 ms | 1 |
| scale | cold_full_recalc / 100000 | 131.894 ms | 1 |
| scale | full_recalc_invalidate_all / 100000 | 185.156 ms | 1 |
| scale | viewport_recalc / 100000 | 309.712 us | 1 |
| scale | build / 1000000 | 1.283 s | 1 |
| scale | cold_full_recalc / 1000000 | 1.092 s | 1 |
| scale | full_recalc_invalidate_all / 1000000 | 1.954 s | 1 |
| scale | viewport_recalc / 1000000 | 439.304 us | 1 |

<!-- benchmark-results:end -->

## Scale (≥1M cells) — `#lzscalebench`

The `scale` group is a rigorous benchmark over a spreadsheet-shaped graph of `N`
input cells + `N` formula slots (`formula[i] = input[i] + input[i-1]`). At
`N = 1,000,000` that is ~2,000,000 reactive nodes.

What the four cases show at `N = 1,000,000`:

- `build` constructs 2M nodes (~1.28 s, ~640 ns/node)
- `cold_full_recalc` computes every formula from cold (~1.09 s, ~546 ns/formula)
- `full_recalc_invalidate_all` re-edits every input and recomputes the whole
  sheet (~1.95 s)
- `viewport_recalc` edits one input and reads only a 1,000-cell viewport —
  **~439 us**, ~4,400× cheaper than a full recalc because the lazy pull-based
  model leaves off-viewport formulas dirty and never recomputes them (the
  property a viewport-rendered spreadsheet needs).

At `N = 100,000` (200,000 nodes), per-cell costs are lower (~690 ns build,
~660 ns recalc) because the smaller working set fits in cache. The viewport
recalc is **~310 us** — again independent of total graph size, confirming the
lazy-pull viewport property.

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
(~640 ns build, ~546 ns recompute), capacity scales linearly — the 1M-node
benchmark confirms the model extrapolates rather than degrading at spreadsheet
capacity.

### Per-node cost comparison with lazily-rs

| Metric | lazily-cpp (C++17, GCC 16) | lazily-rs (Rust, rustc 1.96) |
|---|---:|---:|
| cached read (Context) | 304 ns | 10.5 ns |
| cached read (ThreadSafeContext) | 411 ns | 67 ns |
| cold first get (Context) | 2.52 us | 93 ns |
| cold first get (ThreadSafeContext) | 2.79 us | 1.13 us |
| fan-out 256 (Context) | 88 us | 72 us |
| fan-out 256 (ThreadSafeContext) | 94 us | 219 us |
| set_cell high_fan_out 512 | 125 us | 145 us |
| memo equality suppression (Context) | 988 ns | 3.29 us |
| effect flushing (Context) | 2.39 us | 99 ns |
| batch storms 64 (Context) | 63 us | 3.85 us |
| scale build 1M | 1.28 s | 105 ms |
| scale viewport_recalc 1M | 439 us | 16 us |

**Honest read:** lazily-rs is faster on most micro-benchmarks — Rust's
zero-cost abstractions, monomorphized generics, and `SmallVec` inline edges
give it a per-node advantage. lazily-cpp is competitive on fan-out
invalidation (94 us vs 219 us at 256 — the `std::vector` edge storage is
cache-friendly) and memo equality suppression (988 ns vs 3.29 us). The scale
benchmark confirms lazily-cpp backs a full-capacity spreadsheet: 1M cells
build in ~1.3 s with viewport recalc at ~439 us, independent of sheet size.
Both share the same lazy-pull viewport property that makes viewport recalc
O(viewport) not O(sheet).

The gap is primarily from:
1. **Type erasure overhead** — `shared_ptr<void>` + `type_index` per node vs
   Rust's monomorphized `Rc<T>`
2. **`std::function` heap allocation** for compute closures vs Rust's
   monomorphized `Rc<dyn Fn>`
3. **No `SmallVec`** — `std::vector` always heap-allocates for edges, even
   the common 1-3 fan-out

These are addressable with a custom small-buffer-optimized function wrapper
and an `inline_vector<T, N>` edge storage, leaving the door open for future
optimization without API changes.

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
