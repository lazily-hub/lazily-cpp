// The Cell kernel (`#lzcellkernel`) — `SourceCell` / `FormulaCell` over a single
// genus `Cell<T, K>`.
//
// See `tasks/software/lazily-cell-kernel-design.md` (§1/§3/§4/§9.3). One handle
// type with a **kind** type parameter `K` is the public reactive surface,
// replacing the four-handle vocabulary (`CellHandle` / `SlotHandle` /
// `SignalHandle` / `MergeCell`) with two named aliases over one genus:
//
//   Cell<T, K>                    genus — a node with a readable value
//   ├─ SourceCell<T, M>           written from outside; folds under policy M
//   └─ FormulaCell<T>             computed from upstream (guarded by default)
//
//   Effect                        no value; a sink — outside the hierarchy
//
// Both aliases answer the same question — *where does a node's value come from*
// — so the pair is exhaustive: `SourceCell` from outside, `FormulaCell` from
// upstream. `EffectHandle` stays outside the hierarchy (a sink, no value).
//
// ## Write protection without a base class (§3/§4)
//
// C++ restricts writes by **partial specialization**, the trick §4 lists for
// cpp. Reads (`get`) live on both specializations; writes (`set`/`merge`) live
// **only** on `Cell<T, Source<M>>`. `Cell<T, Formula>` is a distinct
// specialization with no `set`/`merge` member, so `formula.set(...)` is a plain
// "no member named 'set'" compile error — no runtime gate, no shared base. The
// merge policy `M` lives inside the `Source<M>` marker, exactly where writes
// exist. (`tests/test_cell_kernel.cpp` locks this with a `has_set<>` detector
// and `tests/compile_fail_formula_set.cpp` is a WILL_FAIL build.)
//
// ## The eager construction is a driven formula, not a kind (§9.3)
//
// `Signal` is retired: eagerness is `formula(f).drive()` — a `FormulaCell` plus
// a puller `Effect`, with drivenness stored as graph state (a `driven` bit on
// the node plus a `driven_by` side table in the `Context`, cleared on
// dispose/undrive). Because the puller is an ordinary scheduled effect, N
// invalidations in a batch coalesce into one recompute, which makes the
// `#lzsignaleager` per-write-puller defect structurally unwritable.
//
// ## `Slot` is unchanged as the STORAGE sense (§5.0)
//
// The kernel keeps the arena vocabulary: `SlotId`, `SlotNode`, the free-list,
// and the wire `SlotValue` all stay — a slot is the position that holds a node
// of *any* kind. Only the reactive-VALUE sense of "slot" is renamed to
// `FormulaCell`.

#ifndef LAZILY_CELL_HPP
#define LAZILY_CELL_HPP

#include <lazily/context.hpp>
#include <lazily/merge.hpp>  // KeepLatest (default policy) + the merge policies

#include <memory>
#include <utility>

namespace lazily {

// ── Kind markers ────────────────────────────────────────────────────────────

// Kind marker for a **source** cell — a node written from outside, folding
// accumulated writes under merge policy `M`. Carries the policy so writes exist
// exactly where the policy does (`set`/`merge` on `Cell<T, Source<M>>`).
template <typename M>
struct Source {};

// Kind marker for a **formula** cell — a node computed from upstream. A driven
// formula (`formula().drive()`) is still this kind; drivenness is graph state,
// not a distinct type.
struct Formula {};

// ── The genus ───────────────────────────────────────────────────────────────

// Primary template — intentionally undefined. Only the two kind specializations
// (`Source<M>` and `Formula`) exist; bindings do not add new kinds.
template <typename T, typename K>
class Cell;

// A cell **computed from upstream**. Lazy and guarded by default; reads with
// `get`, and `drive()` makes it eager (a driven formula). Has no `set`/`merge`
// — writing a formula does not compile.
template <typename T>
class Cell<T, Formula> {
 public:
  using value_type = T;

  Cell() = default;
  explicit Cell(SlotId id) : id_(id) {}

  // The underlying storage-slot id (STORAGE sense of "slot", §5.0).
  SlotId id() const { return id_; }

  // Read the current value, registering a dependency inside a computation.
  T get(Context& ctx) const { return ctx.template get<T>(SlotHandle<T>(id_)); }

  // Read the current value as a shared_ptr, avoiding a deep clone.
  std::shared_ptr<T> get_rc(Context& ctx) const {
    return ctx.template get_rc<T>(SlotHandle<T>(id_));
  }

  // **Drive** this formula: make it eager by attaching a puller `Effect` that
  // re-materializes it after every invalidation. Idempotent (a second `drive`
  // is a no-op) and returns the **same** handle (mutated graph state), so the
  // caller keeps reading the formula it already holds — this is not builder
  // style. Retires the former `Signal`.
  Cell drive(Context& ctx) const {
    ctx.template drive_formula<T>(id_);
    return *this;
  }

  // Reverse of `drive`: stop eager recomputation and dispose the puller. The
  // value stays readable and reverts to lazy. No-op if not driven.
  void undrive(Context& ctx) const { ctx.undrive_formula(id_); }

  // Whether this formula currently has an active puller.
  bool is_driven(Context& ctx) const { return ctx.is_driven(id_); }

  // Clear the cached value and dependents; recomputes on next read.
  void clear(Context& ctx) const {
    ctx.clear_slot(id_);
    ctx.flush_effects_after_invalidation();
  }

  // Tear this formula down (detaches edges, tears down its puller if driven,
  // recycles the id). Kind-checked: a stale/recycled id is a no-op.
  void dispose(Context& ctx) const { ctx.dispose_slot(SlotHandle<T>(id_)); }

  bool operator==(const Cell& o) const { return id_ == o.id_; }
  bool operator!=(const Cell& o) const { return !(*this == o); }

 private:
  SlotId id_{};
};

// A cell **written from outside**, folding writes under policy `M` (default
// `KeepLatest`, last-writer-wins). `SourceCell<T>` is a plain input cell;
// `SourceCell<T, Sum>` folds additively; etc. Reads with `get`; writes with
// `set`/`merge` — the only kind that has them.
template <typename T, typename M>
class Cell<T, Source<M>> {
 public:
  using value_type = T;
  using policy = M;

  Cell() = default;
  explicit Cell(SlotId id) : id_(id) {}

  SlotId id() const { return id_; }

  // Read the current converged value, registering a dependency in a computation.
  T get(Context& ctx) const {
    return ctx.template get_cell<T>(CellHandle<T>(id_));
  }

  std::shared_ptr<T> get_rc(Context& ctx) const {
    return ctx.template get_cell_rc<T>(CellHandle<T>(id_));
  }

  // Replace the value outright (the keep-latest write). Only a `SourceCell` has
  // this — `formula.set(...)` does not compile.
  void set(Context& ctx, T value) const {
    ctx.template set_cell<T>(CellHandle<T>(id_), std::move(value));
  }

  // Fold `op` into the current value under policy `M`. For `KeepLatest` this is
  // a replace (`Cell ≡ SourceCell<KeepLatest>`). Routes through the ==-guarded
  // `set_cell`, so an idempotent policy's no-op merge fires no cascade.
  void merge(Context& ctx, T op) const {
    T old = ctx.template peek_cell<T>(CellHandle<T>(id_)).value();
    ctx.template set_cell<T>(CellHandle<T>(id_),
                            M::template merge<T>(old, std::move(op)));
  }

  // The policy-erased keep-latest view of this cell, for wiring derived readers
  // that want a plain handle. Same underlying node. (Compatibility shim for the
  // former `MergeCell::cell()`.)
  Cell<T, Source<KeepLatest>> as_keep_latest() const {
    return Cell<T, Source<KeepLatest>>(id_);
  }

  // Clear all dependent formulas without changing this cell's value.
  void clear_dependents(Context& ctx) const {
    ctx.clear_cell_dependents(id_);
  }

  // Tear this source down (detaches dependents, recycles the id). Kind-checked.
  void dispose(Context& ctx) const { ctx.dispose_cell(CellHandle<T>(id_)); }

  bool operator==(const Cell& o) const { return id_ == o.id_; }
  bool operator!=(const Cell& o) const { return !(*this == o); }

 private:
  SlotId id_{};
};

// ── Aliases ─────────────────────────────────────────────────────────────────

// A cell written from outside, folding under policy `M` (default `KeepLatest`).
template <typename T, typename M = KeepLatest>
using SourceCell = Cell<T, Source<M>>;

// A cell computed from upstream (guarded, lazy by default; `.drive()` for eager).
template <typename T>
using FormulaCell = Cell<T, Formula>;

}  // namespace lazily

#endif  // LAZILY_CELL_HPP
