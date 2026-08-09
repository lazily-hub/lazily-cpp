# lazily-cpp on wasm32

`lazily-cpp` builds for Emscripten/wasm32 in **three explicitly-labelled tiers**.
There is no single build that quietly drops features: a header belonging to a
tier your build did not select refuses to compile and says which tier it wants.

| Tier | Contents | Flags | Hosting requirement |
|---|---|---|---|
| **core** | `lazily/core.hpp` — cell kernel, collections, keyed order, statechart, and every family whose closure is standard-library-only | none special | none |
| **threaded** | + `work_queue`, `thread_safe`, `async_context` and the families built on them | `-pthread` | SharedArrayBuffer, so the host must serve `COOP: same-origin` + `COEP: require-corp` |
| **native-only** | `transport`, `reliable_sync`, `ffi` | — | **excluded**; `shm_open`/`mmap`/file-backed storage have no wasm equivalent |

## Getting the toolchain

```sh
git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
cd ~/emsdk && ./emsdk install latest && ./emsdk activate latest
source ~/emsdk/emsdk_env.sh
```

## Building and replaying

```sh
make wasm-core       # configure + build + replay the core tier under Node
make wasm-threaded   # same for the -pthread tier
make wasm            # both, then regenerate and audit the matrix below
make wasm-matrix     # regenerate the matrix from the runtime manifests
```

`make check` is unaffected: the native suite and the wasm tiers share the
library but not a build directory, and `tests/CMakeLists.txt` returns into
`tests/wasm.cmake` only when `EMSCRIPTEN` is set.

## What a consumer should include

Include **`lazily/core.hpp`**. `lazily/lazily.hpp` is the native-only umbrella —
it pulls in `transport.hpp`, and therefore POSIX shared memory. Under
`__EMSCRIPTEN__` the umbrella fails at the offending header with a message
naming the tier, rather than at link time with an undefined symbol.

```cpp
#include <lazily/core.hpp>   // wasm-clean: standard library only

lazily::Context ctx;
auto n = ctx.source<long long>(2);
auto doubled = ctx.computed<long long>([n](lazily::Compute& c) { return n.get(c) * 2; });
```

The freestanding claim is not a promise in prose. `tests/wasm_core_include_check.cpp`
includes `core.hpp` and nothing else and is built for wasm32 on every wasm run,
so a closure that regrows a POSIX or threading dependency fails the build. It
also asserts the umbrella and the native-only headers were *not* pulled in, by
testing their include guards.

## Capability matrix

Generated from the runtime fixture manifests the wasm runs actually produced —
`scripts/check-wasm-tiers.sh` regenerates it and fails when the committed table
differs. **Do not edit this table by hand**; a hand-written matrix records what
someone believed rather than what ran, which is the drift the manifest exists to
catch.

Counts are distinct canonical fixtures from `../lazily-spec/conformance/`
actually opened by a replay in that tier. Every absent cell carries a reason;
there are no blanks.

The table compares the wasm tiers against the **canonical corpus**, not against
what the native suite happens to replay. Comparing to native would make the
matrix depend on a manifest produced by a different CI job — the wasm job does
not build the native suite, so every native cell would read zero there. Against
the corpus the claim is both environment-independent and stronger: every family
the spec ships either replays on a tier or carries a recorded reason.

Two kinds of absence appear, and each reason says which: a family wasm **cannot**
carry (POSIX shared memory, file-backed storage) and a family lazily-cpp does not
replay in **any** target, native included. Those are different facts and the
guard keeps them apart.

<!-- wasm-matrix:start -->
| Family | Corpus | wasm core | wasm threaded | Note |
|---|---:|---:|---:|---|
| `(root)` | 8 | — | — | not on wasm — the top-level snapshot_*/delta_* frames are replayed by the ipc and reliable-sync suites, both native-only |
| `agent-doc` | 2 | — | — | not on wasm — agent-doc session fixtures are not a lazily library family and no binding replays them |
| `codec` | 6 | 6 | — |  |
| `collections` | 21 | 18 | 14 | split across both tiers |
| `coordination` | 5 | 5 | — |  |
| `crdt-tree` | 1 | 1 | — |  |
| `distributed` | 2 | — | — | not on wasm — resolves ShmBlobRef through transport.hpp's ShmBackend — POSIX shm_open/mmap has no wasm implementation |
| `egress` | 4 | — | — | not on wasm — lazily-cpp has no egress runner in ANY target, native included — a binding gap, not a wasm limit |
| `familysync` | 1 | — | 1 | needs -pthread |
| `ingress` | 8 | — | 8 | needs -pthread |
| `lossless-tree` | 9 | 9 | — |  |
| `materialization` | 3 | 3 | 2 | split across both tiers |
| `membership` | 1 | 1 | — |  |
| `message-passing` | 8 | 8 | — |  |
| `presence` | 3 | 3 | — |  |
| `protobuf` | 1 | — | — | not on wasm — lazily-cpp ships no protobuf codec in ANY target, native included — a binding gap, not a wasm limit |
| `rateshape` | 6 | 6 | — |  |
| `reactive-graph` | 21 | — | 21 | needs -pthread |
| `receipts` | 1 | 1 | — |  |
| `reliable-sync` | 9 | — | — | not on wasm — the durable outbox is file-backed (unistd.h/fcntl.h); reliable_sync.hpp refuses to compile under __EMSCRIPTEN__ |
| `resilience` | 4 | 4 | — |  |
| `service` | 4 | 4 | — |  |
| `signaling` | 2 | 1 | — |  |
| `statechart` | 8 | 8 | — |  |
| `stdlib` | 3 | 3 | — |  |
| `temporal` | 4 | 4 | — |  |
| `windowing` | 4 | 4 | — |  |
| **total** | **149** | **89** | **46** | |
<!-- wasm-matrix:end -->

## Known limits

- **The threaded tier costs the consumer something.** `-pthread` requires
  SharedArrayBuffer, which requires cross-origin isolation headers. A page that
  cannot set them can still use the core tier; it cannot use `work_queue`.
- **`transport`/`reliable_sync` are not coming to wasm.** They are POSIX shared
  memory and file-backed durable storage. A wasm equivalent would be a different
  implementation behind the same seam, not a port — the `BlobBackend` interface
  is where that would plug in.
- **`ffi.hpp` has no wasm consumer.** The C-ABI shared library exists so a host
  process can `dlopen` lazily; a wasm consumer links the module directly.
- Benchmarks live in `BENCHMARKS.md` under their own section. Native `-O3` and
  wasm numbers are not comparable and deliberately do not share a table.
