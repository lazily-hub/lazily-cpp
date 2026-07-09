# lazily-cpp Performance Roadmap

Status of the high-load optimization arc and the recommended next paths. See
[`BENCHMARKS.md`](BENCHMARKS.md) for measured numbers and
[`tasks/software/plan-lazily-rs-lock-free-context.md`](../lazily-rs) (in the
workspace) for the lock-free evaluation that governs the A*/CAS gates.

## Shipped

| Version | Optimization | Result |
|---|---|---|
| v0.3.0 | **C** — non-atomic compute/effect closures + contention-throughput harness + `RcPtr::adopt_t` fix | `cold_full_recalc` ~28% faster, `full_recalc` ~2× faster (10M); first real contention baseline exposed the single-mutex load cliff |
| v0.4.0 | **A** — opt-in `RwThreadSafeContext` (`shared_mutex`) | Cached reads scale ~2.6× at 16 threads (plateau: shared_mutex serializes shared acquires on one atomic) |
| v0.5.0 | **A2** — opt-in `ScalableThreadSafeContext` (`ScalableRwLock`, per-cacheline reader counters) | Cached reads scale **near-linearly**: 58 → 925 Mops/s (1→16 threads), ~73× the A plateau; TSan-clean |
| v0.6.0 | **B** — `SmallAny` inline value storage (zero per-value alloc for small POD) + **E** — alloc-free batch bookkeeping | `cold_full_recalc` ~3× faster (1M: 36 ms; 10M: 415 ms, ~41 ns/formula); `batch_storms 64` ~2.7× faster (1.55 µs); build flat (now closure-alloc-bound) |

The default `ThreadSafeContext` (recursive mutex) is byte-for-byte unchanged
across all of the above — every read-scaling policy is opt-in, so existing users
see zero regression.

## Analyzed, not pursued

### A3 — true lock-free cached reads (atomic-publish + hazard/epoch reclamation)

**Expected gain over A2: ~20–30% higher read ceiling only.** A2 already scales
linearly (per-thread throughput is flat 1→16 threads = zero contention left to
remove), so lock-free can only shave the per-op constant (~5 ns of seq_cst
handshake → read pair 17 ns → ~12 ns). Ceiling ~925 → ~1.2 GMops/s.

The more interesting *unquantified* benefit is **read tail-latency under
concurrent writes** (A2 readers stall during a writer's drain; A3 readers never
block) — relevant for CRDT anti-entropy bursts in distributed use, but needs a
mixed read/write p99 benchmark to prove.

**Why deferred:** fails the project's own adoption gate (≥20% target gain AND
≤5% regression elsewhere). The ~30% read gain clears the target bar, but A3
needs either a per-node heap atomic-publish pointer (nodes can't hold atomics —
they must stay movable) or Arc values — both **regress build/scale and directly
conflict with optimization B**. **Do not start A3 until B lands AND a real
distributed workload shows read-tail-latency-under-writes is a bottleneck.**

Path (if ever taken): parallel atomic-publish index + hazard-pointer reclamation
(keep non-atomic `Rc` values; readers hazard-protect + copy `T`, no refcount inc
→ no refcount contention; writers retire boxes, hazard-aware free). Proof
obligations in `plan-lazily-rs-lock-free-context.md` (linearization point,
memory ordering, ABA, reclamation safety). Validate under TSan + a model checker.

## Recommended next paths (priority order)

> ✅ **B** (inline value storage) and **E** (alloc-free batch bookkeeping)
> shipped in v0.6.0 — see the table above. Remaining:

### Distributed-computing paths — measure-first (partially done)

When lazily powers distributed systems, the bottleneck shifts **off** the
in-process reactive core and **onto** IPC, serialization, CRDT merge, and
cross-process coordination. v0.6.x measured both hot paths and landed two
optimizations:

- ✅ **CRDT delta sync** — `elems_` / `by_origin_` moved from `std::map` to
  `std::unordered_map` (OpId was already hashable; ordering unused). `apply_delta`
  @10k **2.5× faster** (1.99 ms → 0.79 ms); `version_vector` 2.2×; `delta_since`
  1.8×. Measured via the new `crdt_sync` benchmark.
- ✅ **ShmBlobArena read** — checksum was recomputed (full FNV-1a) on every read;
  now cached at write (payload is immutable). `read` @64KB **74× faster**
  (49 µs → 0.67 µs). Measured via the new `shm_blob` benchmark.

Remaining distributed work (measured, queued):

- ✅ **msgpack wire codec** (`include/lazily/msgpack.hpp` + `codec.hpp`) — the
  foundational serialization layer lazily was missing (`kDefaultCodec` was a
  stub). Zero-dep hand-rolled MessagePack; self-describing maps (forward-compat
  skip-unknown-key); round-trip tests for every `IpcMessage` variant branch.
  Chosen over protobuf (schema-less flexibility) and capnproto (not needed).
  Benchmark: ~1.7 GB/s encode / ~0.5 GB/s decode at 619 KB. Unblocks cross-wire.
- ✅ **ShmBlobArena zero-copy read** — `read_view(ref)` returns a `const
  std::vector<uint8_t>*` into the immutable cached payload with no copy and no
  checksum work (constant ~16 ns vs 0.58 µs copy at 64 KB → 35×). `read` (copy)
  kept for callers needing an owning vector. Completes the checksum-cache work;
  the IPC zero-copy chain now has its read-side primitive.
- ✅ **TextCrdt insertion O(n²) → O(n)** — lazily-rebuilt `visible_order_` cache;
  local `insert`/`del`/`visible_len` are O(1) lookup + amortized splice (O(1)
  push_back for append). Remote ops invalidate; rebuild via `traverse`. 100k-cell
  build now completes (previously hung). Pathological mid-inserts still O(n)/op
  (vector splice) — O(log n) order-statistics tree deferred until measured.
- **IPC zero-copy across the FFI/process boundary** — with the codec + checksum
  cache + `read_view` landed, the remaining work is the transport: pass a
  `ShmBlobRef` descriptor across processes (over the msgpack codec) instead of
  copying `IpcValueInline` bytes. All primitives are now in place.
- **Codec compactness / decode allocation** — string-keyed maps are ~62 B/node;
  a positional-array encoding (still msgpack, schema-versioned) would be ~2–3×
  smaller. Decode does per-key `std::string` / per-bin `std::vector` allocation
  (string-view keys / in-place decode would speed it). Quantified by the
  `codec` benchmark.
- **SeqCrdt / LosslessTreeCrdt merge throughput** — benchmark + optimize
  `delta_since` / `apply_delta` / dotted-frontier merge on the other CRDTs
  (currently correctness-first; the TextCrdt `unordered_map` + `visible_order_`
  changes are a template for them).

### D — per-kind node vectors (SoA)  ·  **MEDIUM**  ·  helps large-graph scale

Nodes are `std::vector<std::optional<std::variant<SlotNode, CellNode,
EffectNode>>>` — every node is sized to the largest alternative (`SlotNode`), so
`CellNode`/`EffectNode` waste space, and the `optional`+`variant` doubles tag
overhead. Split into per-kind arrays (or SoA fields): improves cache density and
cuts RSS at scale (the 10M-cell case runs ~5 GB). Expected ~10–20% on
large-graph build/cold-recalc (memory-bandwidth-bound), invisible to small
graphs / micro-benchmarks.

Risk: high — large refactor of the engine's node storage and every
`get_slot_node`/`get_cell_node` accessor. Gate behind benchmarks; do only if
large-graph scale (not single-doc reactive UI) is a real target. Now that B
lowered per-cell value RSS, D's density win is smaller than originally
estimated — re-measure before committing.

### E (done) / B (done)

Shipped v0.6.0. See the Shipped table. (Kept here as historical anchors for the
sequencing rationale and the closure-alloc note under B: build is now
closure-bound, so the remaining build gap vs lazily-rs needs inline closures —
low payoff since typical closures exceed the inline buffer.)

### AsyncContext / NUMA / backpressure (lower priority)

- **AsyncContext work-stealing parallel recompute** — `AsyncContext` uses one
  `std::future` per compute (`std::thread` underneath). For independent subgraph
  recomputation, a work-stealing thread pool parallelizes recompute across
  cores. New primitive, no regression risk to the sync contexts.
- **NUMA-aware scalable-lock pool** — `ScalableRwLock`'s 128-slot reader pool
  isn't NUMA-aware; on multi-socket boxes a writer's drain scan crosses NUMA
  nodes. Pool-partition by NUMA node if A2 is deployed on big boxes.
- **Write coalescing / backpressure** — `QueueCell` + the command plane for
  distributed pipelines: coalesce rapid writes, apply backpressure under
  downstream saturation. Reliability over raw throughput.

## Sequencing recommendation

1. ~~**B**~~ ✅ v0.6.0 + ~~**E**~~ ✅ v0.6.0.
2. ~~**Distributed measure-first**~~ ✅ (CRDT `unordered_map` + ShmBlob checksum
   cache + **msgpack wire codec** + ShmBlob `read_view` zero-copy + TextCrdt
   O(n²)→O(n) all landed; benchmarks added). **Remaining distributed work:** the
   IPC cross-process zero-copy transport (all primitives now in place — pass
   `ShmBlobRef` over the msgpack codec instead of inline bytes) → then
   SeqCrdt/LosslessTree merge. (Codec compactness / decode-alloc + CRDT
   order-statistics tree are optional later levers, gated on measurement.)
3. **D** (SoA nodes) — only if re-measured large-graph scale (≥10M cells) is a
   real product target; B reduced its payoff.
4. **A3** — only if a real distributed workload proves read-tail-latency-under-
   writes is a bottleneck (the memory-safety risk of hazard/epoch reclamation
   remains the gating concern).
