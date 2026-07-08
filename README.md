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
| Thread-safe context (lock-backed) | ✅ | ✅ | ✅ | — | — | ✅ | ✅ | ✅ |
| Async reactive context | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Flat state machine | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Harel state charts | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Keyed cell collections (`CellMap` / `CellTree`) + reconcile | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Memoized semantic tree (`SemTree`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Stable-id alignment (manufactured identity) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Reactive queue (`QueueCell` SPSC/MPSC + `QueueStorage` adapter) | — | — | — | — | — | — | — | — |
| Free-text character CRDT (`TextCrdt`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| `TextCrdt` delta sync (`version_vector` / `delta_since` / `apply_delta`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Move-aware sequence CRDT (`SeqCrdt`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Lossless tree CRDT core (`LosslessTreeCrdt`, M1) | ✅ | ✅ | ✅ | ✅ | ✅ | — | ✅ | ✅ |
| Lossless tree — dotted-frontier anti-entropy | ✅ | ✅ | ✅ | ✅ | ✅ | — | ✅ | ✅ |
| Lossless tree — concurrent merge convergence | ✅ | ✅ | ✅ | ✅ | ✅ | — | ✅ | ✅ |
| Registers (LWW / MV) + `PnCounter` + `CellCrdt` | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| IPC wire — `Snapshot` + `Delta` + `CrdtSync` | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Shared-memory blob path (`ShmBlobArena`) | ✅ | ✅ | ✅ | ~ | ~ | ✅ | ✅ | ✅ |
| Distributed CRDT plane (`CrdtPlaneRuntime` / anti-entropy) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Distributed plane — WebRTC transport + signaling | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| State projection / mirror | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Causal receipts (`CausalReceipts` outcome projection) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Message-passing + RPC command plane (`command-plane-v1`) | ✅ | ✅ | ✅ | ✅ | ✅ | — | ✅ | ✅ |
| C-ABI FFI boundary | ✅ | ✅ | ✅ | — | ✅ | ✅ | ✅ | ✅ |
| Permission boundary (`PeerPermissions` / `RemoteOp`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Capability negotiation (`SessionHandshake`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Instrumentation / benchmarks | ✅ | ✅ | ✅ | — | ✅ | ✅ | ✅ | ✅ |
<!-- coverage-table:end -->

CRDT convergence and the wire protocol are pinned by the shared conformance fixtures
and JSON Schemas in `lazily-spec` and the Lean models in `lazily-formal`.

## Benchmark highlights

Micro-benchmarks on `x86_64` with GCC 16, C++17. Full results in
[BENCHMARKS.md](BENCHMARKS.md).

| Benchmark | Context | ThreadSafeContext |
|---|---:|---:|
| cached read | 304 ns | 411 ns |
| cold first get | 2.52 us | 2.79 us |
| fan-out 256 | 88 us | 94 us |
| set_cell high_fan_out 512 | 125 us | — |
| memo equality suppression | 988 ns | 912 ns |
| batch storms 64 | 63 us | 63 us |

**Scale (1M cells, 2M nodes):** build ~1.28 s, cold recalc ~1.09 s, viewport
recalc (edit 1, read 1k) **~439 us** — independent of sheet size thanks to the
lazy pull-based model.

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

## Architecture

- **Context owns all nodes** in a `std::vector<std::optional<Node>>` indexed by
  `SlotId` (uint64_t) — cache-friendly, allocation-light, no hash probes on the
  read path.
- **Lightweight Copy handles** (`SlotHandle<T>`, `CellHandle<T>`,
  `EffectHandle`, `SignalHandle<T>`) are just ids — all data lives in the
  Context.
- **Type erasure** via `std::shared_ptr<void>` + `std::type_index` — the
  Context stores heterogeneous node types in a single `std::variant`.
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
cells), and a cross-language comparison with lazily-rs.

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
| `state_machine.hpp` | Flat state machine (Cell-backed FSM) |
| `statechart.hpp` | Full Harel/SCXML state charts (compound, parallel, history, actions, guards) |
| `collections.hpp` | CellMap, CellFamily, CellTree, keyed reconciliation (LIS) |
| `sem_tree.hpp` | Memoized semantic tree (incremental fold, memo equality guard) |
| `thread_safe.hpp` | ThreadSafeContext (recursive_mutex-wrapped Context) |
| `async_context.hpp` | AsyncContext (Empty/Computing/Resolved/Error lifecycle) |
| `hlc.hpp` | Hybrid logical clock, StampFrontier |
| `crdt.hpp` | TextCrdt (+ delta sync), SeqCrdt, LwwRegister, MvRegister, PnCounter |
| `lossless_tree_crdt.hpp` | LosslessTreeCrdt (M1, dotted-frontier anti-entropy) |
| `stable_id.hpp` | Manufactured identity (anchors, content hashes, word-LCS alignment) |
| `ipc.hpp` | IPC wire types (Snapshot/Delta/CrdtSync), NodeKey, ShmBlobArena, PeerPermissions, CapabilityHandshake |
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
