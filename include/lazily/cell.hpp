// The Cell kernel (`#lzcellkernel`) — the two concrete handle templates
// `Source<T, M>` and `Computed<T>`.
//
// See `tasks/software/lazily-cell-kernel-design.md`. **`Cell` is a conceptual
// word, not a type**: a *cell* is a value-bearing reactive node, and the two
// kinds of cell are named by the two handle structs a caller holds:
//
//   Source<T, M>      handle to a source cell — written from outside; folds under policy M
//   Computed<T>       handle to a computed cell — computed from upstream (guarded)
//
//   Effect            no value; a sink — outside the hierarchy
//
// Both answer the same question — *where does a node's value come from* — so
// the pair is exhaustive: `Source` from outside, `Computed` from upstream.
// `Effect` stays outside the hierarchy (a sink, no value). There is **no
// `Cell<T, K>` genus struct**: the former genus dissolves into these two
// concrete templates, and the former `Source<M>` / `Formula` *kind markers*
// are gone — `M` is now `Source`'s own policy parameter.
//
// ## Write protection without a base class (§3/§4)
//
// C++ restricts writes by giving each kind its own concrete type. Reads (`get`)
// live on both handles; writes (`set`/`merge`) live **only** on `Source<T, M>`.
// `Computed<T>` is a distinct template with no `set`/`merge` member, so
// `computed.set(...)` is a plain "no member named 'set'" compile error — no
// runtime gate, no shared base. The merge policy `M` lives on `Source<T, M>`,
// exactly where writes exist. (`tests/test_cell_kernel.cpp` locks this with a
// `has_set<>` detector `static_assert` and `tests/compile_fail_formula_set.cpp`
// is a WILL_FAIL build.)
//
// ## The eager construction is an eager computed, not a kind (§9.3)
//
// `Signal` is retired: eagerness is `computed(f).eager()` — a `Computed` plus a
// puller `Effect`, with eagerness stored as graph state (an `eager` bit on the
// node plus an `eager_by` side table in the `Context`, cleared on
// dispose/`.lazy()`). Because the puller is an ordinary scheduled effect, N
// invalidations in a batch coalesce into one recompute, which makes the
// `#lzsignaleager` per-write-puller defect structurally unwritable.
//
// ## Guarded by default (§9.3, DECIDED 2026-07-21)
//
// Every cell is guarded, always — there is no unguarded mode. A `Source`
// suppresses an equal write (`==` store-guard); a `Computed` suppresses an
// equal recompute (matching TC39 `Signal.Computed`). The former `memo`
// constructor is gone — `computed` now *is* the guarded form. `T` must have
// `operator==` on every cell.
//
// ## `Slot` is unchanged as the STORAGE sense (§5.0)
//
// The kernel keeps the arena vocabulary: `SlotId`, `SlotNode`, the free-list,
// and the wire `SlotValue` all stay — a slot is the position that holds a node
// of *any* kind. Only the reactive-VALUE sense of "slot" is named `Computed`.

#ifndef LAZILY_CELL_HPP
#define LAZILY_CELL_HPP

#include <lazily/context.hpp>
#include <lazily/merge.hpp>  // KeepLatest (default policy) + the merge policies

#include <memory>
#include <utility>

namespace lazily {

// ── Source — the source-cell handle ─────────────────────────────────────────

// A cell **written from outside**, folding writes under policy `M` (default
// `KeepLatest`, last-writer-wins). `Source<T>` is a plain input cell;
// `Source<T, Sum>` folds additively; etc. Reads with `get`; writes with
// `set`/`merge` — the only kind that has them. Guarded: an equal write is a
// no-op that fires no cascade.
template <typename T, typename M = KeepLatest>
class Source {
 public:
  using value_type = T;
  using policy = M;

  Source() = default;
  explicit Source(SlotId id) : id_(id) {}

  // The underlying storage-slot id (STORAGE sense of "slot", §5.0).
  SlotId id() const { return id_; }

  // Read the current converged value, registering a dependency in a computation.
  // Routes through the unified `Context::get` (`#lzcellkernel`).
  T get(Context& ctx) const { return ctx.get(*this); }

  std::shared_ptr<T> get_rc(Context& ctx) const { return ctx.get_rc(*this); }

  // Replace the value outright (the keep-latest write). Only a `Source` has
  // this — `computed.set(...)` does not compile. Routes through the unified
  // `Context::set`.
  void set(Context& ctx, T value) const { ctx.set(*this, std::move(value)); }

  // Fold `op` into the current value under policy `M`. For `KeepLatest` this is
  // a replace (`Source ≡ Source<T, KeepLatest>`). Routes through the ==-guarded
  // `set_cell`, so an idempotent policy's no-op merge fires no cascade.
  void merge(Context& ctx, T op) const {
    T old = ctx.template peek_cell<T>(CellHandle<T>(id_)).value();
    ctx.template set_cell<T>(CellHandle<T>(id_),
                            M::template merge<T>(old, std::move(op)));
  }

  // The policy-erased keep-latest view of this cell, for wiring derived readers
  // that want a plain handle. Same underlying node.
  Source<T, KeepLatest> as_keep_latest() const {
    return Source<T, KeepLatest>(id_);
  }

  // Clear all dependent computed cells without changing this cell's value.
  void clear_dependents(Context& ctx) const {
    ctx.clear_cell_dependents(id_);
  }

  // Tear this source down (detaches dependents, recycles the id). Kind-checked.
  void dispose(Context& ctx) const { ctx.dispose_cell(CellHandle<T>(id_)); }

  bool operator==(const Source& o) const { return id_ == o.id_; }
  bool operator!=(const Source& o) const { return !(*this == o); }

 private:
  SlotId id_{};
};

// ── Computed — the computed-cell handle ─────────────────────────────────────

// A cell **computed from upstream**. Lazy and guarded by default; reads with
// `get`, and `.eager()` makes it eager (an eager computed cell). Has no
// `set`/`merge` — writing a computed cell does not compile.
template <typename T>
class Computed {
 public:
  using value_type = T;

  Computed() = default;
  explicit Computed(SlotId id) : id_(id) {}

  // The underlying storage-slot id (STORAGE sense of "slot", §5.0).
  SlotId id() const { return id_; }

  // Read the current value, registering a dependency inside a computation.
  // Routes through the unified `Context::get` (`#lzcellkernel`).
  T get(Context& ctx) const { return ctx.get(*this); }

  // Read the current value as a shared_ptr, avoiding a deep clone.
  std::shared_ptr<T> get_rc(Context& ctx) const { return ctx.get_rc(*this); }

  // Transition this computed cell to **eager**: attach a puller `Effect` that
  // re-materializes it after every invalidation. Idempotent (a second `eager`
  // is a no-op) and returns the **same** handle (mutated graph state), so the
  // caller keeps reading the computed cell it already holds — not builder
  // style. This is the eager construction that retires the former `Signal`.
  Computed eager(Context& ctx) const {
    ctx.template make_eager<T>(id_);
    return *this;
  }

  // Reverse of `eager`: stop eager recomputation and dispose the puller. The
  // value stays readable and reverts to lazy. No-op if not eager.
  void lazy(Context& ctx) const { ctx.make_lazy(id_); }

  // Whether this computed cell currently has an active puller (is eager).
  bool is_eager(Context& ctx) const { return ctx.is_eager(id_); }

  // Clear the cached value and dependents; recomputes on next read.
  void clear(Context& ctx) const {
    ctx.clear_slot(id_);
    ctx.flush_effects_after_invalidation();
  }

  // Tear this computed cell down (detaches edges, tears down its puller if
  // eager, recycles the id). Kind-checked: a stale/recycled id is a no-op.
  void dispose(Context& ctx) const { ctx.dispose_slot(SlotHandle<T>(id_)); }

  bool operator==(const Computed& o) const { return id_ == o.id_; }
  bool operator!=(const Computed& o) const { return !(*this == o); }

 private:
  SlotId id_{};
};

}  // namespace lazily

#endif  // LAZILY_CELL_HPP
