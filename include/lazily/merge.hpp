// Phase 1 of the RelayCell backpressure plan (#relaycell) — the merge algebra.
//
// See lazily-spec/docs/reactive-graph.md § "MergeCell and the merge algebra" and
// relaycell-backpressure-analysis.md §4.0/§4.3. A merge policy is an associative
// fold ⊕: T×T→T; the properties it satisfies (associativity always; commutativity
// = reordering tax; idempotency = durability tax) select which overflow behaviour
// is sound. MergeCell generalizes a plain Cell — Cell ≡ MergeCell<KeepLatest> —
// a source whose write is a merge. Backed by an ordinary cell, so it inherits the
// Phase-0 == store-guard + store-without-cascade.
//
// Policies are compile-time types (a static `merge` + `static constexpr` flags),
// mirroring the Rust `MergePolicy` trait. Associativity is a law verified by the
// law-tests, not a flag.

#ifndef LAZILY_MERGE_HPP
#define LAZILY_MERGE_HPP

#include <lazily/context.hpp>

#include <set>
#include <utility>
#include <vector>

namespace lazily {

// -- Merge policies (compile-time) ------------------------------------------

/// Keep-latest band (old ⊕ op = op) — the policy behind a plain Cell.
struct KeepLatest {
  static constexpr const char* name = "KeepLatest";
  static constexpr bool commutative = false;
  static constexpr bool idempotent = true;
  static constexpr bool conflates = true;
  template <typename T>
  static T merge(const T&, T op) {
    return op;
  }
};

/// Additive commutative monoid (old + op). Not idempotent.
struct Sum {
  static constexpr const char* name = "Sum";
  static constexpr bool commutative = true;
  static constexpr bool idempotent = false;
  static constexpr bool conflates = true;
  template <typename T>
  static T merge(const T& a, T b) {
    return a + b;
  }
};

/// Max semilattice (max(old, op)). Associative, commutative, idempotent.
struct Max {
  static constexpr const char* name = "Max";
  static constexpr bool commutative = true;
  static constexpr bool idempotent = true;
  static constexpr bool conflates = true;
  template <typename T>
  static T merge(const T& a, T b) {
    return b > a ? b : a;
  }
};

/// Grow-only set-union semilattice over std::set<E>.
struct SetUnion {
  static constexpr const char* name = "SetUnion";
  static constexpr bool commutative = true;
  static constexpr bool idempotent = true;
  static constexpr bool conflates = true;
  template <typename S>
  static S merge(const S& a, S b) {
    S out = a;
    out.insert(b.begin(), b.end());
    return out;
  }
};

/// Raw FIFO append over std::vector<E> (old ++ op). Order + multiplicity are
/// meaning — associative only; cannot conflate.
struct RawFifo {
  static constexpr const char* name = "RawFifo";
  static constexpr bool commutative = false;
  static constexpr bool idempotent = false;
  static constexpr bool conflates = false;
  template <typename V>
  static V merge(const V& a, V b) {
    V out = a;
    out.insert(out.end(), b.begin(), b.end());
    return out;
  }
};

// -- MergeCell --------------------------------------------------------------

/// A cell whose write is a merge under `Policy` rather than a replace.
/// Cell ≡ MergeCell<KeepLatest>. `merge` routes through the cell's ==-guarded
/// set_cell, so an idempotent policy's no-op merge fires no cascade (free dedup).
template <typename T, typename Policy>
class MergeCell {
 public:
  // `#lzcellkernel`: the cell is held by id (`Source<T>` lives in cell.hpp,
  // which includes this header — storing the id keeps merge.hpp free of that
  // cycle; the `Source<T>` handle is materialized inside each template method,
  // which is only instantiated where cell.hpp is already complete).
  MergeCell(Context& ctx, T initial)
      : ctx_(&ctx), cell_(ctx.source(std::move(initial)).id()) {}

  /// The underlying reactive cell (for wiring derived readers).
  Source<T> cell() const { return Source<T>(cell_); }

  /// Read the current converged value (tracks a dependency in a computation).
  T get() const { return ctx_->get(Source<T>(cell_)); }

  /// Replace the value outright (the keep-latest write), bypassing the policy.
  void set(T value) { ctx_->set(Source<T>(cell_), std::move(value)); }

  /// Fold `op` into the current value under `Policy`. Reads untracked via peek.
  void merge(T op) {
    Source<T> h(cell_);
    T old = ctx_->peek_cell(h).value();
    ctx_->set(h, Policy::template merge<T>(old, std::move(op)));
  }

 private:
  Context* ctx_;
  SlotId cell_;
};

}  // namespace lazily

#endif  // LAZILY_MERGE_HPP
