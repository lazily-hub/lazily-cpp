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

### B — inline value storage  ·  **HIGH**  ·  helps writes + build + scale

Every `set_cell` / cell-creation does `new RcBox<T>` (one heap allocation per
value, even for `CellHandle<int>`). Store trivially-copyable / small values
**inline** in the node (a small-buffer `AnyValue` variant: inline-by-value vs
boxed), boxing only on escape. Expected:
- `set_cell` write throughput up (alloc is a meaningful slice of `set_cell
  high_fan_out 512` = 3.26 µs).
- **Scale build** (1.38 s / 10M cells ≈ 69 ns/node, much of it the per-cell
  `new`) — likely the biggest single build win, ~30–50% if the alloc dominates.
- Less allocator pressure under sustained writes (real-world streaming input /
  telemetry / CRDT apply).

Risk: medium — touches the type-erased `AnyValue` storage (the core abstraction).
Keep the boxing fallback for large/non-trivial `T`. Scope: `rc_ptr.hpp`
(`AnyValue` layout) + `context.hpp` (make/assign sites). **Highest-value remaining lever.**

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
large-graph scale (not single-doc reactive UI) is a real target. **D and B are
the two architectural levers on the in-process core.**

### E — allocation-free batch bookkeeping  ·  **LOW–MEDIUM**  ·  helps batch storms

`batched_cells_` / `scheduled_effects_` / `batched_*` are
`std::unordered_set<SlotId>` → bucket allocations under batch storms. Replace
with a bitset over the node arena (or a flat vector + mark byte): allocation-free,
cache-friendlier. Expected: `batch_storms 64` (4.2 µs) improvement, and helps
sustained batched-write throughput.

Risk: low–medium — localized to the batch path in `context.hpp`. Good first
"engine internals" change if scoping D feels too large.

## Distributed-computing paths (when lazily is the substrate, not just agent-doc)

When lazily powers distributed systems, the bottleneck shifts **off** the
in-process reactive core (where B/D/E live) and **onto** IPC, serialization,
CRDT merge, and cross-process coordination. Prioritize these for distributed
deployments:

- **IPC zero-copy hot path** — extend `ShmBlobArena` usage so hot
  snapshot/delta/CrdtSync messages avoid copy across the FFI/process boundary
  (`ipc.hpp`). Distributed throughput is usually IPC/serialize-bound before the
  reactive core is. **Highest-value for distributed.**
- **CRDT anti-entropy throughput** — `delta_since` / `apply_delta` /
  dotted-frontier merge on `TextCrdt` / `SeqCrdt` / `LosslessTreeCrdt` under
  frequent sync. Benchmark + optimize merge (currently correctness-first).
  Directly affects distributed convergence speed.
- **AsyncContext work-stealing parallel recompute** — `AsyncContext` uses one
  `std::future` per compute (`std::thread` underneath). For independent subgraph
  recomputation (embarrassingly parallel derived slots), a work-stealing thread
  pool would parallelize recompute across cores. New primitive, not a
  regression risk to the sync contexts.
- **NUMA-aware scalable-lock pool** — `ScalableRwLock`'s 128-slot reader pool
  isn't NUMA-aware; on multi-socket boxes a writer's drain scan crosses NUMA
  nodes. Pool-partition by NUMA node if A2 is deployed on big boxes.
- **Write coalescing / backpressure** — `QueueCell` + the command plane for
  distributed pipelines: coalesce rapid writes, apply backpressure under
  downstream saturation. Reliability over raw throughput.

## Sequencing recommendation

1. **B** (inline value storage) — biggest write/build/scale win, unblocks the
   cleanest future A3 if ever needed.
2. **E** (allocation-free batch) — low-risk, do alongside or after B.
3. **IPC zero-copy** + **CRDT merge throughput** — once distributed deployments
   are the target, these dominate over further in-process-core work.
4. **D** (SoA nodes) — only if large-graph scale (≥10M cells) is a real product
   target; otherwise the density win isn't worth the refactor risk.
5. **A3** — only if B is done AND a distributed workload proves
   read-tail-latency-under-writes is a real bottleneck.
