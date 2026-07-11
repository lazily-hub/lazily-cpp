#ifndef LAZILY_THREAD_SAFE_REACTIVE_FAMILY_HPP
#define LAZILY_THREAD_SAFE_REACTIVE_FAMILY_HPP

// Thread-safe keyed reactive family (`ThreadSafeReactiveFamily`, `#lzmatmode`,
// thread-safe flavor).
//
// The shared-across-threads analog of `ReactiveFamily`: keys `K` map to per-entry
// reactive nodes (`CellHandle<V>` input cells / `SlotHandle<V>` derived slots)
// allocated on a `ThreadSafeContext` per the family's `MaterializationMode`. Where
// `ReactiveFamily` keeps its present-set in a bare `shared_ptr` and is meant for a
// single thread, this family guards that state behind a `std::mutex`, so it can be
// captured by compute/effect closures and shared across the threads a
// `ThreadSafeContext` is driven from.
//
// It obeys the same three laws as the single-threaded family:
//   - Eager/lazy contract: eager materializes every declared node at build; lazy
//     defers derived (slot) nodes to first read. Cell entries are always
//     materialized.
//   - Observational transparency: `observe(key)` returns an identical value under
//     either mode.
//   - Present-set monotonicity: the materialized set only grows (deferral, never
//     de-allocation).
//
// Mirrors the `ThreadSafeReactiveFamily` conformance case in lazily-spec
// (`conformance/materialization/*`) and the `Materialization` confluence proofs in
// lazily-formal. Rust reference: `lazily-rs/src/thread_safe_reactive_family.rs`.

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <lazily/reactive_family.hpp>
#include <lazily/thread_safe.hpp>

namespace lazily {

// Traits abstracting over the two thread-safe family handle kinds —
// `CellHandle<V>` (input cells) and `SlotHandle<V>` (derived slots). The
// `Send + Sync` analog of `FamilyHandleTraits`; only these two specializations
// exist. Materialization/observation run against a `ThreadSafeContext`.
template <typename H>
struct ThreadSafeFamilyHandleTraits;  // primary template intentionally undefined

template <typename V>
struct ThreadSafeFamilyHandleTraits<CellHandle<V>> {
  static constexpr EntryKind kind = EntryKind::Cell;

  // An input has no derivation: materialize by setting its value directly.
  template <typename K>
  static CellHandle<V> materialize(ThreadSafeContext& ctx, const K& key,
                                   const std::function<V(const K&)>& factory) {
    return ctx.template cell<V>(factory(key));
  }

  static V observe(const CellHandle<V>& h, ThreadSafeContext& ctx) {
    return ctx.get_cell(h);
  }
};

template <typename V>
struct ThreadSafeFamilyHandleTraits<SlotHandle<V>> {
  static constexpr EntryKind kind = EntryKind::Slot;

  // A derived node: the same node an eager build would allocate. The factory is
  // captured as the slot's recomputation; a lazy slot's factory runs on first
  // read, off the family lock.
  template <typename K>
  static SlotHandle<V> materialize(ThreadSafeContext& ctx, const K& key,
                                   const std::function<V(const K&)>& factory) {
    K k = key;
    return ctx.template computed<V>(
        [factory, k](Context&) -> V { return factory(k); });
  }

  static V observe(const SlotHandle<V>& h, ThreadSafeContext& ctx) {
    return ctx.get(h);
  }
};

template <typename K, typename V, typename H>
struct ThreadSafeReactiveFamilyInner {
  MaterializationMode mode;
  std::function<V(const K&)> factory;
  // Present-set state guarded by `state_mutex`.
  std::mutex state_mutex;
  std::unordered_map<K, H> materialized;
  std::vector<K> order;
};

// The thread-safe unified keyed reactive family (`#lzmatmode`): keys `K` map to
// per-entry reactive nodes of handle kind `H` (`CellHandle<V>` for input cells,
// `SlotHandle<V>` for derived slots), allocated per its `MaterializationMode`.
//
// Cheap to copy (a `shared_ptr` to shared inner state) so it can be captured by
// compute/effect closures and stored in a cross-thread owner. Operations run
// against the owning `ThreadSafeContext`.
template <typename K, typename V, typename H>
class ThreadSafeReactiveFamily {
 public:
  using Handle = H;
  using Traits = ThreadSafeFamilyHandleTraits<H>;

  // -- Builders --

  // Build an **eager** family: every declared key's node is allocated now. The
  // default mode (`MaterializationMode::Eager`).
  static ThreadSafeReactiveFamily eager(ThreadSafeContext& ctx,
                                        std::vector<K> keys,
                                        std::function<V(const K&)> factory) {
    return build(ctx, MaterializationMode::Eager, std::move(keys),
                 std::move(factory));
  }
  static ThreadSafeReactiveFamily eager(ThreadSafeContext& ctx,
                                        std::initializer_list<K> keys,
                                        std::function<V(const K&)> factory) {
    return eager(ctx, std::vector<K>(keys), std::move(factory));
  }

  // Build a **lazy** family: derived (slot) entries are deferred to first read;
  // input (cell) entries in `keys` are still materialized at build (cells are
  // always materialized). Pass empty `keys` for a purely on-demand slot family.
  static ThreadSafeReactiveFamily lazy(ThreadSafeContext& ctx,
                                       std::vector<K> keys,
                                       std::function<V(const K&)> factory) {
    return build(ctx, MaterializationMode::Lazy, std::move(keys),
                 std::move(factory));
  }
  static ThreadSafeReactiveFamily lazy(ThreadSafeContext& ctx,
                                       std::initializer_list<K> keys,
                                       std::function<V(const K&)> factory) {
    return lazy(ctx, std::vector<K>(keys), std::move(factory));
  }

  // Build a family in the **default** mode (eager). Alias for `eager`.
  static ThreadSafeReactiveFamily create(ThreadSafeContext& ctx,
                                         std::vector<K> keys,
                                         std::function<V(const K&)> factory) {
    return eager(ctx, std::move(keys), std::move(factory));
  }
  static ThreadSafeReactiveFamily create(ThreadSafeContext& ctx,
                                         std::initializer_list<K> keys,
                                         std::function<V(const K&)> factory) {
    return eager(ctx, std::vector<K>(keys), std::move(factory));
  }

  // -- Access --

  // Get the entry handle for `key`, materializing it on first access (the lazy
  // pull) and caching it. Under eager mode an entry is already present, so this
  // returns the cached handle.
  H get(ThreadSafeContext& ctx, const K& key) {
    return materialize_key(ctx, key);
  }

  // Observe `key`'s value — the transparency law: the returned value is identical
  // under either mode. Materializes the entry if absent.
  V observe(ThreadSafeContext& ctx, const K& key) {
    return Traits::observe(get(ctx, key), ctx);
  }

  // Whether `key` is currently materialized (present in the allocated set).
  // Non-reactive.
  bool is_present(const K& key) const {
    std::lock_guard<std::mutex> g(inner_->state_mutex);
    return inner_->materialized.count(key) > 0;
  }

  // The currently-materialized keys, in first-materialization order. The present
  // set only grows (deferral, not de-allocation).
  std::vector<K> present_keys() const {
    std::lock_guard<std::mutex> g(inner_->state_mutex);
    return inner_->order;
  }

  // Number of currently-materialized entries.
  size_t present_count() const {
    std::lock_guard<std::mutex> g(inner_->state_mutex);
    return inner_->order.size();
  }

  // This family's materialization mode.
  MaterializationMode mode() const { return inner_->mode; }

  // This family's entry kind (`EntryKind::Cell` / `EntryKind::Slot`).
  EntryKind entry_kind() const { return Traits::kind; }

 private:
  std::shared_ptr<ThreadSafeReactiveFamilyInner<K, V, H>> inner_;

  static ThreadSafeReactiveFamily build(ThreadSafeContext& ctx,
                                        MaterializationMode mode,
                                        std::vector<K> keys,
                                        std::function<V(const K&)> factory) {
    ThreadSafeReactiveFamily fam;
    fam.inner_ = std::make_shared<ThreadSafeReactiveFamilyInner<K, V, H>>();
    fam.inner_->mode = mode;
    fam.inner_->factory = std::move(factory);
    for (auto& key : keys) {
      // buildEager materializes every node; buildLazy materializes only input
      // cells. A cell entry is always materialized regardless of mode; a slot
      // entry only under eager.
      if (Traits::kind == EntryKind::Cell ||
          mode == MaterializationMode::Eager) {
        fam.materialize_key(ctx, key);
      }
    }
    return fam;
  }

  H materialize_key(ThreadSafeContext& ctx, const K& key) {
    // Fast path: already allocated. Release the family lock before touching
    // `ctx` so a slot recompute triggered later can never re-enter this lock.
    {
      std::lock_guard<std::mutex> g(inner_->state_mutex);
      auto it = inner_->materialized.find(key);
      if (it != inner_->materialized.end()) return it->second;  // warm.
    }
    H handle = Traits::materialize(ctx, key, inner_->factory);
    std::lock_guard<std::mutex> g(inner_->state_mutex);
    // Lost a materialization race for this key: first writer wins so the key
    // keeps a stable handle (cell-identity). Our freshly-allocated node is
    // orphaned in `ctx` (unreferenced, never observed) — a rare, harmless cost.
    auto it = inner_->materialized.find(key);
    if (it != inner_->materialized.end()) return it->second;
    inner_->materialized.emplace(key, handle);
    inner_->order.push_back(key);
    return handle;
  }
};

// A thread-safe **input-cell** family: every entry is an always-materialized
// `CellHandle<V>`.
template <typename K, typename V>
using ThreadSafeCellFamily = ThreadSafeReactiveFamily<K, V, CellHandle<V>>;

// A thread-safe **derived-slot** family: entries are `SlotHandle<V>` governed by
// the family's `MaterializationMode`.
template <typename K, typename V>
using ThreadSafeSlotFamily = ThreadSafeReactiveFamily<K, V, SlotHandle<V>>;

}  // namespace lazily

#endif  // LAZILY_THREAD_SAFE_REACTIVE_FAMILY_HPP
