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
built on `std::thread`, `std::mutex`, and `std::future` / coroutines where
available.

## The reactive family

- **Slot** — a lazily-computed cached value that automatically tracks its
  dependencies and recomputes only when read after an upstream change.
- **Cell** — a mutable source value that invalidates dependent Slots/Signals
  when it changes.
- **Signal** — an *eager* derived value that recomputes the instant a dependency
  changes, with no intermediate unset value.

Values are **lazy by default**. When you need eager push-style semantics, reach
for `Signal`. Use `Effect` for side effects and `Memo` for an equality-guarded
derived value.

## Usage

```cpp
#include <lazily/lazily.hpp>

lazily::Context ctx;
auto a = lazily::Cell<int>::make(ctx, 2);
auto b = lazily::Cell<int>::make(ctx, 3);
auto sum = lazily::Slot<int>::make(ctx, [&] { return a->get() + b->get(); });

assert(sum->get() == 5);
a->set(10);
assert(sum->get() == 13);
```

## Build

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Feature coverage

Conformant with the [`lazily-spec`][spec] binding conformance matrix. See the
coverage table in [`lazily-spec`][spec] for the cross-language feature matrix.

[spec]: https://github.com/lazily-hub/lazily-spec
[formal]: https://github.com/lazily-hub/lazily-formal
[rs]: https://github.com/lazily-hub/lazily-rs
[py]: https://github.com/lazily-hub/lazily-py
[kt]: https://github.com/lazily-hub/lazily-kt
[js]: https://github.com/lazily-hub/lazily-js
[dart]: https://github.com/lazily-hub/lazily-dart
[zig]: https://github.com/lazily-hub/lazily-zig
[go]: https://github.com/lazily-hub/lazily-go
