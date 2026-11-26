# lazily-cpp

C++ port of the lazily reactive family, built on the **Cell kernel**
(`#lzcellkernel`): two concrete handles — `Source<T, M>` (written from outside,
folds under merge policy `M`) and `Computed<T>` (computed from upstream, guarded
by default) — plus value-less `Effect` sinks outside the hierarchy. `Cell` is a
*concept* (a value-bearing node), not a type. Ships the full lazily-spec wire
protocol, CRDT collection types, the lossless tree CRDT, and the command/RPC
message plane.

## Reactive kernel (`#lzcellkernel`, v2)

The public reactive surface is `include/lazily/cell.hpp`:

- **Two concrete handles.** `Source<T, M = KeepLatest>` and `Computed<T>` — no
  `Cell<T, K>` genus, no `Source<M>`/`Formula` kind markers. `M` is `Source`'s
  own policy param; `Source ≡ Source<KeepLatest>`.
- **Constructors on `Context`.** `source(v)` (keep-latest input),
  `source_with<M>(v)` (folding input, formerly `merge_cell<M>`), `computed(f)`
  (guarded; renamed from `formula`, folds in the former `memo`). Reads: `get`.
  Writes: `set`/`merge`.
- **Guarded by default, always (§9.3, DECIDED 2026-07-21).** Every cell is
  guarded — no unguarded mode. `Source` suppresses an equal write; `Computed`
  suppresses an equal recompute (TC39 `Signal.Computed`). The `memo` constructor
  is removed (folded into `computed`).
- **Write protection is a compile error, not a runtime gate (§3/§4).** `set` and
  `merge` exist ONLY on `Source<T, M>`, so `computed.set(...)` fails to compile
  ("no member named 'set'"). Locked by `has_set<>` `static_assert`s in
  `tests/test_cell_kernel.cpp` and the WILL_FAIL build
  `tests/compile_fail_formula_set.cpp`.
- **Eager = an eager `Computed`, not a `Signal` kind (§9.3).**
  `computed(f).eager()` attaches a puller `Effect`; eagerness is an `eager` bit
  on the node plus an `eager_by_` side table in `Context`, cleared on
  `.lazy()`/dispose (`is_eager()` queries it). Because the puller is an ordinary
  scheduled effect, batched invalidations coalesce into one recompute — the
  `#lzsignaleager` per-write-puller defect is structurally unwritable.
- **`Slot` is the STORAGE sense only (§5.0).** `SlotId`, `SlotNode`, the arena
  free-list, and the wire `SlotValue` are unchanged — a slot is the position that
  holds a node of any kind. Only the reactive-VALUE sense of "slot" became
  `Computed`.

Engine substrate (kept, not the public vocabulary): `SlotId`, `SlotNode`, the
arena/free-list, and the wire `SlotValue` storage name. The removed
`CellHandle<T>` / `SlotHandle<T>` value-handle spellings and their old
constructors are not an alternate lower-level API; production code uses
`Source<T, M>` / `Computed<T>` / `Effect`. `AsyncContext` remains a separate
incomplete plane and is tracked by the async-v2 migration plan.

## Build tiers and wasm (`#lzcppwasm`)

`include/lazily/core.hpp` is the **narrow-include contract**: the cell kernel,
collections, keyed order, and statechart, whose entire transitive closure is the
C++17 standard library. Include it from anything that must not depend on POSIX.

`include/lazily/lazily.hpp` is the **native-only umbrella**. It includes
`transport.hpp` (POSIX `shm_open`/`mmap`) and `reliable_sync.hpp` (file-backed
outbox), so it is not usable on wasm32 and both of those headers refuse to
compile under `__EMSCRIPTEN__` with a message naming the tier.

Three tiers: **core** (no flags), **threaded** (`-pthread`, needs
SharedArrayBuffer + COOP/COEP from the host), **native-only** (excluded).
Assignment lives in `wasm-tiers.conf`, which is the single source of truth read
by both `tests/wasm.cmake` and `scripts/check-wasm-tiers.sh`.

When adding a test or benchmark, include the families it needs **narrowly**
rather than reaching for the umbrella — otherwise it silently becomes
native-only. `make wasm-core` / `make wasm-threaded` build and replay the
conformance suite under Node; `make wasm` also audits the WASM.md matrix, which
is generated from the runtime fixture manifests and must never be hand-edited.
See `WASM.md`.

## Commit & Push

Commit and push completed work at the end of every turn that changed code,
tests, docs, or fixtures — do not leave finished work uncommitted. Run `make
check` first and ensure it is green; stage only the files that belong to the
change (never secrets or private customer names — see the workspace
`runbooks/private-name-hygiene.md`); write a concise commit message in the
repo's existing style; push to the current branch on `origin`. This standing
rule overrides the harness default of "commit only when explicitly asked" for
this repo.

<!-- tsift:code-navigation v=0.1.110 -->
## Code Navigation

Use the repository tsift skill at [`.agents/skills/tsift/SKILL.md`](.agents/skills/tsift/SKILL.md) for code search, source reading, call graphs, diffs, logs, and test output.
<!-- /tsift:code-navigation -->
