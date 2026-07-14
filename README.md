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
[`lazily-spec` § Cross-Language Coverage](https://github.com/lazily-hub/lazily-spec/blob/main/docs/coverage.md).

<!-- coverage-table:start -->
| Feature | Rust | Python | Kotlin | JS | Dart | Zig | Go | C++ |
| --------- | :----: | :------: | :------: | :--: | :----: | :---: | :--: | :---: |
| Reactive graph — core `Cell` / `Slot` / `Effect` (+ derived `Signal` = `Slot.eager`) / memo / batch | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Keyed-map materialization (`SlotMap`) — mint-on-access derived slots: transparency + deferral (`#lzmatmode`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Thread-safe keyed map (`ThreadSafeSlotMap`) — `Send + Sync` + materialization confluence (`#lzmatmode`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Async keyed map (`AsyncSlotMap`) — eventual transparency (`#lzmatmode`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Keyed-map sync — membership propagation + materialize-on-ingest + derived-aggregate transparency (`#lzfamilysync`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Thread-safe context (lock-backed) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Async reactive context | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Flat state machine | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Harel state charts | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Keyed reactive maps (`ReactiveMap`: `CellMap` / `SlotMap`) + `CellTree` + reconcile | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Memoized semantic tree (`SemTree`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Stable-id alignment (manufactured identity) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Reactive queue (`QueueCell` SPSC/MPSC + `QueueStorage` adapter) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Broadcast topic (`TopicCell`) — independent cursors + durable replay + safe GC (`#lztopiccell`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Competing-consumer work queue (`WorkQueueCell`) — exclusive leases + ack/nack + redelivery + DLQ (`#lzworkqueue`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Merge algebra + `MergeCell` — associative `MergePolicy` (`KeepLatest`/`Sum`/`Max`/`SetUnion`/`RawFifo`), `Cell ≡ MergeCell<KeepLatest>`, `Reactive`/`Source` split (`#relaycell`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| RelayCell — conflating relay + `BackpressurePolicy` + `SpillStore` + `Transport` + Inbox/Outbox + Rate/Window/Expiry/Priority/keyed policies (`#relaycell`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Free-text character CRDT (`TextCrdt`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| `TextCrdt` delta sync (`version_vector` / `delta_since` / `apply_delta`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| `CrdtTree` lossless document contract (`#lzcrdttree`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Move-aware sequence CRDT (`SeqCrdt`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Lossless tree CRDT core (`LosslessTreeCrdt`, M1) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Lossless tree — dotted-frontier anti-entropy | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Lossless tree — concurrent merge convergence | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Registers (LWW / MV) + `PnCounter` + `CellCrdt` | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| IPC wire — `Snapshot` + `Delta` + `CrdtSync` | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Shared-memory blob path (`ShmBlobArena`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Cross-process zero-copy transport (`BlobBackend` / shm / arrow) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Distributed CRDT plane (`CrdtPlaneRuntime` / anti-entropy) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Reliable sync — resync coordinator + at-least-once durable outbox + OR-set/LWW liveness (`#lzsync`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Storage-independent durable outbox (`OutboxStore` + shared outbox protocol; SQLite/Room/IndexedDB/file adapters) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Reliable-sync transport seam + full-duplex `SyncDriver` loop (`IpcSink`/`IpcSource`, `#sync-driver`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Distributed plane — WebRTC transport + signaling | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| State projection / mirror | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Causal receipts (`CausalReceipts` outcome projection) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Message-passing + RPC command plane (`command-plane-v1`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| C-ABI FFI boundary | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Permission boundary (`PeerPermissions` / `RemoteOp`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Capability negotiation (`SessionHandshake`) | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Instrumentation / benchmarks | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| Temporal sources — `TimerCell` / `IntervalCell` / `CronCell` / `DeadlineCell` over a logical clock (`#lztime`) | ✅ | — | ✅ | ✅ | — | — | — | — |
| Rate-shaping operators — `DebounceCell` / `ThrottleCell` / `SampleCell` / `ProbabilisticSampleCell` (`#lzrateshape`) | ✅ | — | ✅ | ✅ | — | — | — | — |
| Membership + failure detection — `MembershipCell` (SWIM + Phi-accrual) / `PeerSet` / `PeerChangeEvent` (`#lzmemb`) | ✅ | — | ✅ | ✅ | — | — | — | — |
| Distributed coordination — `LeaseCell` / `LeaderCell` / `LockCell` / `SemaphoreCell` / `BarrierCell`+`QuorumCell` (`#lzcoord`) | ✅ | — | ✅ | ✅ | — | — | — | — |
| Presence + ephemeral plane — `PresenceCell` / `AwarenessCell` / `EphemeralCell` + `Ephemeral`/`Durable` markers (`#lzpresence`) | ✅ | — | ✅ | ✅ | — | — | — | — |
| Stream windowing — `TumblingWindow` / `SlidingWindow` / `SessionWindow` over the merge algebra (`#lzwindow`) | ✅ | — | ✅ | ✅ | — | — | — | — |
| Fault tolerance — `CircuitBreakerCell` / `RetryPolicyCell` / `BulkheadCell` / `TimeoutCell` (`#lzresilience`) | ✅ | — | ✅ | ✅ | — | — | — | — |
| Embedded-service plane — `HealthCell` / `ReadinessCell` / `DiscoveryCell` / `ServiceRegistry` (`#lzservice`) | ✅ | — | ✅ | ✅ | — | — | — | — |
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

**Reliable sync (v0.10.0, `#lzsync`):** delivery-reliability over the
`Snapshot`/`Delta`/`CrdtSync` planes — `<lazily/reliable_sync.hpp>`. Three pure
pieces (no I/O, clock, or storage engine): a `ResyncCoordinator` (gap detection →
`ResyncRequest`, multi-epoch-span deltas, idempotent re-delivery for exactly-once
effect), a `DurableOutbox`/`InMemoryOutbox` (append-before-send, replay-from-cursor
on reconnect for at-least-once delivery), and `OrSet`/`WireLwwRegister` liveness
cells on the CRDT plane. The full-duplex `SyncDriver` composes them over a
caller-supplied `IpcSink`/`IpcSource`/`Clock`/`SnapshotProvider` seam — the host
owns threads, cadence, and backoff. `ResyncRequest`/`OutboxAck` are two new
`IpcMessage` variants (FFI kinds 4/5). Pinned by
`lazily-spec/conformance/reliable-sync/` and `lazily-formal` `ReliableSync.lean`.

**Document/outbox parity (v0.16.1):** `CrdtTree<T>` is the C++17 structural
lossless-document contract implemented by `TextCrdt`. `StoredOutbox<Store>` is
the single byte-store-independent acknowledgement/prune/replay protocol;
`InMemoryOutbox` now uses it, and `FileOutbox` supplies a locked append-only
journal whose persisted cursor folds with `max`, including across stale handles.

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

### Keyed reactive collections (`ReactiveMap`)

`ReactiveMap<K, V, H>` is the one keyed primitive: it maps keys to per-entry
reactive nodes of a single handle kind — `CellHandle<V>` (input cells) or
`SlotHandle<V>` (derived slots) — with **reactive membership and order**. It has
two specializations (`#reactivemap`):

- **`CellMap<K, V>`** — input-cell entries. Adds cell-only `set` and eager
  value-minting (`entry` / `entry_with`).
- **`SlotMap<K, V>`** — derived-slot entries. `get_or_insert_with` mints a slot on
  first access (**lazy materialization**); `materialize_all` pre-mints the keyset
  (**eager**). A slot's value is derived, so `SlotMap` has no `set`. There is no
  eager/lazy mode flag — lazy is mint-on-access, eager is the pre-mint loop.

```cpp
lazily::Context ctx;
lazily::SlotMap<uint32_t, uint32_t> slots(ctx);

// Lazy: a slot is minted on first access ("materialize on pull").
assert(slots.present_count() == 0);
assert(slots.get_or_insert_with(ctx, 5, [](const uint32_t& k) { return k * 3; }) == 15);
assert(slots.present_count() == 1);

// Eager: pre-mint the whole keyset up front — observationally identical.
lazily::SlotMap<uint32_t, uint32_t> eager(ctx);
eager.materialize_all(ctx, {0, 1, 2}, [](const uint32_t& k) { return k * 3; });
assert(eager.present_count() == 3);
```

Minting is **observationally transparent** — a value read is identical whether the
entry was pre-minted or minted on access; eager only changes allocation timing and
memory. Membership is tracked by a dedicated version cell, so `len` / `keys`
readers recompute only on add/remove (or reorder for `keys`), never on a per-entry
value change. The contract is proved in lazily-formal (`Materialization.lean`) and
exercised against the shared lazily-spec `conformance/materialization/*` fixtures.
The `Send + Sync` (`ThreadSafeReactiveMap`) and async (`AsyncReactiveMap`) flavors
carry the same `CellMap` / `SlotMap` specializations.

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

### Competing-consumer work queue

`WorkQueueCell<T>` provides exclusive FIFO claims with visibility deadlines,
worker-scoped acknowledgements, tail retries, and bounded dead-letter handling.
Item ids remain stable across retries while every claim receives a fresh
delivery id. Its four reactive readers invalidate independently.

```cpp
lazily::WorkQueueCell<std::string> work(ctx, 10, 3);
work.push(ctx, "job");
auto delivery = work.claim(ctx, "worker-a", 100).value();
assert(work.ack(ctx, "worker-a", delivery.delivery_id));
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
| `collections.hpp` | CellTree, keyed reconciliation (LIS) (re-exports `CellMap` / `SlotMap` / `ReactiveMap`) |
| `reactive_family.hpp` | `ReactiveMap<K, V, H>` — unified keyed cell/slot collection with reactive membership + order; `CellMap` (`set` + eager `entry`) and `SlotMap` (`get_or_insert_with` lazy mint / `materialize_all` eager pre-mint) specializations (`#reactivemap`) |
| `thread_safe_reactive_family.hpp` | `ThreadSafeReactiveMap` — `Send + Sync` keyed collection over ThreadSafeContext (mutex-guarded present set); `ThreadSafeCellMap` / `ThreadSafeSlotMap` (`#reactivemap`) |
| `async_reactive_family.hpp` | `AsyncReactiveMap` — keyed collection over AsyncContext (`observe` → `std::optional<V>`, eventual transparency); `AsyncCellMap` / `AsyncSlotMap` (`#reactivemap`) |
| `queue.hpp` | QueueCell (SPSC/MPSC reactive queue) + QueueStorage adapter + VecDequeStorage |
| `work_queue.hpp` | WorkQueueCell competing-consumer claims, leases, retries, and dead letters |
| `relay.hpp` | RelayCell conflating relay + `BackpressurePolicy` + `SpillStore` + `Transport` (InProc/Framed) + Outbox/Inbox roles + Rate/Window/Expiry/Priority/`KeyedRelay` policies (`#relaycell`) |
| `sem_tree.hpp` | Memoized semantic tree (incremental fold, memo equality guard) |
| `thread_safe.hpp` | `BasicThreadSafeContext<Policy>` — `ThreadSafeContext` (recursive_mutex, default) + `RwThreadSafeContext` (shared_mutex) + `ScalableThreadSafeContext` (reader-scalable lock) |
| `async_context.hpp` | AsyncContext (Empty/Computing/Resolved/Error lifecycle) |
| `hlc.hpp` | Hybrid logical clock, StampFrontier |
| `crdt.hpp` | TextCrdt (+ delta sync), SeqCrdt, LwwRegister, MvRegister, PnCounter |
| `crdt_tree.hpp` | C++17 `CrdtTree<T>` structural contract (`#lzcrdttree`) |
| `lossless_tree_crdt.hpp` | LosslessTreeCrdt (M1, dotted-frontier anti-entropy) |
| `stable_id.hpp` | Manufactured identity (anchors, content hashes, word-LCS alignment) |
| `ipc.hpp` | IPC wire types (Snapshot/Delta/CrdtSync), NodeKey, ShmBlobArena, PeerPermissions, CapabilityHandshake |
| `codec.hpp` | msgpack wire codec — `encode`/`decode` the `IpcMessage` tree |
| `msgpack.hpp` | Minimal zero-dependency MessagePack packer/unpacker |
| `reliable_sync.hpp` | Reliable sync plus `OutboxStore`, `StoredOutbox<S>`, `InMemoryStore`, and locked `FileOutboxStore` |
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
