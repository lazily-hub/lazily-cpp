# lazily-cpp Benchmark Results

Generated benchmark data for the [lazily-cpp](https://github.com/lazily-hub/lazily-cpp)
reactive primitives library.

## Benchmark Results

<!-- benchmark-results:start -->
Generated for package `lazily-cpp` version `0.5.0`.

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
| cached_reads | context | 23.294 ns | 1000000 |
| cached_reads | thread_safe_context | 21.745 ns | 1000000 |
| cold_first_get | context | 97.398 ns | 100000 |
| cold_first_get | thread_safe_context | 107.351 ns | 100000 |
| dependency_fan_out | context / 32 | 153.386 ns | 10000 |
| dependency_fan_out | context / 256 | 1.123 us | 10000 |
| dependency_fan_out | thread_safe_context / 32 | 223.948 ns | 10000 |
| dependency_fan_out | thread_safe_context / 256 | 1.680 us | 10000 |
| set_cell_invalidation | high_fan_out / 512 | 3.262 us | 1000 |
| set_cell_invalidation | same_slot_contention / 1 | 36.354 ns | 10000 |
| set_cell_invalidation | same_slot_contention / 2 | 39.784 ns | 10000 |
| set_cell_invalidation | same_slot_contention / 4 | 61.536 ns | 10000 |
| set_cell_invalidation | same_slot_contention / 8 | 88.612 ns | 10000 |
| set_cell_invalidation | same_slot_contention / 16 | 153.572 ns | 10000 |
| set_cell_invalidation | independent_slot_contention / 1 | 34.439 ns | 10000 |
| set_cell_invalidation | independent_slot_contention / 2 | 56.048 ns | 10000 |
| set_cell_invalidation | independent_slot_contention / 4 | 98.461 ns | 10000 |
| set_cell_invalidation | independent_slot_contention / 8 | 201.007 ns | 10000 |
| set_cell_invalidation | independent_slot_contention / 16 | 372.359 ns | 10000 |
| memo_equality_suppression | context | 34.064 ns | 100000 |
| memo_equality_suppression | thread_safe_context | 37.924 ns | 100000 |
| effect_flushing | context | 87.446 ns | 1000000 |
| effect_flushing | thread_safe_context | 91.721 ns | 1000000 |
| batch_storms | context / 64 | 4.215 us | 100000 |
| batch_storms | thread_safe_context / 64 | 3.634 us | 100000 |
| thread_safe_concurrency | contention/recursive @ 1 | 19.300 Mops/s | 1 |
| thread_safe_concurrency | contention/recursive @ 16 | 1.265 Mops/s | 16 |
| thread_safe_concurrency | contention/rw @ 1 | 12.600 Mops/s | 1 |
| thread_safe_concurrency | contention/rw @ 16 | 1.980 Mops/s | 16 |
| thread_safe_concurrency | contention/scalable @ 1 | 7.600 Mops/s | 1 |
| thread_safe_concurrency | contention/scalable @ 16 | 0.955 Mops/s | 16 |
| thread_safe_concurrency | read_scaling/recursive @ 1 | 45.700 Mops/s | 1 |
| thread_safe_concurrency | read_scaling/recursive @ 16 | 7.800 Mops/s | 16 |
| thread_safe_concurrency | read_scaling/rw @ 1 | 54.300 Mops/s | 1 |
| thread_safe_concurrency | read_scaling/rw @ 16 | 12.500 Mops/s | 16 |
| thread_safe_concurrency | read_scaling/scalable @ 1 | 58.000 Mops/s | 1 |
| thread_safe_concurrency | read_scaling/scalable @ 16 | 925.000 Mops/s | 16 |
| scale | build / 100000 | 10.800 ms | 1 |
| scale | cold_full_recalc / 100000 | 10.300 ms | 1 |
| scale | full_recalc_invalidate_all / 100000 | 8.250 ms | 1 |
| scale | viewport_recalc / 100000 | 40.400 us | 1 |
| scale | build / 1000000 | 130.000 ms | 1 |
| scale | cold_full_recalc / 1000000 | 111.000 ms | 1 |
| scale | full_recalc_invalidate_all / 1000000 | 117.000 ms | 1 |
| scale | viewport_recalc / 1000000 | 35.100 us | 1 |
| scale | build / 2000000 | 234.000 ms | 1 |
| scale | cold_full_recalc / 2000000 | 181.000 ms | 1 |
| scale | full_recalc_invalidate_all / 2000000 | 177.000 ms | 1 |
| scale | viewport_recalc / 2000000 | 27.700 us | 1 |
| scale | build / 10000000 | 1.383 s | 1 |
| scale | cold_full_recalc / 10000000 | 827.363 ms | 1 |
| scale | full_recalc_invalidate_all / 10000000 | 778.251 ms | 1 |
| scale | viewport_recalc / 10000000 | 43.800 us | 1 |

<!-- benchmark-results:end -->

> **Micro-benchmark** rows are a single stable run (high iteration counts,
> low variance). **Scale** rows are the median of 3 runs — single-sample cases
> carry ±15% run-to-run variance, so small deltas are not meaningful. The robust
> v0.2.0 → v0.3.0 scale changes (`cold_full_recalc` ~28% faster, `full_recalc`
> ~2× faster at 10M, from non-atomic compute closures) are unchanged in v0.5.0.
> **`thread_safe_concurrency`** is the headline — three lock policies now. v0.5.0
> adds the **`ScalableThreadSafeContext`**: cached reads scale **near-linearly**
> (~925 Mops/s at 16 threads, vs the RW policy's 12.5 Mops/s plateau). See
> [Thread-safe concurrency](#thread-safe-concurrency--read-scaling).

## Optimizations Applied (v0.5.0)

v0.5.0 ships **optimization A2 — reader-scalable locking**, as a third opt-in
policy. The default (`ThreadSafeContext`) is unchanged.

1. **`ScalableThreadSafeContext` (opt-in) — `ScalableRwLock`.** `std::shared_mutex`
   serializes shared acquires on a single internal atomic, so concurrent readers
   contend on one cache line and plateau at ~12.5 Mops/s. `ScalableRwLock` gives
   each reader thread its own cache line (a per-thread "active" counter in a
   128-slot pool); readers only touch their own line plus a shared read of a
   writer-waiting flag — so reads scale **near-linearly** (~925 Mops/s at 16
   threads, ~73× the RW plateau). Mutual exclusion is a seq_cst two-phase
   handshake (reader marks active before checking the flag; writer raises the
   flag before draining slots) — validated clean under ThreadSanitizer.
2. **Honest trade-off**: writers scan all 128 slots on every write, so write
   throughput is the lowest of the three policies (7.6 Mops/s @N=1 vs recursive
   19.3). Reader-preferring (writers can starve under a steady reader stream).
   Choose for **read-heavy** high-concurrency loads; use the recursive default
   otherwise.
3. **Why a scalable lock, not lock-free?** True lock-free cached reads need
   atomic-refcounted values + hazard/epoch reclamation, which requires deep
   retrofitting of the type-erased engine's value-publish/recompute paths and
   introduces memory-safety risk disproportionate to the gain — the scalable
   lock achieves the same near-linear read scaling with zero reclamation risk
   and no regression to the non-atomic Rc value path.
4. **Tests**: `test_concurrency` (all three policies under concurrent RMW +
   read consistency) joins the suite; `tests/tsan_stress.cpp` (heavier, built
   behind `LAZILY_BUILD_TSAN_TEST=ON`) validates race-freedom under TSan.

### How to choose (three policies)

| Workload | Use |
|---|---|
| Single-threaded, low concurrency, or write-heavy | `ThreadSafeContext` (default, recursive) |
| Read-heavy concurrent (simple) | `RwThreadSafeContext` (shared_mutex, ~2.6× read scaling) |
| Read-heavy high-concurrency (best read scaling) | `ScalableThreadSafeContext` (~73× read scaling) |

## Optimizations Applied (v0.4.0)

v0.4.0 ships **optimization A — read/write concurrency**, made **opt-in** so the
recursive default is unchanged (zero regression for existing users).

1. **`RwThreadSafeContext` (opt-in) — `shared_mutex` read/write locking.** Cached
   reads (`get`/`get_cell` on a clean value) run under a **shared** lock, so
   concurrent readers scale across cores (~2.6× at 16 threads on this host).
   Mutations (`set_cell`, `batch`, recomputation) run under an **exclusive** lock.
   The exclusive acquire is heavier than `recursive_mutex`, so write-heavy
   single-thread paths regress ~2× — hence opt-in, not default.
2. **Lock-policy template** (`BasicThreadSafeContext<Policy>`) with
   `RecursiveLockPolicy` (default) and `RwLockPolicy`. `ThreadSafeContext` keeps
   the exact v0.3.0 semantics/perf; `RwThreadSafeContext` is the read-scaling
   variant. Choose by workload.
3. **Re-entrancy preserved** via an owner token: the RW policy is not recursive,
   so a wrapper call from inside a recompute/effect cascade (the thread already
   holding the exclusive lock) bypasses locking. The recursive policy relies on
   native recursion as before.
4. **Read-only cache-peek API** on `Context`: `try_get_cached<T>()` /
   `peek_cell<T>()` — non-mutating, safe under a shared lock (copy the boxed T
   through an immutably-published pointer). Enables the shared read fast path.
5. **New tests**: concurrent shared-read consistency (8 threads × 10k reads, zero
   torn reads) and re-entrant-callback (would deadlock under a naive
   shared_mutex).

### How to choose

| Workload | Use |
|---|---|
| Single-threaded, or low concurrency, or write-heavy | `ThreadSafeContext` (default, recursive) |
| Many concurrent readers, occasional writes (UI/editor/CRDT reads) | `RwThreadSafeContext` (RW) |

## Optimizations Applied (v0.3.0)

v0.3.0 targets the **high-load / real-world** path rather than micro-benchmark
gaming, and ships the first honest contention baseline.

1. **Real contention-throughput benchmark** (`benches/bench_main.cpp`) — the
   v0.2.0 `thread_safe_contention` group reported the *fixed 10 ms window
   duration* (flat ~10 ms for 1→16 threads) and never counted completed ops, so
   it could not show whether throughput scaled or collapsed. v0.3.0 counts
   completed ops per worker and reports **total throughput (Mops/s)** and
   **per-op latency (ns/op)**. It reveals the load cliff (see
   [Thread-safe contention](#thread-safe-contention--the-load-cliff)) that
   motivates the read/write-lock work (optimization A, gated behind this
   baseline).

2. **Non-atomic ref counting for compute/effect closures** — the v0.2.0 `RcPtr`
   work covered *values* but `ComputeFnPtr`/`EffectFnPtr` were still
   `std::shared_ptr` (atomic control block on every slot/effect creation).
   v0.3.0 swaps them to `RcPtr<RcBox<…>>` (non-atomic). This is the recompute
   path, so `cold_full_recalc` (~28% faster at 10M) and
   `full_recalc_invalidate_all` (~2× faster at 10M) improve; build cost is flat
   (it is dominated by per-cell value allocation, not closures).

3. **`RcPtr::adopt_t` refcount fix** — `RcBase` initializes `rc_strong = 0` and
   `adopt_t` previously stored the pointer without setting it to 1, so a
   singly-held adopted value leaked and any copy led to use-after-free. v0.3.0
   makes `adopt_t` establish refcount = 1. This fixes a silent leak in the
   value path introduced in v0.2.0 (values were never `RcPtr`-copied, so it
   only leaked) and is required for the closure path (closures are copied).

### What v0.3.0 deliberately does NOT do

To avoid over-optimizing micro-benchmarks at the expense of real-world system
performance, v0.3.0 does **not** tune `SmallVec`/`SmallFn` inline capacities,
hand-roll edge walks, or chase cached-read below ~20 ns — those move
`BENCHMARKS.md` rows without touching a real hot path. The real high-load
levers (read/write-lock concurrency on `ThreadSafeContext`, inline value
storage, per-kind node vectors) are sequenced behind this baseline.

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

## Scale (up to 10M cells — Google Sheets capacity) — `#lzscalebench`

The `scale` group is a rigorous benchmark over a spreadsheet-shaped graph of `N`
input cells + `N` formula slots (`formula[i] = input[i] + input[i-1]`). At
`N = 10,000,000` that is **20,000,000 reactive nodes** — the full Google Sheets
workbook cell limit.

### Spreadsheet-scale results

| N (cells) | Nodes | Build | Cold recalc | Full recalc (edit all) | Viewport (edit 1, read 1k) |
|---:|---:|---:|---:|---:|---:|
| 100,000 | 200K | 10.8 ms | 10.3 ms | 8.3 ms | 40.4 us |
| 1,000,000 | 2M | 130.0 ms | 111.0 ms | 117.0 ms | 35.1 us |
| 2,000,000 | 4M | 234.0 ms | 181.0 ms | 177.0 ms | 27.7 us |
| 10,000,000 | 20M | 1.383 s | 827 ms | 778 ms | 43.8 us |

**Per-node costs at 10M cells (20M nodes):** ~69 ns/node build, ~83 ns/formula
cold recalc (down from ~112 ns in v0.2.0 — the non-atomic compute closures).
Capacity scales linearly — the per-node cost at 10M is within 2× of the 100K
baseline, confirming the model does not degrade at spreadsheet scale.

**Viewport property:** The viewport recalc stays at **~28–44 us** from 100K to
10M cells — independent of sheet size. This is the lazy-pull advantage: editing
one input and reading a 1,000-cell viewport is **~17,000× cheaper** than a full
10M-cell recalc because off-viewport formulas are left dirty and never
recomputed.

### What the four cases mean

- `build` — constructs 2N nodes (N input cells + N formula slots)
- `cold_full_recalc` — computes every formula from a cold start (first read)
- `full_recalc_invalidate_all` — re-edits every input then recomputes the whole sheet
- `viewport_recalc` — edits one input and reads only a 1,000-cell viewport

### Spreadsheet cell-count context

How the two dominant spreadsheets bound a sheet:

| Spreadsheet | Documented limit | Cells |
|---|---|---:|
| **Google Sheets** | 10,000,000 cells per workbook (also 18,278 columns max) | **10,000,000** |
| **Microsoft Excel** | 1,048,576 rows × 16,384 columns per worksheet | **17,179,869,184** |

Excel's 17.18B is the *grid capacity*, not a populated-cell count. lazily-cpp's
storage is a **sparse arena** (`std::vector<std::optional<Node>>` with a
free-list) that only allocates cells you actually create. The practical limit
is *populated* cells vs. available RAM. The 10M-cell benchmark (20M nodes,
~5 GB RSS) confirms the model scales linearly to the full Google Sheets
workbook capacity. At ~69 ns/node build and ~83 ns/formula recalc, lazily-cpp
can construct and recompute a complete 10M-cell sheet in ~1.4 s and ~0.83 s
respectively.

### Per-node cost comparison with lazily-rs

| Metric | lazily-cpp 0.1.0 | lazily-cpp 0.2.0 | lazily-cpp 0.3.0 | lazily-rs |
|---|---:|---:|---:|---:|
| cached read (Context) | 304 ns | 19 ns | 23 ns | 10.5 ns |
| cached read (ThreadSafeContext) | 411 ns | 22 ns | 22 ns | 67 ns |
| cold first get (Context) | 2.52 us | 88 ns | 97 ns | 93 ns |
| cold first get (ThreadSafeContext) | 2.79 us | 98 ns | 107 ns | 1.13 us |
| fan-out 256 (Context) | 88 us | 1.05 us | 1.12 us | 72 us |
| fan-out 256 (ThreadSafeContext) | 94 us | 1.68 us | 1.68 us | 219 us |
| set_cell high_fan_out 512 | 125 us | 3.08 us | 3.26 us | 145 us |
| memo equality (Context) | 988 ns | 34 ns | 34 ns | 3.29 us |
| effect flushing (Context) | 2.39 us | 127 ns | 87 ns | 99 ns |
| batch storms 64 (Context) | 63 us | 4.45 us | 4.22 us | 3.85 us |
| scale build 1M | 1.28 s | 169 ms | 130 ms | 105 ms |
| scale full_recalc 10M | — | 1.61 s | 778 ms | — |
| scale viewport_recalc 1M | 439 us | 39 us | 35 us | 16 us |

**Honest read:** v0.3.0 keeps the micro-benchmark wins from v0.2.0 (numbers are
within run-to-run noise) and adds real gains on the **recompute paths**: scale
`full_recalc_invalidate_all` at 10M dropped from 1.61 s → 0.78 s (~2×) and
`cold_full_recalc` at 10M from 1.12 s → 0.83 s (~28%), from moving compute/effect
closures off `std::shared_ptr` (atomic) onto non-atomic `RcPtr`. lazily-cpp now
**beats lazily-rs** on effect flushing (87 ns vs 99 ns) and stays ahead on
fan-out 256, set_cell high_fan_out, and memo equality.

lazily-rs retains an edge on scale build (105 ms vs 130 ms at 1M). The remaining
gap is the per-cell value allocation (`new RcBox<T>` on every cell —
optimization B, inline value storage, not yet done) vs Rust's `Rc<T>`
monomorphization — an architectural difference that keeps the C++ API
type-erased but costs one allocation per node.

The key optimizations across versions:
1. `SmallFn` eliminated per-node `std::function` heap allocations (v0.2.0)
2. `SmallVec` eliminated per-edge `std::vector` heap allocations for 0–2 edges (v0.2.0)
3. Function-pointer `EqualsFn` shrank `SlotNode` by 24 bytes (better cache) (v0.2.0)
4. `Context::reserve()` eliminated vector reallocation during bulk construction

## Cross-language comparison (lazily-rs / lazily-cpp / lazily-zig)

Head-to-head on the same spreadsheet-shaped workload (`N` input cells + `N`
formula slots, `formula[i] = input[i] + input[i-1]`), measured on `x86_64`
Linux. lazily-rs uses criterion; lazily-cpp uses its `std::chrono` harness;
lazily-zig uses `clock_gettime(.MONOTONIC)` for the scale bench. Numbers are
the current published results from each repo's `BENCHMARKS.md`.

### Micro-benchmarks (single-threaded `Context` unless noted)

| Metric | lazily-rs | lazily-cpp | lazily-zig |
|---|---:|---:|---:|
| cached read (Context) | 10.5 ns | 23 ns | — † |
| cached read (ThreadSafeContext) | 67 ns | 22 ns | — † |
| cold first get (Context) | 93 ns | 97 ns | — † |
| cold first get (ThreadSafeContext) | 1.13 µs | 107 ns | — † |
| fan-out 256 (Context) | 72.5 µs | 1.12 µs | — † |
| fan-out 256 (ThreadSafeContext) | 219 µs | 1.68 µs | — |
| set_cell high_fan_out 512 | 145 µs | 3.26 µs | — † |
| memo equality suppression (Context) | 3.29 µs | 34 ns | — † |
| effect flushing (Context) | 99 ns | 87 ns | — |
| batch storms 64 (Context) | 3.85 µs | 4.22 µs | — |

† lazily-zig 0.17-dev removed `std.time.Timer`, so its reactive-core
micro-bench is **counter-based** (deterministic work-counts: allocations,
edges, recomputes — not wall-clock). The counters confirm the same zero-work
steady state (cached reads = 0 allocs / 0 recomputes) but are not directly
comparable on a wall-clock axis. See
[lazily-zig BENCHMARKS.md](https://github.com/lazily-hub/lazily-zig/blob/main/BENCHMARKS.md).

### Scale — 1M rows (~2M cells)

| Metric | lazily-rs | lazily-cpp | lazily-zig |
|---|---:|---:|---:|
| build (2N nodes) | 105 ms | 130 ms | 132 ms |
| cold full recalc | 106 ms | 111 ms | 381 ms |
| viewport recalc (edit 1, read 1k) | 15.6 µs | 35.1 µs | 6.4 µs |

### Scale — 10M cells (full Google Sheets workbook capacity)

| Metric | lazily-rs | lazily-cpp | lazily-zig |
|---|---:|---:|---:|
| build | 706 ms | 1.383 s | 1.13 s |
| cold full recalc | 518 ms | 827 ms | 2.26 s |
| viewport recalc | 11.4 µs | 43.8 µs | 6.6 µs |

**Honest read:** lazily-cpp's type-erased `SmallFn` + `SmallVec` node layout
wins the high-fan-out micro-benchmarks (fan-out 256, set_cell 512, memo
equality) by 30–97× over lazily-rs and keeps cached reads within 2× of
lazily-rs's monomorphized `Rc<T>` fast path. On the spreadsheet-scale wall
clock, lazily-rs leads build/cold-recalc (leaner per-node storage), while
lazily-zig's integer-keyed cache delivers the cheapest viewport reads. The
**shared headline** across all three: they back a full-capacity Google Sheets
workbook and all exhibit the **lazy-pull viewport property** — a one-cell
edit + bounded-viewport read stays in the **microsecond** range, independent
of sheet size, because off-viewport formulas are left dirty and never
recomputed (~5,000–650,000× cheaper than a full recalc).

## Thread-safe concurrency — read scaling

Two lock policies, run on the same workloads. v0.3.0 established the baseline
(the recursive single-mutex load cliff); v0.4.0 adds the opt-in `RwThreadSafeContext`
and measures both side by side. Each policy runs N worker threads for a fixed
30 ms window; completed ops are counted and reported as total throughput.

### Write-heavy (set_cell + get on one shared cell)

| Threads | recursive (Mops/s) | rw (Mops/s) |
|---:|---:|---:|
| 1 | 17.5 | 12.4 |
| 2 | 9.7 | 3.6 |
| 4 | 5.7 | 3.1 |
| 8 | 1.7 | 2.6 |
| 16 | 1.2 | 2.6 |

This is a single-writer-cell hotspot: it serializes under any design (only one
thread may mutate the cell at a time). The recursive policy wins at low thread
counts (its exclusive acquire is lighter); the RW policy is roughly even at high
counts. **Neither scales a contended writable cell** — that is fundamental, not a
lock-design problem.

### Read-scaling (cached get + get_cell, no writes) — the A2 win

| Threads | recursive (Mops/s) | rw (Mops/s) | scalable (Mops/s) |
|---:|---:|---:|---:|
| 1 | 46 | 54 | 58 |
| 2 | 14 | 23 | 127 |
| 4 | 10 | 16 | 242 |
| 8 | 7.9 | 13 | 473 |
| 16 | 7.8 | 12.5 | **925** |

- **recursive**: every read takes the exclusive lock → concurrent readers
  serialize, throughput *falls* as threads contend.
- **rw** (`shared_mutex`): cached reads take a shared lock, but all readers CAS
  one internal atomic → contend on one cache line → plateau at ~12.5 Mops/s.
- **scalable** (`ScalableRwLock`, v0.5.0): each reader thread touches its own
  cache line + reads a shared writer-waiting flag → reads scale
  **near-linearly**: 58 → 127 → 242 → 473 → 925 (≈16× at 16 threads). That is
  **~73× the RW plateau** and ~120× the recursive default at 16 threads.

### Honest read

- **Why a scalable lock, not lock-free?** True lock-free cached reads need
  atomic-refcounted values + hazard/epoch reclamation, which requires deep
  retrofitting of the type-erased engine's value-publish/recompute paths and
  introduces memory-safety risk disproportionate to the gain. The scalable lock
  achieves the same near-linear read scaling with **zero reclamation risk** and
  keeps the non-atomic `Rc` value path (no regression to the v0.2.0 win).
  Validated race-free under ThreadSanitizer (`tests/tsan_stress.cpp`).
- **Write trade-off**: `ScalableRwLock` writers scan all 128 reader slots per
  write, so it is the slowest policy for writes (7.6 Mops/s @N=1 vs recursive
  19.3; see contention table above). Reader-preferring — a steady reader stream
  can starve writers. Choose it for read-heavy loads; use the recursive default
  otherwise.
- **Why not make RW/scalable the default?** Both regress write-heavy
  single-thread paths. The recursive default keeps v0.3.0 performance exactly;
  the read-scaling policies are opt-in where they pay off.
- **Re-entrancy** (RW + scalable): wrapper calls made from inside a
  recompute/effect cascade (the thread already holding the exclusive lock)
  bypass locking via an owner token. Covered by regression tests.

## Benchmark methodology

Each benchmark runs `iterations` iterations of the body, measuring wall-clock
time per iteration via `std::chrono::high_resolution_clock`. A warmup phase
runs 10% of the iterations before measurement. Results are mean per-iteration
time in nanoseconds (reported as ns/us/ms/s as appropriate), or an explicit
unit (e.g. `Mops/s`) when the row carries `unit_override`.

The `contention/{recursive,rw,scalable}` and `read_scaling/{recursive,rw,scalable}`
groups run `n` worker threads for a fixed 30 ms wall-clock window, each counting
completed ops in a per-thread counter (relaxed atomics), and report total
throughput (Mops/s) and per-op latency (ns/op). They are single-window samples,
so intermediate thread counts are noisy; the 1 → 16 scaling trend is robust.

The `scale` group uses a single timed measurement per case (samples = 1) since
each case operates on 200K–20M nodes. The table reports the **median of 3 runs**
(single-sample cases carry ±15% run-to-run variance, so small deltas are not
meaningful). The `build` case includes a `reserve(2N)` call to pre-allocate the
node arena, matching real-world bulk-construction patterns.
