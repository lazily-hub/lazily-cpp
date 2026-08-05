#ifndef LAZILY_THREAD_SAFE_REACTIVE_FAMILY_HPP
#define LAZILY_THREAD_SAFE_REACTIVE_FAMILY_HPP

// Thread-safe keyed reactive collection (`ThreadSafeReactiveMap`, `#reactivemap`
// thread-safe flavor).
//
// The `Send + Sync` analog of `ReactiveMap`: keys `K` map to per-entry reactive
// nodes (`Source<V>` input cells / `Computed<V>` derived slots) allocated
// on a `ThreadSafeContext`. Where `ReactiveMap` keeps its present-set in a bare
// `shared_ptr` and is meant for a single thread, this map guards that state
// behind a `std::mutex`, so it can be captured by compute/effect closures and
// shared across the threads a `ThreadSafeContext` is driven from.
//
// It obeys the same materialization laws as the single-threaded map:
//   - Eager pre-mints every declared node (`materialize_all`); lazy defers
//     derived (slot) nodes to first read (`get_or_insert_*`). There is no
//     eager/lazy mode flag.
//   - Observational transparency: a read returns an identical value whether the
//     entry was pre-minted or minted on access.
//   - Present-set monotonicity: the materialized set only grows (deferral, never
//     de-allocation).
//
// Its two specializations are `ThreadSafeSourceMap` (input cells) and
// `ThreadSafeComputedMap` (derived slots). Rust reference:
// `lazily-rs/src/thread_safe_reactive_family.rs`.

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <lazily/cell.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include <lazily/reactive_family.hpp>
#include <lazily/thread_safe.hpp>

namespace lazily {

// Traits abstracting over the two thread-safe map handle kinds — `Source<V>`
// (input cells) and `Computed<V>` (derived slots). The `Send + Sync` analog of
// `MapHandleTraits`; only these two specializations exist. Materialization and
// observation run against a `ThreadSafeContext`.
template <typename H> struct ThreadSafeMapHandleTraits; // primary template intentionally undefined

template <typename V> struct ThreadSafeMapHandleTraits<Source<V>> {
  static constexpr EntryKind kind = EntryKind::Source;

  // An input has no derivation: materialize by setting its value directly.
  template <typename K>
  static Source<V> materialize(ThreadSafeContext& ctx, const K& key,
                               const std::function<V(const K&)>& factory) {
    return ctx.template source<V>(factory(key));
  }

  static V observe(const Source<V>& h, ThreadSafeContext& ctx) { return ctx.get(h); }

  // Disposal on removal. The thread-safe context projects onto a real `Context`
  // (`BasicThreadSafeContext` holds one), so the same node-level teardown the
  // single-threaded map uses is available here — there was never a missing
  // primitive, only a missing hook.
  static void clear_dependents(const Source<V>& h, ThreadSafeContext& ctx) {
    h.clear_dependents(ctx.context());
  }
};

template <typename V> struct ThreadSafeMapHandleTraits<Computed<V>> {
  static constexpr EntryKind kind = EntryKind::Computed;

  // A derived node: the same node an eager pre-mint would allocate. The factory
  // is captured as the slot's recomputation; it runs on first read, off the map
  // lock.
  template <typename K>
  static Computed<V> materialize(ThreadSafeContext& ctx, const K& key,
                                 const std::function<V(const K&)>& factory) {
    K k = key;
    return ctx.template computed<V>([factory, k](auto&) -> V { return factory(k); });
  }

  static V observe(const Computed<V>& h, ThreadSafeContext& ctx) { return ctx.get(h); }

  static void clear_dependents(const Computed<V>& h, ThreadSafeContext& ctx) {
    h.clear(ctx.context());
  }
};

template <typename K, typename H> struct ThreadSafeReactiveMapInner {
  // Present-set state guarded by `state_mutex`.
  mutable std::mutex state_mutex;
  KeyedOrder<K, H> keyed;
  // Reactive *set-membership* signal, minted on THIS flavor's graph. A shared
  // graph-agnostic core cannot supply reactivity — each flavor must own its own
  // version cells and route bumps through its own `set`.
  Source<uint64_t> membership;
  Source<uint64_t> order_signal;
  // Untracked mirrors, guarded by `state_mutex`, so a bump never has to read a
  // cell back through the graph while holding the map lock.
  uint64_t version = 0;
  uint64_t order_version = 0;
};

// The thread-safe keyed reactive collection (`#reactivemap`) generic over the
// entry handle kind `H` (`Source<V>` for input cells, `Computed<V>` for
// derived slots).
//
// Cheap to copy (a `shared_ptr` to shared inner state) so it can be captured by
// compute/effect closures and stored in a cross-thread owner. Operations run
// against the owning `ThreadSafeContext`.
template <typename K, typename V, typename H> class ThreadSafeReactiveMap {
public:
  using Handle = H;
  using Traits = ThreadSafeMapHandleTraits<H>;

  // Create an empty map bound to `ctx`.
  explicit ThreadSafeReactiveMap(ThreadSafeContext& ctx)
      : inner_(std::make_shared<ThreadSafeReactiveMapInner<K, H>>()) {
    inner_->membership = ctx.template source<uint64_t>(0);
    inner_->order_signal = ctx.template source<uint64_t>(0);
  }

  // -- Shared surface --

  // Get the entry handle for `key`, minting it via `factory(key)` on first access
  // (the lazy pull) and caching it. Returns the same handle on repeat.
  H get_or_insert_handle(ThreadSafeContext& ctx, const K& key, std::function<V(const K&)> factory) {
    return mint_with(ctx, key, factory);
  }

  // Get the value at `key`, minting the entry via `factory(key)` first if absent.
  // For a `ThreadSafeComputedMap` this is the lazy materialization pull.
  V get_or_insert_with(ThreadSafeContext& ctx, const K& key, std::function<V(const K&)> factory) {
    return Traits::observe(get_or_insert_handle(ctx, key, factory), ctx);
  }

  // Observe `key`'s value if the entry is present, else `std::nullopt`.
  // Non-minting.
  std::optional<V> observe(ThreadSafeContext& ctx, const K& key) {
    auto h = handle(key);
    if (!h) return std::nullopt;
    return Traits::observe(*h, ctx);
  }

  // Return the existing entry handle for `key`, or `std::nullopt`. Non-minting.
  std::optional<H> handle(const K& key) const {
    std::lock_guard<std::mutex> g(inner_->state_mutex);
    return inner_->keyed.get(key);
  }

  // Whether `key` is currently materialized (present in the allocated set).
  bool is_present(const K& key) const {
    std::lock_guard<std::mutex> g(inner_->state_mutex);
    return inner_->keyed.contains(key);
  }

  // The currently-materialized keys, in first-materialization order. The present
  // set only grows. Non-reactive — see `keys` for the tracked read.
  std::vector<K> present_keys() const {
    std::lock_guard<std::mutex> g(inner_->state_mutex);
    return inner_->keyed.keys();
  }

  // Number of currently-materialized entries. Non-reactive.
  size_t present_count() const {
    std::lock_guard<std::mutex> g(inner_->state_mutex);
    return inner_->keyed.len();
  }

  // -- Core surface: ordering, atomic move, and reactive membership --
  //
  // These bind every flavor. The move algebra touches no entry handle and
  // awaits nothing, so it is neither thread- nor async-coloured; the membership
  // and order signals are minted on this flavor's own graph.

  // Reactive snapshot of the keys in their current order. Subscribes the caller
  // to order changes (add/remove and move/reorder), not to per-entry values.
  //
  // Generic over the read surface, exactly as the single-threaded map is: a
  // `Compute&` registers a dependency edge, a bare `ThreadSafeContext&` does
  // not. A reader that could only be spelled with the context would never be
  // able to subscribe from inside a derived node.
  template <typename Cx> std::vector<K> keys(Cx& ctx) {
    (void)ctx.get(inner_->order_signal);
    std::lock_guard<std::mutex> g(inner_->state_mutex);
    return inner_->keyed.keys();
  }

  // Reactive entry count. Subscribes the caller to membership changes only.
  template <typename Cx> size_t len(Cx& ctx) {
    (void)ctx.get(inner_->membership);
    std::lock_guard<std::mutex> g(inner_->state_mutex);
    return inner_->keyed.len();
  }

  // Reactive emptiness check.
  template <typename Cx> bool is_empty(Cx& ctx) { return len(ctx) == 0; }

  // Reactive membership test for `key`. Subscribes to membership changes
  // (add/remove of any key), not to value changes.
  template <typename Cx> bool contains_key(Cx& ctx, const K& key) {
    (void)ctx.get(inner_->membership);
    std::lock_guard<std::mutex> g(inner_->state_mutex);
    return inner_->keyed.contains(key);
  }

  // Non-reactive count.
  size_t len_untracked() const { return present_count(); }

  // Current 0-based position of `key` in the order. Non-reactive.
  std::optional<size_t> position(const K& key) const {
    std::lock_guard<std::mutex> g(inner_->state_mutex);
    return inner_->keyed.position(key);
  }

  // Atomically move `key` to `index` (`#lzcellmove`). The entry keeps the same
  // node, its dependents, and its CRDT lineage; only the order signal is bumped.
  bool move_to(ThreadSafeContext& ctx, const K& key, size_t index) {
    MapMove outcome;
    {
      std::lock_guard<std::mutex> g(inner_->state_mutex);
      outcome = inner_->keyed.move_to(key, index);
    }
    return apply_move(ctx, outcome);
  }

  // Atomically move `key` to just before `anchor`.
  bool move_before(ThreadSafeContext& ctx, const K& key, const K& anchor) {
    MapMove outcome;
    {
      std::lock_guard<std::mutex> g(inner_->state_mutex);
      outcome = inner_->keyed.move_before(key, anchor);
    }
    return apply_move(ctx, outcome);
  }

  // Atomically move `key` to just after `anchor`.
  bool move_after(ThreadSafeContext& ctx, const K& key, const K& anchor) {
    MapMove outcome;
    {
      std::lock_guard<std::mutex> g(inner_->state_mutex);
      outcome = inner_->keyed.move_after(key, anchor);
    }
    return apply_move(ctx, outcome);
  }

  // Remove `key`'s entry, disposing the removed node so a reader cannot be left
  // on a stale cached value. Returns whether the key was present.
  bool remove(ThreadSafeContext& ctx, const K& key) {
    std::optional<H> handle;
    MapMutation mutation;
    {
      std::lock_guard<std::mutex> g(inner_->state_mutex);
      auto removed = inner_->keyed.remove(key);
      handle = removed.first;
      mutation = removed.second;
    }
    if (!mutation_changed(mutation)) return false;
    // Off the map lock: teardown and the membership bump can both drive a
    // dependent recompute that re-enters this map.
    Traits::clear_dependents(*handle, ctx);
    bump_membership(ctx);
    return true;
  }

  // This map's entry kind (`EntryKind::Source` / `EntryKind::Computed`).
  EntryKind entry_kind() const { return Traits::kind; }

protected:
  std::shared_ptr<ThreadSafeReactiveMapInner<K, H>> inner_;

  H mint_with(ThreadSafeContext& ctx, const K& key, const std::function<V(const K&)>& factory) {
    // Fast path: already allocated. Release the map lock before touching `ctx` so
    // a slot recompute triggered later can never re-enter this lock.
    {
      std::lock_guard<std::mutex> g(inner_->state_mutex);
      if (auto warm = inner_->keyed.get(key)) return *warm; // warm.
    }
    H handle = Traits::materialize(ctx, key, factory);
    // `H` is not required to be default-constructible (an `AsyncSource`
    // needs a context), so the race outcome is carried in an optional.
    std::optional<H> stored;
    MapMutation mutation;
    {
      std::lock_guard<std::mutex> g(inner_->state_mutex);
      // Lost a materialization race for this key: first writer wins so the key
      // keeps a stable handle (cell-identity). Our freshly-allocated node is
      // orphaned in `ctx` (unreferenced, never observed) — a rare, harmless cost.
      auto inserted = inner_->keyed.insert(key, handle);
      stored.emplace(inserted.first);
      mutation = inserted.second;
    }
    // Bump with the map lock released: `ctx.set` can drive a dependent recompute
    // that re-enters this map.
    if (mutation_changed(mutation)) bump_membership(ctx);
    return *stored;
  }

  // Bump the order signal only when the order actually changed.
  bool apply_move(ThreadSafeContext& ctx, MapMove outcome) {
    if (!move_applied(outcome)) return false;
    if (move_changed(outcome)) bump_order(ctx);
    return true;
  }

  // Bump the *order* signal (invalidates `keys` readers).
  void bump_order(ThreadSafeContext& ctx) {
    uint64_t next;
    {
      std::lock_guard<std::mutex> g(inner_->state_mutex);
      next = ++inner_->order_version;
    }
    ctx.set(inner_->order_signal, next);
  }

  // Bump set-membership (invalidates `len` / `contains_key` readers). Always
  // paired with an order bump because add/remove change order too.
  void bump_membership(ThreadSafeContext& ctx) {
    uint64_t next;
    {
      std::lock_guard<std::mutex> g(inner_->state_mutex);
      next = ++inner_->version;
    }
    ctx.set(inner_->membership, next);
    bump_order(ctx);
  }
};

// A thread-safe **input-cell** map: every entry is an always-materialized
// `Source<V>`. Adds cell-only `set`. The `Send + Sync` analog of `SourceMap`.
template <typename K, typename V>
class ThreadSafeSourceMap : public ThreadSafeReactiveMap<K, V, Source<V>> {
public:
  using Base = ThreadSafeReactiveMap<K, V, Source<V>>;
  using Base::Base;

  // Set the value at `key`, inserting a new input cell if absent. Cell-only.
  void set(ThreadSafeContext& ctx, const K& key, V value) {
    auto h = this->handle(key);
    if (h) {
      ctx.set(*h, std::move(value));
      return;
    }
    this->get_or_insert_handle(ctx, key, [value](const K&) -> V { return value; });
  }
};

// The lock-backed exact-key dependency availability family.
template <typename K, typename V>
class ThreadSafeDependencyMap : public ThreadSafeSourceMap<K, DependencyAvailability<V>> {
public:
  using Availability = DependencyAvailability<V>;
  using Base = ThreadSafeSourceMap<K, Availability>;
  using Base::Base;

  Availability observe_dependency(ThreadSafeContext& ctx, const K& key) {
    return this->get_or_insert_with(ctx, key, [](const K&) { return Availability::unavailable(); });
  }

  void publish(ThreadSafeContext& ctx, const K& key, V value) {
    this->set(ctx, key, Availability::available(std::move(value)));
  }

  void unpublish(ThreadSafeContext& ctx, const K& key) {
    this->set(ctx, key, Availability::unavailable());
  }
};

// A thread-safe **derived-slot** map: entries are `Computed<V>` minted lazily
// on access or eagerly via `materialize_all`.
template <typename K, typename V>
class ThreadSafeComputedMap : public ThreadSafeReactiveMap<K, V, Computed<V>> {
public:
  using Base = ThreadSafeReactiveMap<K, V, Computed<V>>;
  using Base::Base;

  // **Eager materialization**: pre-mint a derived slot for every key in `keys`.
  // Observationally identical to minting each lazily on first read.
  void materialize_all(ThreadSafeContext& ctx, const std::vector<K>& keys,
                       std::function<V(const K&)> factory) {
    for (const auto& key : keys)
      this->get_or_insert_handle(ctx, key, factory);
  }
  void materialize_all(ThreadSafeContext& ctx, std::initializer_list<K> keys,
                       std::function<V(const K&)> factory) {
    materialize_all(ctx, std::vector<K>(keys), std::move(factory));
  }
};

// -- Deprecated pre-v2 spellings --
//
// The v2 kernel renamed the node kinds to `Source` and `Computed`; the map names
// followed. The old names remain as alias templates so existing callers keep
// compiling — they are not removed.
template <typename K, typename V>
using ThreadSafeCellMap [[deprecated("renamed to ThreadSafeSourceMap")]] =
    ThreadSafeSourceMap<K, V>;
template <typename K, typename V>
using ThreadSafeSlotMap [[deprecated("renamed to ThreadSafeComputedMap")]] =
    ThreadSafeComputedMap<K, V>;

} // namespace lazily

#endif // LAZILY_THREAD_SAFE_REACTIVE_FAMILY_HPP
