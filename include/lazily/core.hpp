// `lazily/core.hpp` — the freestanding reactive core (`#lzcppwasm` Phase 1).
//
// This is the narrow-include contract, named. It pulls in the reactive kernel
// and the data structures built directly on it, and **nothing that touches the
// operating system**: no threads, no shared memory, no sockets, no files. Its
// entire transitive closure is the C++17 standard library.
//
// ## Why this header exists
//
// `lazily.hpp` is the umbrella and includes everything, which means a consumer
// who reaches for the obvious header also gets `transport.hpp` — and with it
// `shm_open`/`mmap`/`unistd.h`. On a hosted Linux target that is invisible. On
// wasm32, a freestanding target, or a build that simply does not want POSIX
// shared memory linked in, it is a wall.
//
// The split already existed at *file* granularity: `cell.hpp`'s closure has
// always been clean. What did not exist was a *name* for it, so "include
// narrowly" was tribal knowledge that each consumer had to rediscover by
// reading include graphs. This header is that name.
//
// ## What is in it
//
//   cell.hpp          Source<T, M> / Computed<T> / Effect — the kernel
//   collections.hpp   ReactiveMap and friends
//   keyed_order.hpp   KeyedOrder<K, H> — present set + authoritative key order
//   statechart.hpp    hierarchical statecharts over the kernel
//
// Twelve headers transitively (the four above plus `context`, `merge`,
// `rc_ptr`, `reactive_family`, `small_any`, `small_fn`, `small_vec`, `types`).
//
// ## What is deliberately NOT in it, and why
//
//   work_queue.hpp / thread_safe.hpp / async_context.hpp
//       <thread>, <mutex>, <condition_variable>, <future>. These work on wasm
//       but only under `-pthread`, which obliges the consumer to serve
//       COOP/COEP headers and have SharedArrayBuffer. That is a hosting
//       decision, not a library default — so it is a separate tier, not a
//       silent dependency.
//
//   transport.hpp / reliable_sync.hpp
//       ::shm_open, mmap, unistd.h, fcntl.h. There is no meaningful wasm
//       implementation of POSIX shared memory. Including these under
//       __EMSCRIPTEN__ is a hard preprocessor error rather than an obscure
//       link failure — see the guard at the top of each of those headers.
//
// `<atomic>` *is* present, via `rc_ptr.hpp`. That is not a threading
// dependency: `<atomic>` compiles and works in single-threaded wasm without
// `-pthread`. Only `<thread>` and friends need the pthread build.
//
// ## How the claim is kept honest
//
// A grep over include lines proves only what is written. This header's
// wasm-cleanliness is proven by *compiling* it: `make wasm-core` builds
// `tests/wasm_core_include_check.cpp` — which includes this header and nothing
// else — with `emcc` for wasm32, and `scripts/check-wasm-tiers.sh` asserts the
// resulting module imports nothing from the POSIX filesystem or threading
// layer. A closure that regressed would fail to build rather than fail to be
// noticed.
//
// See `WASM.md` for the build tiers and the replayed capability matrix.

#ifndef LAZILY_CORE_HPP
#define LAZILY_CORE_HPP

#include <lazily/cell.hpp>
#include <lazily/collections.hpp>
#include <lazily/keyed_order.hpp>
#include <lazily/statechart.hpp>

// Marks a translation unit as having taken the narrow-include contract, so a
// test can assert it compiled against `core.hpp` rather than the umbrella.
#define LAZILY_CORE 1

#endif // LAZILY_CORE_HPP
