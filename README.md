# lazily (C++)

Lazy reactive primitives for C++17 — **Slots, Cells, and Signals** with automatic
dependency tracking and cache invalidation, plus the full [`lazily-spec`][spec]
wire protocol, CRDT collection types, keyed cell collections, Harel state
charts, and the distributed CRDT plane.

A C++ port of the lazily reactive family ([`lazily-rs`][rs], [`lazily-py`][py],
[`lazily-kt`][kt], [`lazily-js`][js], [`lazily-dart`][dart], [`lazily-zig`][zig],
[`lazily-go`][go]) — conformant with [`lazily-spec`][spec] and
[`lazily-formal`][formal]. The concurrency surfaces (thread-safe reactive
context, async reactive context, signaling room, CRDT anti-entropy plane) are
built on `std::thread`, `std::recursive_mutex`, and `std::future`.

## Overview

`lazily` provides five core primitives for reactive computation:

- **Context** — owns all reactive state and manages the dependency graph
- **Slot** — a lazily-computed cached value that automatically tracks dependencies
- **Cell** — a mutable value that invalidates dependent Slots when changed
- **Signal** — an eager derived value that recomputes the instant a dependency invalidates, with no intermediate unset value
- **Effect** — a side-effect callback that automatically reruns after tracked dependencies invalidate

Values are **lazy by default**: dependents are marked dirty on invalidation but only validated or recomputed when accessed. When you need eager push-style semantics — recompute immediately, observe `v1 -> v2` with no unset window — reach for **`Signal`**, which layers a puller effect over a memoized slot. The `Slot -> Cell -> Signal` progression lets you choose lazy or eager per derived value within one graph.
`ctx.memo()` Slots use a memo guard: if recomputation produces the same value, downstream dirty caches and effects are left alone.
Multiple updates can be grouped with `ctx.batch(...)` so invalidation and effect reruns happen once after the outermost batch exits.

## Feature Set

The full `lazily` capability set and its cross-language coverage across every
binding. Legend: ✅ shipped · `~` partial · `—` absent or not applicable. The
canonical matrix with per-cell notes and platform carve-outs lives in
[`lazily-spec` § Cross-Language Coverage](../lazily-spec/docs/coverage.md).

<!-- coverage-table:start -->
| Feature | Rust | Python | Kotlin | JS | Dart | Zig | Go | C++ |
| --------- | :----: | :------: | :------: | :--: | :----: | :---: | :--: | :---: |
| Reactive graph — `Cell` / `Slot` / `Signal` / `Effect` / memo / batch | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Reactive family (`ReactiveFamily`) — keyed cell/slot family + materialization mode (`#lzmatmode`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Thread-safe context (lock-backed) | ✅ | ✅ | ✅ | — | — | ✅ | ✅ | ✅ |
| Async reactive context | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Flat state machine | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Harel state charts | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Keyed cell collections (`CellMap` / `CellTree`) + reconcile | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Memoized semantic tree (`SemTree`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Stable-id alignment (manufactured identity) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Reactive queue (`QueueCell` SPSC/MPSC + `QueueStorage` adapter) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Free-text character CRDT (`TextCrdt`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| `TextCrdt` delta sync (`version_vector` / `delta_since` / `apply_delta`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Move-aware sequence CRDT (`SeqCrdt`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Lossless tree CRDT core (`LosslessTreeCrdt`, M1) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Lossless tree — dotted-frontier anti-entropy | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Lossless tree — concurrent merge convergence | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Registers (LWW / MV) + `PnCounter` + `CellCrdt` | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| IPC wire — `Snapshot` + `Delta` + `CrdtSync` | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Shared-memory blob path (`ShmBlobArena`) | ✅ | ✅ | ✅ | ~ | ~ | ✅ | ✅ | ✅ |
| Cross-process zero-copy transport (`BlobBackend` / shm / arrow) | ✅ | ✅ | ✅ | — | ✅ | ✅ | ✅ | ✅ |
| Distributed CRDT plane (`CrdtPlaneRuntime` / anti-entropy) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Distributed plane — WebRTC transport + signaling | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| State projection / mirror | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Causal receipts (`CausalReceipts` outcome projection) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Message-passing + RPC command plane (`command-plane-v1`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| C-ABI FFI boundary | ✅ | ✅ | ✅ | — | ✅ | ✅ | ✅ | ✅ |
| Permission boundary (`PeerPermissions` / `RemoteOp`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Capability negotiation (`SessionHandshake`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Instrumentation / benchmarks | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
<!-- coverage-table:end -->

CRDT convergence and the wire protocol are pinned by the shared conformance fixtures
and JSON Schemas in `lazily-spec` and the Lean models in `lazily-formal`.

## Benchmark highlights

Micro-benchmarks on `x86_64` with GCC 16, C++17 (`-O3`). Full results in
[BENCHMARKS.md](BENCHMARKS.md).

| Benchmark | Context | ThreadSafeContext |
|---|---:|---:|
| cached read | 23 ns | 22 ns |
| cold first get | 97 ns | 107 ns |
| fan-out 256 | 1.12 us | 1.68 us |
| set_cell high_fan_out 512 | 3.26 us | — |
| memo equality suppression | 34 ns | 38 ns |
| batch storms 64 | 4.22 us | 3.63 us |

**Scale (up to 10M cells — Google Sheets capacity, 20M nodes):** build ~1.4 s,
**cold recalc ~415 ms (~41 ns/formula — ~3× faster than lazily-rs at 1M)**,
viewport recalc (edit 1, read 1k) **~44 us** — independent of sheet size thanks
to the lazy pull-based model. Full table in
[BENCHMARKS.md](BENCHMARKS.md#scale-up-to-10m-cells--google-sheets-capacity--lzscalebench).

**Thread-safe concurrency (v0.5.0):** three opt-in lock policies ship. The
default `ThreadSafeContext` (recursive mutex) is unchanged. `RwThreadSafeContext`
(`shared_mutex`) scales cached reads ~2.6× at 16 threads; the new
`ScalableThreadSafeContext` (`ScalableRwLock`, per-cacheline reader counters)
scales cached reads **near-linearly — ~925 Mops/s at 16 threads (~73× the RW
plateau)**, at the cost of slower writes (writer scans a 128-slot reader pool).
Validated race-free under ThreadSanitizer. Choose by workload — see
[BENCHMARKS.md](BENCHMARKS.md#thread-safe-concurrency--read-scaling).

**Performance roadmap:** [ROADMAP.md](ROADMAP.md) — shipped optimizations
(v0.3.0–v0.5.0), the A3 lock-free analysis, and recommended next paths
(B inline value storage, D/E node-layout, distributed-computing IPC/CRDT paths).

**Cross-process zero-copy transport (v0.7.0):** large payloads cross the IPC
plane as small descriptors (not copies) via a pluggable `BlobBackend` —
`InProcessBackend` (wraps `ShmBlobArena`), `ShmBackend` (POSIX shm, Linux), and
an Apache Arrow adapter (consumer-provided). Wire shrinks ~459× at 64 KB;
resolve is zero-copy. Spec: lazily-spec `zero-copy-transport.md`. See
[BENCHMARKS.md](BENCHMARKS.md#zero-copy-transport-transporthpp).

## Usage

```cpp
#include <lazily/lazily.hpp>

lazily::Context ctx;
auto a = ctx.cell(2);
auto b = ctx.cell(3);
auto sum = ctx.computed<int>([&](lazily::Context& c) {
  return c.get_cell(a) + c.get_cell(b);
});

assert(ctx.get(sum) == 5);
ctx.set_cell(a, 10);
assert(ctx.get(sum) == 13);
```

### The reactive family

- **Slot** — a lazily-computed cached value that automatically tracks its
  dependencies and recomputes only when read after an upstream change.
- **Cell** — a mutable source value that invalidates dependent Slots/Signals
  when it changes.
- **Signal** — an *eager* derived value that recomputes the instant a dependency
  changes, with no intermediate unset value.

Values are **lazy by default**. When you need eager push-style semantics, reach
for `Signal`. Use `Effect` for side effects and `Memo` for an equality-guarded
derived value.

### Keyed reactive family & materialization mode

`ReactiveFamily<K, V, H>` maps keys to per-entry reactive nodes of a single
handle kind — `CellHandle<V>` (input cells) or `SlotHandle<V>` (derived slots).
Its **materialization mode** (`#lzmatmode`) is orthogonal to entry kind: it fixes
*when* a derived node is allocated, never *what* it observes.

```cpp
lazily::Context ctx;
using SlotFamily =
    lazily::ReactiveFamily<uint32_t, uint32_t, lazily::SlotHandle<uint32_t>>;

// Eager (default): every key's node is allocated up front.
auto eager = SlotFamily::eager(ctx, {0, 1, 2}, [](const uint32_t& k) { return k * 3; });
assert(eager.present_count() == 3);

// Lazy (opt-in): derived nodes are allocated on first read ("materialize on pull").
auto lazy = SlotFamily::lazy(ctx, {0, 1, 2, 5, 9}, [](const uint32_t& k) { return k * 3; });
assert(lazy.present_count() == 0);
assert(lazy.observe(ctx, 5) == 15);  // materializes just key 5
assert(lazy.present_count() == 1);
```

Materialization mode is **observationally transparent** — `eager.observe(ctx, k)
== lazy.observe(ctx, k)` for every key; lazy only changes allocation timing and
memory. Input-cell entries (`CellHandle`) are always materialized at build under
either mode; only derived slots are deferred under lazy. The contract is proved
in lazily-formal (`Materialization.lean`) and exercised against the shared
lazily-spec `conformance/materialization/*` fixtures.

### State charts

```cpp
lazily::Context ctx;
auto def = lazily::ChartBuilder()
  .state(lazily::StateBuilder::compound("root", "off"))
  .state(lazily::StateBuilder::atomic("off").parent("root").on("toggle", "on"))
  .state(lazily::StateBuilder::atomic("on").parent("root").on("toggle", "off"))
  .build().value();

lazily::StateChart chart(ctx, std::move(def));
std::unordered_map<std::string, bool> guards;
assert(chart.active_leaves(ctx)[0] == "off");
chart.send(ctx, "toggle", guards);
assert(chart.active_leaves(ctx)[0] == "on");
```

### CRDTs

```cpp
lazily::TextCrdt a(1);
a.insert_str(0, "hello world");

lazily::TextCrdt b(2);
auto delta = a.delta_since({});
b.apply_delta(delta);
assert(b.text() == "hello world");
```

### Reactive queue

```cpp
lazily::Context ctx;
auto q = lazily::QueueCell<std::string>::bounded(ctx, 2);

q.push(ctx, "a");
q.push(ctx, "b");
assert(q.try_push(ctx, "c") == lazily::PushResult::Full);  // backpressure

// A reactive reader observing the head — invalidated only when head changes.
auto head = ctx.computed<std::optional<std::string>>([&](lazily::Context& c) {
  return q.head(c);
});
assert(ctx.get(head).value() == "a");

assert(q.pop(ctx).value() == "a");
assert(ctx.get(head).value() == "b");  // head reader sees the new head

q.close(ctx);
assert(q.try_pop(ctx).is_closed());    // closed + empty → Closed (≠ Empty)
```

## Architecture

- **Context owns all nodes** in a `std::vector<std::optional<Node>>` indexed by
  `SlotId` (uint64_t) — cache-friendly, allocation-light, no hash probes on the
  read path.
- **Lightweight Copy handles** (`SlotHandle<T>`, `CellHandle<T>`,
  `EffectHandle`, `SignalHandle<T>`) are just ids — all data lives in the
  Context.
- **Type erasure** via `std::shared_ptr<void>` + `std::type_index` — the
  Context stores heterogeneous node types in a single `std::variant`.
- **SmallFn** — small-buffer-optimized type-erased callable (replaces
  `std::function` for compute/effect closures, zero heap allocation for typical
  lambdas).
- **SmallVec** — inline edge storage (same 24-byte footprint as `std::vector`
  but zero heap allocation for 0–2 edges, the common dependency fan-out).
- **Pull-based lazy recompute** with dependency tracking, cycle detection, and
  memo equality guard.
- **Batch coalescing** — cell writes inside a batch defer invalidation to the
  outermost boundary, producing one coalesced cascade.
- **Thread-safe context** wraps Context with `std::recursive_mutex`.
- **Async context** uses `std::future`/`std::thread` for async computations with
  revision tracking and stale-completion discard.
- **C-ABI FFI** — `lazily_ffi` shared library exports the `extern "C"` boundary
  (channel send/recv, message validate/kind/clone).

## Build

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

### Benchmarks

```bash
cmake -S . -B build -DLAZILY_BUILD_BENCHMARKS=ON
cmake --build build --target lazily_bench
./build/benches/lazily_bench
```

See [BENCHMARKS.md](BENCHMARKS.md) for full results, scale benchmarks (≥1M
cells), and a cross-language comparison with lazily-rs and lazily-zig.

### CMake integration

```cmake
add_subdirectory(lazily-cpp)
target_link_libraries(your_target PRIVATE lazily)
# For C-ABI FFI:
# target_link_libraries(your_target PRIVATE lazily_ffi)
```

## Modules

| Header | Module |
|--------|--------|
| `context.hpp` | Reactive graph core (Context, Slot, Cell, Signal, Effect, Memo, batch) |
| `small_fn.hpp` | SmallFn — small-buffer-optimized type-erased callable |
| `small_any.hpp` | SmallAny — small-buffer type-erased value (inline value storage, optimization B) |
| `small_vec.hpp` | SmallVec — inline edge storage (0–2 elements inline, heap fallback) |
| `rc_ptr.hpp` | RcPtr/ArcPtr smart pointers (closures), RcTraits/ArcTraits value-storage traits |
| `state_machine.hpp` | Flat state machine (Cell-backed FSM) |
| `statechart.hpp` | Full Harel/SCXML state charts (compound, parallel, history, actions, guards) |
| `collections.hpp` | CellMap, CellFamily, CellTree, keyed reconciliation (LIS) |
| `reactive_family.hpp` | ReactiveFamily — unified keyed cell/slot family + materialization mode (eager default / lazy opt-in, `#lzmatmode`) |
| `thread_safe_reactive_family.hpp` | ThreadSafeReactiveFamily — `Send + Sync` keyed family over ThreadSafeContext (mutex-guarded present set, materialization confluence, `#lzmatmode`) |
| `async_reactive_family.hpp` | AsyncReactiveFamily — keyed family over AsyncContext (`observe` → `std::optional<V>`, eventual transparency, `#lzmatmode`) |
| `queue.hpp` | QueueCell (SPSC/MPSC reactive queue) + QueueStorage adapter + VecDequeStorage |
| `sem_tree.hpp` | Memoized semantic tree (incremental fold, memo equality guard) |
| `thread_safe.hpp` | `BasicThreadSafeContext<Policy>` — `ThreadSafeContext` (recursive_mutex, default) + `RwThreadSafeContext` (shared_mutex) + `ScalableThreadSafeContext` (reader-scalable lock) |
| `async_context.hpp` | AsyncContext (Empty/Computing/Resolved/Error lifecycle) |
| `hlc.hpp` | Hybrid logical clock, StampFrontier |
| `crdt.hpp` | TextCrdt (+ delta sync), SeqCrdt, LwwRegister, MvRegister, PnCounter |
| `lossless_tree_crdt.hpp` | LosslessTreeCrdt (M1, dotted-frontier anti-entropy) |
| `stable_id.hpp` | Manufactured identity (anchors, content hashes, word-LCS alignment) |
| `ipc.hpp` | IPC wire types (Snapshot/Delta/CrdtSync), NodeKey, ShmBlobArena, PeerPermissions, CapabilityHandshake |
| `codec.hpp` | msgpack wire codec — `encode`/`decode` the `IpcMessage` tree |
| `msgpack.hpp` | Minimal zero-dependency MessagePack packer/unpacker |
| `transport.hpp` | Cross-process zero-copy transport — pluggable `BlobBackend` (`InProcessBackend`/`ShmBackend`), spill/resolve, `BlobRouter` |
| `receipt.hpp` | Causal receipts, StateProjectionMirror |
| `command.hpp` | Command plane (command-plane-v1), CrdtPlaneRuntime, instrumentation |
| `signaling.hpp` | WebRTC signaling room (peer discovery, SDP/ICE relay) |
| `ffi.hpp` | C-ABI FFI boundary (LazilyFfiChannel, extern "C" exports) |

## Related Projects

- [`lazily-spec`][spec] — wire protocol specification, JSON Schemas, conformance fixtures
- [`lazily-formal`][formal] — Lean 4 formal models
- [`lazily-rs`][rs] — Rust reference implementation
- [`lazily-py`][py] — Python implementation
- [`lazily-kt`][kt] — Kotlin/JVM implementation
- [`lazily-js`][js] — TypeScript/Worker implementation
- [`lazily-dart`][dart] — Dart implementation
- [`lazily-zig`][zig] — Zig implementation
- [`lazily-go`][go] — Go implementation

[spec]: https://github.com/lazily-hub/lazily-spec
[formal]: https://github.com/lazily-hub/lazily-formal
[rs]: https://github.com/lazily-hub/lazily-rs
[py]: https://github.com/lazily-hub/lazily-py
[kt]: https://github.com/lazily-hub/lazily-kt
[js]: https://github.com/lazily-hub/lazily-js
[dart]: https://github.com/lazily-hub/lazily-dart
[zig]: https://github.com/lazily-hub/lazily-zig
[go]: https://github.com/lazily-hub/lazily-go
