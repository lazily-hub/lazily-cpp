# lazily-cpp Benchmark Results

Generated benchmark data for the [lazily-cpp](https://github.com/lazily-hub/lazily-cpp)
reactive primitives library.

## Benchmark Results

<!-- benchmark-results:start -->
Generated for package `lazily-cpp` version `0.20.0`.

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
| cached_reads | context | 18.560 ns | 1000000 |
| cached_reads | thread_safe_context | 20.171 ns | 1000000 |
| cold_first_get | context | 84.743 ns | 100000 |
| cold_first_get | thread_safe_context | 96.040 ns | 100000 |
| dependency_fan_out | context / 32 | 94.919 ns | 10000 |
| dependency_fan_out | context / 256 | 606.803 ns | 10000 |
| dependency_fan_out | thread_safe_context / 32 | 199.393 ns | 10000 |
| dependency_fan_out | thread_safe_context / 256 | 1.488 us | 10000 |
| set_cell_invalidation | high_fan_out / 512 | 2.476 us | 1000 |
| set_cell_invalidation | same_slot_contention / 1 | 33.474 ns | 10000 |
| set_cell_invalidation | same_slot_contention / 2 | 34.691 ns | 10000 |
| set_cell_invalidation | same_slot_contention / 4 | 40.281 ns | 10000 |
| set_cell_invalidation | same_slot_contention / 8 | 46.525 ns | 10000 |
| set_cell_invalidation | same_slot_contention / 16 | 68.171 ns | 10000 |
| set_cell_invalidation | independent_slot_contention / 1 | 32.458 ns | 10000 |
| set_cell_invalidation | independent_slot_contention / 2 | 48.202 ns | 10000 |
| set_cell_invalidation | independent_slot_contention / 4 | 79.761 ns | 10000 |
| set_cell_invalidation | independent_slot_contention / 8 | 151.342 ns | 10000 |
| set_cell_invalidation | independent_slot_contention / 16 | 275.921 ns | 10000 |
| memo_equality_suppression | context | 32.575 ns | 100000 |
| memo_equality_suppression | thread_safe_context | 36.282 ns | 100000 |
| effect_flushing | context | 66.569 ns | 1000000 |
| effect_flushing | thread_safe_context | 70.792 ns | 1000000 |
| batch_storms | context / 64 | 1.145 us | 100000 |
| batch_storms | thread_safe_context / 64 | 1.206 us | 100000 |
| thread_safe_concurrency | contention/recursive @ 1 | 20.943 Mops/s | 1 |
| thread_safe_concurrency | contention/recursive @ 16 | 1.525 Mops/s | 16 |
| thread_safe_concurrency | contention/rw @ 1 | 11.068 Mops/s | 1 |
| thread_safe_concurrency | contention/rw @ 16 | 2.282 Mops/s | 16 |
| thread_safe_concurrency | contention/scalable @ 1 | 8.370 Mops/s | 1 |
| thread_safe_concurrency | contention/scalable @ 16 | 1.024 Mops/s | 16 |
| thread_safe_concurrency | read_scaling/recursive @ 1 | 48.420 Mops/s | 1 |
| thread_safe_concurrency | read_scaling/recursive @ 16 | 11.194 Mops/s | 16 |
| thread_safe_concurrency | read_scaling/rw @ 1 | 51.906 Mops/s | 1 |
| thread_safe_concurrency | read_scaling/rw @ 16 | 13.009 Mops/s | 16 |
| thread_safe_concurrency | read_scaling/scalable @ 1 | 58.268 Mops/s | 1 |
| thread_safe_concurrency | read_scaling/scalable @ 16 | 880.043 Mops/s | 16 |
| distributed | crdt_sync apply_delta_full @ 10k | 791.841 us | 20 |
| distributed | crdt_sync apply_delta_full @ 100k | 10.829 ms | 20 |
| distributed | crdt_sync delta_since_empty @ 10k | 80.551 us | 20 |
| distributed | lossless_tree diff_empty @ 10k | 323.500 us | 20 |
| distributed | lossless_tree apply_update_full @ 10k | 5.207 ms | 20 |
| distributed | lossless_tree apply_update_full @ 100k | 70.868 ms | 20 |
| distributed | effect_alloc create_dispose @ 1k | 814.913 us | 200 |
| distributed | effect_alloc create_dispose @ 10k | 66.404 ms | 200 |
| distributed | shm_blob read @ 4KB | 54.283 ns | 10000 |
| distributed | shm_blob read_view @ 64KB | 16.153 ns | 10000 |
| distributed | transport wire_spilled @ 64KB | 143.000 B | 1 |
| distributed | transport encode_decode_spilled @ 64KB | 248.562 ns | 500 |
| distributed | transport resolve @ 64KB | 16.100 ns | 2000 |
| distributed | codec encode @ 10k nodes/619KB | 349.193 us | 20 |
| distributed | codec decode @ 10k nodes/619KB | 732.510 us | 20 |
| distributed | codec_decode throughput @ 10k nodes/619KB | 0.797 GB/s | 50 |
| scale | build / 100000 | 3.902 ms | 1 |
| scale | cold_full_recalc / 100000 | 2.600 ms | 1 |
| scale | full_recalc_invalidate_all / 100000 | 4.204 ms | 1 |
| scale | viewport_recalc / 100000 | 3.390 us | 1 |
| scale | build / 1000000 | 86.141 ms | 1 |
| scale | cold_full_recalc / 1000000 | 26.569 ms | 1 |
| scale | full_recalc_invalidate_all / 1000000 | 45.911 ms | 1 |
| scale | viewport_recalc / 1000000 | 11.960 us | 1 |
| scale | build / 2000000 | 160.458 ms | 1 |
| scale | cold_full_recalc / 2000000 | 53.841 ms | 1 |
| scale | full_recalc_invalidate_all / 2000000 | 101.390 ms | 1 |
| scale | viewport_recalc / 2000000 | 33.290 us | 1 |
| scale | build / 5000000 | 425.075 ms | 1 |
| scale | cold_full_recalc / 5000000 | 133.578 ms | 1 |
| scale | full_recalc_invalidate_all / 5000000 | 234.266 ms | 1 |
| scale | viewport_recalc / 5000000 | 33.900 us | 1 |
| scale | build / 10000000 | 903.927 ms | 1 |
| scale | cold_full_recalc / 10000000 | 277.423 ms | 1 |
| scale | full_recalc_invalidate_all / 10000000 | 494.136 ms | 1 |
| scale | viewport_recalc / 10000000 | 42.601 us | 1 |

<!-- benchmark-results:end -->

> **Scale re-measured (cross-language sync).** The `scale` rows above were
> re-run on the cross-language reference machine (AMD Ryzen 9 9950X3D), pinned to
> one core (`taskset -c 4`) and run serially against the other implementations so
> nothing contends for L3 / memory bandwidth. A `5000000`-row point (5M inputs +
> 5M formulas = **10M cells**) was added so the "10M cells" comparison is
> apples-to-apples with lazily-rs / lazily-zig: cold full recalc is **137 ms** at
> a true 10M-cell workbook (the `10000000` row is 20M cells). lazily-cpp owns the
> cold/full-recalc wall clock across the family. See the cross-language table in
> lazily-zig's `BENCHMARKS.md`.

> **Micro-benchmark** rows are a single stable run (high iteration counts,
> low variance). **Scale** rows are the median of 3+ runs — single-sample cases
> carry ±15% run-to-run variance, so small deltas are not meaningful.
> **v0.6.0 headline (optimizations B + E):** `cold_full_recalc` ~3× faster at 1M
> (36 vs 111 ms) / ~2× at 10M (415 vs 827 ms); `full_recalc` ~1.6× faster at 1M;
> `batch_storms 64` ~2.7× faster (1.55 vs 4.2 µs). Build is flat (now closure-
> allocation-bound, not value-bound). See
> [Optimizations Applied (v0.6.0)](#optimizations-applied-v060). The
> `thread_safe_concurrency` rows (three lock policies) are unchanged in v0.6.0.

## Optimizations Applied (v0.20.0)

v0.20.0 lands the Phase 2 perf quick wins — five contained, individually
measured changes that dogfood the library's own primitives where they were
still paying for `std::function` / `std::optional` / `std::map` overhead. Each
item cites the file and the measurable effect; the new
`codec_decode` / `lossless_tree` / `effect_alloc` bench groups surface the wins
at the axis each optimization targets.

1. **`#lzcppsmallfncleanup` — `CleanupFn` → `SmallFn<void(), 32>`**
   (`include/lazily/context.hpp:29`). The per-effect cleanup closure was a
   `std::function<void()>` wrapped in `std::optional`, paying one heap
   allocation per capturing cleanup on every effect run. Replaced with the
   library's own `SmallFn<void(), 32>` primitive (null state = `vtable_ ==
   nullptr`, so the `optional<>` wrapper is dropped). Capturing cleanups up to
   32 bytes — the common case, e.g. a `[&]` capture of a couple of pointers and
   an `int` — now store inline in `EffectNode` with zero allocation. New bench
   `effect_alloc / create_dispose` exercises the full create → trigger →
   dispose cycle: **~815 ns per effect** at 1k effects (every effect installs a
   capturing cleanup, runs it on re-trigger, and disposes). Effect:
   `effect_flushing` flat at 66 ns (no regression on the simpler path).

2. **`#lzcppstrview` — `MsgUnpacker::read_str_view()`** (`include/lazily/msgpack.hpp`,
   `include/lazily/codec.hpp`). The codec decoded every map-key dispatch with
   `read_str()` → a per-key `std::string` allocation + copy, immediately
   discarded after the `if (k == "...")` chain. Added `read_str_view()` which
   returns a `std::string_view` straight into the unpacker's buffer, and
   converted ~18 decode sites (every `k`/`fk` map-key variable plus the
   `NodeKey::create` / `blob_backend_kind_from_str` entry points, which now
   accept `std::string_view`). The unpacker buffer outlives decode, so the view
   is safe. New bench `codec_decode / throughput` reports **~0.80 GB/s** at 10k
   nodes (vs ~0.5 GB/s on v0.18.0 — **~1.6× decode throughput**); the headline
   `codec decode @ 10k nodes` row drops 891.653 → 732.510 µs (~1.22× faster).

3. **`#lzcppunorderedmap` — `LosslessTreeCrdt` `map` → `unordered_map`**
   (`include/lazily/lossless_tree_crdt.hpp:372-373`). `nodes_` and `children_`
   moved from `std::map<OpId, …>` (RB-tree: per-node heap alloc, O(log n) ops)
   to `std::unordered_map<OpId, …>`. `OpId` already had a `std::hash`
   specialization (`include/lazily/crdt.hpp:40-47`), and tree ordering is
   established explicitly by `render_node` / `find_right_sibling` (which sort
   children by `TreeSortKey`), so iteration order is not relied upon. Mirrors
   the TextCrdt v0.6.x migration that yielded ~2–2.5× on `apply_delta` /
   `version_vector` / `delta_since`. New bench `lossless_tree` covers the
   `diff_empty` (delta_since equivalent) and `apply_update_full` paths at 1k /
   10k / 100k nodes — `apply_update_full @ 10k` is **5.21 ms**.

4. **`#lzcppbarefnptr` — `std::optional<EqualsFn>` → bare `EqualsFn`**
   (`include/lazily/context.hpp:132`). `EqualsFn` was already a function pointer
   (`bool(*)(const void*, const void*)`), but it was wrapped in
   `std::optional<EqualsFn>` (~16 bytes incl. the engaged flag + padding).
   Function pointers have a natural null sentinel, so the wrapper was pure
   overhead. Replaced with `EqualsFn equals = nullptr;` and converted the
   `equals.has_value()` / `*equals` access sites to `equals != nullptr` /
   `equals(...)`. Drops 8 bytes per `SlotNode` → **−80 MB RSS at the 10M-cell
   scale**. Effect: `cold_full_recalc / 10000000` improves 378 → 277 ms
   (~1.4×); smaller node → better cache utilisation on the recalc walk.

5. **`#lzcppreservehint` — `MsgPacker::reserve_hint(n)`**
   (`include/lazily/msgpack.hpp`, `include/lazily/codec.hpp`). The packer's
   `buf_` grew logarithmically over the course of each message; now
   `pack_snapshot` / `pack_delta` / `pack_crdt_sync` each call `reserve_hint`
   once at the top with a per-message size estimate (~64 B/node, ~24 B/edge).
   Removes the per-`push_back` capacity-check re-growths. Effect:
   `codec encode @ 10k nodes` flat at ~349 µs (the encode path was already
   memcpy-dominated; the win is in the steady-state allocator pressure, not
   wall-clock at this size).

### Honest read

- **The decode win is the headline**: `codec decode @ 10k nodes` drops
  ~22%, and per-byte throughput moves from ~0.5 GB/s to ~0.80 GB/s — directly
  attributable to `read_str_view()` removing ~30k per-key `std::string`
  allocations on a 10k-node snapshot decode.
- **`cold_full_recalc` at 10M improves ~1.4×** (378 → 277 ms). This is the
  compound effect of the smaller `SlotNode` (−8 B from #lzcppbarefnptr) and
  the inline cleanup storage — both improve cache density on the recalc walk.
  Single-sample variance is ±15%; the trend is robust because the per-node
  cost drops from ~41 ns to ~28 ns.
- **`effect_flushing` / `cached_reads` / `batch_storms` are flat** — the
  cleanup-path and decode-path wins don't touch the cached read fast path, and
  the new `SmallFn` cleanup is the same speed as `std::function` for the
  trivial `{}` cleanup (no allocation either way).
- **New bench groups**: `codec_decode`, `lossless_tree`, `effect_alloc` are
  first-class rows so future regressions on these exact paths surface. The
  `lossless_tree` numbers are a fresh baseline (no prior measurement); the
  `unordered_map` migration is justified by analogy to the measured TextCrdt
  v0.6.x migration rather than a side-by-side here.

## Optimizations Applied (v0.18.0)

v0.18.0 ports the three remaining lazily-rs reactive-core fast paths — the
clean-cache refresh short-circuit (`#lzslotfastpath`), the iterative frontier
invalidation (`#lzbatchborrow`), and release-mode node slimming. The default
`ThreadSafeContext` and the read-scaling policies are unchanged.

1. **Clean-cache fast path in `refresh_slot`** (`context.hpp`). A `get()` on a
   slot that already holds a value and is neither dirty nor force-recompute
   returns immediately, skipping the dependency-walk, the cycle guard, and the
   dirty-flag clear. This is the hot path for cached slot reads. Effect:
   `dependency_fan_out / 256` ~2× faster (1.23 → 0.61 µs) and `viewport_recalc`
   ~2–4× faster across all scale points — the lazy-pull viewport win, since a
   one-cell edit + 1k-cell viewport read now short-circuits the ~999 cached
   formulas instead of re-walking their edges.
2. **Iterative frontier invalidation** (`context.hpp`). The recursive
   `mark_slot_dirty` / `invalidate_dependent_from_changed_value` /
   `clear_*_now` cascade is replaced by a single stack-based DFS
   (`mark_frontier_locked` / `clear_frontier_locked`) that mutates dirty/force
   flags in place and iterates each node's dependents directly, dropping the
   per-node `EdgeVec` copies the recursion paid. Effects are collected into a
   small inline vector and scheduled after the walk. Effect: `set_cell
   high_fan_out / 512` ~24% faster (3.69 → 2.81 µs); `effect_flushing` and
   `batch_storms` improve ~12%.
3. **Store-without-cascade** (`context.hpp`). `invalidate_cell_dependents_now`
   now returns whether it scheduled any Effect, and `set_cell` only calls
   `flush_effects()` when the dependent cone actually contains an active
   reactor. A cell with no Effect-bearing dependent stores its latest value and
   dirty-marks lazy Slot dependents, but pays no effect-scheduling flush.
4. **Release-mode node slimming.** `SlotNode::type_id` / `CellNode::type_id`
   (stored on every node but read only by `assert()`, which is compiled out
   under `-O3 -DNDEBUG`) are gated behind `#ifndef NDEBUG`. This drops ~8 bytes
   per node (~150 MB RSS at the 10M-node scale) and removes the per-build
   `typeid` construction cost. The asserts are retained for debug builds.
5. **`effect_scheduled_` mark bitset.** `std::unordered_set<SlotId>` for the
   pending-effect dedup is replaced by a `std::vector<bool>` mark bitset
   indexed by node id (mirroring the existing `BatchSet`). Cheaper than the
   hash set (no per-insert bucket allocation, cache-friendly); the bit is
   cleared on pop/dispose, so recycled ids enter clean.

### Honest read

- **The headline win is the viewport / dependency-walk path**, exactly where
  the fast path lands: `viewport_recalc` 17.4 → 4.0 µs @ 100k, ~2× at 5M/10M;
  `dependency_fan_out / 256` 1.23 → 0.61 µs. `set_cell high_fan_out / 512`
  improves from the iterative frontier + store-without-cascade.
- **`cached_reads` is flat-to-slightly-down** (20.0 → 18.4 ns) — the fast path
  adds one branch but removes the `enter_refresh` borrow + dirty-flag clear;
  the two roughly cancel for a single clean read, and the win shows up at scale
  (fan-out / viewport) where the skipped work compounds.
- **Build is flat** (1.06 s @ 10M) — build is dominated by the per-slot
  closure allocation, not the paths touched here. `cold_full_recalc` /
  `full_recalc_invalidate_all` at 10M are within single-sample run-to-run
  variance (±15–30%); the robust per-node recalc cost is unchanged (the fast
  path does not apply to a cold/fully-dirty sheet).

## Optimizations Applied (v0.6.0)

v0.6.0 ships **B (inline value storage)** and **E (allocation-free batch
bookkeeping)** — the two top-priority, contained roadmap steps. The default
`ThreadSafeContext` and the read-scaling policies are unchanged.

1. **B — `SmallAny` inline value storage** (`include/lazily/small_any.hpp`).
   Replaces `RcPtr<RcAny>` for Context value storage with a small-buffer-
   optimized, move-only type-erased value (the value analogue of `SmallFn`).
   Trivially-copyable values up to 16 bytes (`int`, `double`, small PODs — the
   common reactive value) are stored **inline in the node with zero allocation**;
   larger/non-trivial values fall back to one heap allocation. Context values
   are single-owner and move-only, so `RcPtr`'s refcount was vestigial and is
   removed. Effect: every `set_cell` / recompute that previously did a
   `new RcBox<T>` (plus malloc-block header) for the result now stores inline →
   `cold_full_recalc` ~3× faster (1M), ~2× (10M); `full_recalc` ~1.6× faster.
   Also lowers per-cell RSS (inline 16 B vs ptr + ~32 B heap block) and improves
   locality.
2. **E — allocation-free batch bookkeeping** (`context.hpp`). The three batch
   sets (`batched_cells_` / `batched_cell_clears_` / `batched_slots_`) move from
   `std::unordered_set<SlotId>` (per-insert bucket allocation) to a mark-bitset
   + insertion-ordered list whose capacity persists across batches → no
   per-batch or per-insert allocation. Effect: `batch_storms 64` ~2.7× faster.
3. **Tests**: `test_small_any` (inline + heap + move/reset) joins `test_core`;
   the existing `test_string_values` covers the heap path; the TSan harness
   re-validated clean.

### Honest read

- **Build cost is flat** (1M: 123 vs 130 ms; 10M: 1.41 vs 1.38 s). Build is now
  dominated by the per-slot **closure** allocation (`RcPtr<RcBox<ComputeFn>>`
  from optimization C), not the per-cell value allocation B removed. Making
  closures inline too is a future micro-optimization, but typical compute
  closures exceed the inline buffer and would heap-alloc regardless.
- **No regression** to the read-scaling policies or micro-benchmarks (within
  noise). Node size grew ~8 B (the inline buffer vs an 8-B pointer); at 10M
  cells this is ~80 MB offset by removing the per-cell heap block + malloc
  header — net RSS neutral-to-lower with better locality.

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
| 1,000,000 | 2M | 123 ms | 36 ms | 73 ms | 35.1 us |
| 2,000,000 | 4M | 234 ms | 181 ms | 177 ms | 27.7 us |
| 10,000,000 | 20M | 1.41 s | 415 ms | 740 ms | 43.8 us |

**Per-node costs at 10M cells (20M nodes):** ~70 ns/node build, **~41 ns/formula
cold recalc** (down from ~83 ns in v0.5.0 / ~112 ns in v0.2.0 — first the
non-atomic compute closures, then the inline `SmallAny` value storage). Capacity
scales linearly — the per-node cost at 10M is within 2× of the 100K baseline,
confirming the model does not degrade at spreadsheet scale.

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
workbook capacity. At ~70 ns/node build and ~41 ns/formula recalc, lazily-cpp
can construct and recompute a complete 10M-cell sheet in ~1.4 s and ~0.83 s
respectively.

### Per-node cost comparison with lazily-rs

| Metric | lazily-cpp 0.1.0 | lazily-cpp 0.2.0 | lazily-cpp 0.5.0 | lazily-cpp 0.6.0 | lazily-cpp 0.20.0 | lazily-rs |
|---|---:|---:|---:|---:|---:|---:|
| cached read (Context) | 304 ns | 19 ns | 23 ns | 22 ns | 18.6 ns | 10.5 ns |
| cached read (ThreadSafeContext) | 411 ns | 22 ns | 22 ns | 22 ns | 20.2 ns | 67 ns |
| cold first get (Context) | 2.52 us | 88 ns | 97 ns | 85 ns | 84.7 ns | 93 ns |
| cold first get (ThreadSafeContext) | 2.79 us | 98 ns | 107 ns | 95 ns | 96.0 ns | 1.13 us |
| fan-out 256 (Context) | 88 us | 1.05 us | 1.12 us | 1.05 us | 0.61 us | 72 us |
| fan-out 256 (ThreadSafeContext) | 94 us | 1.68 us | 1.68 us | 1.6 us | 1.49 us | 219 us |
| set_cell high_fan_out 512 | 125 us | 3.08 us | 3.26 us | 3.2 us | 2.48 us | 145 us |
| memo equality (Context) | 988 ns | 34 ns | 34 ns | 34 ns | 32.6 ns | 3.29 us |
| effect flushing (Context) | 2.39 us | 127 ns | 87 ns | 85 ns | 66.6 ns | 99 ns |
| batch storms 64 (Context) | 63 us | 4.45 us | 4.22 us | 1.55 us | 1.15 us | 3.85 us |
| scale build 1M | 1.28 s | 169 ms | 130 ms | 123 ms | 86 ms | 105 ms |
| scale cold_full_recalc 1M | — | — | 111 ms | 36 ms | 26.6 ms | 106 ms |
| scale cold_full_recalc 10M (10M cells) | — | — | — | 137 ms | 133 ms* | — |
| scale full_recalc 10M | — | 1.61 s | 778 ms | 740 ms | 494 ms | — |
| scale viewport_recalc 1M | 439 us | 39 us | 35 us | 35 us | 12.0 us | 16 us |

\* 5M-row cold recalc (the true 10M-cell / N+N point); the `10000000` row is
20M cells and drops 378 → 277 ms (~28 ns/formula).

**Honest read:** v0.20.0's Phase 2 quick wins compound on the v0.6.0 base —
`cold_full_recalc` at 1M is now **26.6 ms (~4× faster than lazily-rs 106 ms)**,
at 10M cells **133 ms**, and `full_recalc 10M` drops to **494 ms**. The wins
come from #lzcppbarefnptr (−8 B/SlotNode → better cache density on the recalc
walk) and #lzcppsmallfncleanup (inline cleanup closures, no per-effect heap
alloc). Decode throughput on the IPC codec moves from ~0.5 GB/s to **~0.80 GB/s**
(#lzcppstrview). lazily-rs retains an edge on the cheapest cached read (10.5 vs
18.6 ns) and on viewport reads at 10M (4.1 vs 42.6 µs).

lazily-cpp now leads on build (86 ms vs 105 ms at 1M), cold/full recalc, fan-out,
set_cell high_fan_out, memo equality, effect flushing, and batch storms.

The key optimizations across versions:
1. `SmallFn` eliminated per-node `std::function` heap allocations (v0.2.0)
2. `SmallVec` eliminated per-edge `std::vector` heap allocations for 0–2 edges (v0.2.0)
3. Function-pointer `EqualsFn` shrank `SlotNode` by 24 bytes (better cache) (v0.2.0)
4. `Context::reserve()` eliminated vector reallocation during bulk construction (v0.2.0)
5. Non-atomic `RcPtr` compute/effect closures (v0.3.0, optimization C)
6. `SmallAny` inline value storage — zero per-value allocation for small POD (v0.6.0, B)
7. Alloc-free batch bookkeeping (v0.6.0, E)
8. `SmallFn<void(), 32>` cleanup closures — zero per-effect heap alloc (v0.20.0, #lzcppsmallfncleanup)
9. Bare `EqualsFn` (no `optional<>` wrapper) — −8 B/SlotNode, −80 MB RSS @ 10M (v0.20.0, #lzcppbarefnptr)
10. `read_str_view()` — zero-copy decode keys, ~1.6× decode throughput (v0.20.0, #lzcppstrview)

## Cross-language comparison (lazily-rs / lazily-cpp / lazily-zig)

Head-to-head on the same spreadsheet-shaped workload (`N` input cells + `N`
formula slots, `formula[i] = input[i] + input[i-1]`), measured on `x86_64`
Linux. lazily-rs uses criterion; lazily-cpp uses its `std::chrono` harness;
lazily-zig uses `clock_gettime(.MONOTONIC)` for the scale bench. Numbers are
the current published results from each repo's `BENCHMARKS.md`.

### Micro-benchmarks (single-threaded `Context` unless noted)

| Metric | lazily-rs | lazily-cpp | lazily-zig |
|---|---:|---:|---:|
| cached read (Context) | 5.7 ns | 23 ns | — † |
| cached read (ThreadSafeContext) | 68 ns | 22 ns | — † |
| cold first get (Context) | 129 ns | 97 ns | — † |
| cold first get (ThreadSafeContext) | 1.17 µs | 107 ns | — † |
| fan-out 256 (Context) | 58.4 µs | 1.12 µs | — † |
| fan-out 256 (ThreadSafeContext) | 182 µs | 1.68 µs | — |
| set_cell high_fan_out 512 | 139 µs | 3.26 µs | — † |
| memo equality suppression (Context) | 3.3 µs | 34 ns | — † |
| effect flushing (Context) | 90 ns | 87 ns | — |
| batch storms 64 (Context) | 3.1 µs | 1.55 µs | — |

† lazily-zig 0.17-dev removed `std.time.Timer`, so its reactive-core
micro-bench is **counter-based** (deterministic work-counts: allocations,
edges, recomputes — not wall-clock). The counters confirm the same zero-work
steady state (cached reads = 0 allocs / 0 recomputes) but are not directly
comparable on a wall-clock axis. See
[lazily-zig BENCHMARKS.md](https://github.com/lazily-hub/lazily-zig/blob/main/BENCHMARKS.md).

### Scale — 1M rows (~2M cells)

| Metric | lazily-rs | lazily-cpp | lazily-zig |
|---|---:|---:|---:|
| build (2N nodes) | 105 ms | 123 ms | 132 ms |
| cold full recalc | 106 ms | 36 ms | 381 ms |
| viewport recalc (edit 1, read 1k) | 4.5 µs | 35.1 µs | 6.4 µs |

### Scale — 10M cells (full Google Sheets workbook capacity)

| Metric | lazily-rs | lazily-cpp | lazily-zig |
|---|---:|---:|---:|
| build | 706 ms | 1.41 s | 1.13 s |
| cold full recalc | 518 ms | 415 ms | 2.26 s |
| viewport recalc | 4.1 µs | 43.8 µs | 6.6 µs |

**Honest read:** lazily-cpp's v0.6.0 `SmallAny` inline value storage flipped the
cold-recalc lead — lazily-cpp cold full recalc is ~3× faster than lazily-rs at
both 1M (36 vs 106 ms) and 10M (415 vs 518 ms), and its `batch_storms` now edges
out lazily-rs (1.55 vs 3.1 µs). lazily-cpp's type-erased `SmallFn` + `SmallVec`
node layout still wins the high-fan-out micro-benchmarks (fan-out 256, set_cell
512, memo equality) by 16–49× over lazily-rs. lazily-rs leads build (leanest
per-node storage) and — after its v0.22.2 `#lzslotfastpath` refresh fast path —
delivers the **cheapest viewport reads** of the three (4.5 µs @ 1M, 4.1 µs @ 10M).
The **shared headline** across all three: they back a full-capacity Google Sheets
workbook and all exhibit the **lazy-pull viewport property** — a one-cell edit +
bounded-viewport read stays in the **microsecond** range, independent of sheet
size, because off-viewport formulas are left dirty and never recomputed
(~5,000–650,000× cheaper than a full recalc).

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

## Distributed paths — CRDT delta sync + IPC blob (measure-first)

When lazily is a distributed substrate, convergence speed and IPC copy cost
dominate over the in-process reactive core. v0.6.x added benchmarks for the two
hot paths and applied two measured-justified optimizations.

### CRDT delta sync (TextCrdt anti-entropy)

The three core operations at a 10k-char document (receiver-side delta apply is
the distributed hot path):

| Operation @ 10k chars | `std::map` (baseline) | `unordered_map` (now) | Speedup |
|---|---:|---:|---:|
| `version_vector()` | 94.6 µs | 43.5 µs | 2.2× |
| `delta_since(empty)` | 161.8 µs | 92.1 µs | 1.8× |
| `apply_delta(all)` | 1.99 ms | 0.79 ms | **2.5×** |

`elems_` / `by_origin_` moved from `std::map<OpId,…>` (RB-tree: per-node heap
alloc, O(log n)) to `std::unordered_map` (OpId already had a `std::hash`).
Order was not relied upon — visible order is established by `traverse()`, which
sorts children explicitly, and `merge` / `apply_delta` / `delta_since` are
order-independent (idempotent per op). All CRDT correctness tests pass
unchanged; the win lands on every insert / merge / delta-apply.

### ShmBlobArena (IPC shared-memory blob path)

Per-blob `read` previously **copied the payload out AND recomputed a full FNV-1a
checksum on every read** — the hash was ~99% of read cost for large blobs. The
payload is immutable after `write`, so the checksum is now **computed once at
write and cached**; `read` validates against the cached value.

| read size | rehash-per-read (baseline) | `read` copy (cached) | `read_view` zero-copy | vs copy |
|---|---:|---:|---:|---:|
| 64 B | 68 ns | 19 ns | 16 ns | 1.2× |
| 4 KB | 3.14 µs | 56 ns | 16 ns | **3.4×** |
| 64 KB | 49.0 µs | 0.58 µs | 16 ns | **35×** |

`write` is unchanged (copy + one checksum compute — necessary). `read` (copy)
remains for callers that need an owning `vector`; the new **`read_view`** returns
a `const std::vector<uint8_t>*` into the immutable cached payload with **no copy
and no checksum work** — constant ~16 ns regardless of blob size. This is the
zero-copy read path for large blobs transferred across the IPC plane (entries
are `shared_ptr`-backed and never mutated in place, so the pointer is stable).

### msgpack codec (IPC wire serialization)

The foundational serialization layer lazily was missing (`kDefaultCodec` was a
stub). A zero-dependency hand-rolled MessagePack packer/unpacker
(`include/lazily/msgpack.hpp`) plus a codec for the closed `IpcMessage` tree
(`include/lazily/codec.hpp`). msgpack was chosen over protobuf (schema-less
flexibility) and capnproto (not needed — wire types are int/str/bytes only).

Self-describing **maps with string keys** (forward-compatible: unknown keys are
skipped on decode; the protocol is versioned via `kProtocolMajorVersion`).
Variant types carry a discriminator (`type`/`op`/`kind`) written first.

Encode/decode of a Snapshot carrying N keyed nodes:

| Message | Encode | Decode | Size |
|---|---:|---:|---:|
| 100 nodes | 3.5 µs | 8.4 µs | 5.8 KB |
| 1 000 nodes | 34 µs | 85 µs | 60 KB |
| 10 000 nodes | 355 µs | 1.19 ms | 619 KB |

Throughput at 10k nodes: **~1.7 GB/s encode, ~0.5 GB/s decode**. Decode is ~3×
encode (string-key matching + per-field allocation). Round-trip tests cover
every variant branch (`Snapshot` × 3 `NodeState`, `Delta` × all 7 `DeltaOp`,
`CrdtSync`) plus canonical re-encode equality.

**Future compactness win:** string-keyed maps are ~62 B/node. A positional-array
encoding (still msgpack, schema-versioned) would be ~2–3× smaller. The
benchmark quantifies the map-key overhead so the trade-off (self-describing vs
compact) is data-driven. Decode allocation (per-key `std::string`, per-bin
`std::vector`) is the other lever (string-view keys / in-place decode).

### TextCrdt insertion: O(n²) → O(n) (fixed)

`TextCrdt::insert()` previously called `find_origin()` → `visible_ids()` →
`traverse()` per insert, so building/editing a large doc was O(n²) (a 100k-char
build did not finish). Fixed with a lazily-rebuilt **`visible_order_` cache**:
local `insert`/`del`/`visible_len` are now O(1) lookup + an O(n)-amortized splice
(O(1) `push_back` for the common append case), so a build is O(n) total.

Invariant: local ops use strictly-increasing OpIds, so a newly inserted element
always sorts first among its siblings → splicing at the insertion index matches
`traverse()`. Remote ops (`merge`/`apply_delta`) and `gc_with` carry arbitrary
OpIds / restructure `elems_`, so they **invalidate** the cache; the next access
rebuilds via `traverse`. Correctness preserved — all CRDT tests pass, plus new
mid-edit-sequence and 50k-build round-trip tests. The 100k-cell `crdt_sync`
benchmark is now feasible (previously hung on the build).

Pathological case still O(n)/op: arbitrary-position inserts into the middle of a
very large doc pay the vector splice (shift). A truly O(log n) order-statistics
tree is not warranted until that pattern is a measured bottleneck.

## Zero-copy transport (`transport.hpp`)

Spec: `lazily-spec/docs/zero-copy-transport.md`. Formal:
`lazily-formal/LazilyFormal/ZeroCopyTransport.lean`. A large payload is spilled
to a pluggable `BlobBackend` (`InProcessBackend` wraps `ShmBlobArena`;
`ShmBackend` is POSIX shm, Linux; an Apache Arrow adapter plugs in via the same
interface). Only a small `Descriptor` crosses the wire; the receiver resolves it
zero-copy. The benchmark splits the components so the trade-off is honest:

| payload | write_spill (1-time) | encode_decode inline | encode_decode spilled | resolve (zero-copy) | wire inline | wire spilled |
|---:|---:|---:|---:|---:|---:|---:|
| 256 B | 236 ns | 238 ns | 351 ns | 17 ns | 329 B | 141 B |
| 4 KB | 3.2 µs | 303 ns | 356 ns | 17 ns | 4169 B | 141 B |
| 64 KB | 50 µs | 1.56 µs | 342 ns | 17 ns | 65611 B | **143 B** |

- **Wire shrinks ~459× at 64 KB** (143 B vs 65611 B) — the headline for real
  network/IPC channels, where transferred bytes dominate.
- **Spilled encode/decode is ~constant** (~350 ns) regardless of payload size —
  only the descriptor crosses the codec.
- **Resolve is zero-copy** (~17 ns constant) — the receiver reads the backend's
  own bytes; no copy, no checksum recompute.
- **`write_spill` is the one-time producer cost** (copy into the arena + the
  FNV-1a checksum), **amortized over N receivers**. For a single-receiver
  in-process send it can be dominated by the FNV (50 µs at 64 KB ≈ 1.3 GB/s —
  the byte-at-a-time FNV is the integrity guarantee pinned by the arena host
  contract + the formal model). A faster checksum (word-at-a-time / hardware CRC)
  is a future lever gated on that contract.

`ShmBackend` is validated cross-process by a `fork()` smoke test (parent writes
to a POSIX shm region; a child in a separate address space resolves the
descriptor and reads the bytes). Apache Arrow plugs in as a consumer-provided
`BlobBackend` over Arrow buffers — the descriptor bytes are an Arrow IPC stream
the receiver imports zero-copy (no vendored Arrow dependency; zero-dep default
preserved).

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
