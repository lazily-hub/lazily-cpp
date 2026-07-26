#ifndef LAZILY_ASYNC_REACTIVE_FAMILY_HPP
#define LAZILY_ASYNC_REACTIVE_FAMILY_HPP

// Async keyed reactive collection (`AsyncReactiveMap`, `#reactivemap` async
// flavor).
//
// The `AsyncContext` analog of `ReactiveMap`: keys `K` map to per-entry async
// reactive nodes (`AsyncCellHandle<V>` input cells / `AsyncSlotHandle<V>` derived
// slots). Like `ThreadSafeReactiveMap` it guards its present-set state behind a
// `std::mutex`, so it can live in a cross-task owner.
//
// Eager pre-mints every declared node (`materialize_all`); lazy mints on access
// (`get_or_insert_handle`). There is no eager/lazy mode flag. Present-set
// monotonicity holds as in the other flavors. The transparency law is
// **eventual**: an async derived slot read is empty (`std::nullopt`) while pending
// and resolves to the canonical value — so `observe` returns `std::optional<V>`.
// Input cells are always resolved. Drive a slot to resolution with `get_async()`
// on the handle from `get_or_insert_handle`.
//
// Rust reference: `lazily-rs/src/async_reactive_family.rs`.

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include <lazily/async_context.hpp>
#include <lazily/keyed_order.hpp>
#include <lazily/reactive_family.hpp>

namespace lazily {

// Traits abstracting over the two async map handle kinds — `AsyncCellHandle<V>`
// (input cells, always resolved) and `AsyncSlotHandle<V>` (derived slots, resolve
// asynchronously). The `AsyncContext` analog of `MapHandleTraits`.
template <typename H>
struct AsyncMapHandleTraits;  // primary template intentionally undefined

template <typename V>
struct AsyncMapHandleTraits<AsyncCellHandle<V>> {
  static constexpr EntryKind kind = EntryKind::Source;

  template <typename K>
  static AsyncCellHandle<V> materialize(
      AsyncContext& ctx, const K& key,
      const std::function<V(const K&)>& factory) {
    return ctx.template cell<V>(factory(key));
  }

  // A materialized cell is always resolved.
  static std::optional<V> observe(AsyncCellHandle<V> h, AsyncContext&) {
    return std::optional<V>(h.get());
  }

  static void clear_dependents(AsyncCellHandle<V> h, AsyncContext&) {
    h.clear_dependents();
  }
};

template <typename V>
struct AsyncMapHandleTraits<AsyncSlotHandle<V>> {
  static constexpr EntryKind kind = EntryKind::Computed;

  // A derived node whose async recompute yields the sync factory value. Resolve
  // it with `get_async()` on the returned handle.
  template <typename K>
  static AsyncSlotHandle<V> materialize(
      AsyncContext& ctx, const K& key,
      const std::function<V(const K&)>& factory) {
    K k = key;
    return ctx.template slot<V>(
        std::function<V()>([factory, k]() -> V { return factory(k); }));
  }

  // Non-blocking read: a value once resolved, else `std::nullopt`.
  static std::optional<V> observe(AsyncSlotHandle<V> h, AsyncContext&) {
    return h.get();
  }

  static void clear_dependents(AsyncSlotHandle<V> h, AsyncContext&) {
    h.clear_dependents();
  }
};

template <typename K, typename H>
struct AsyncReactiveMapInner {
  mutable std::mutex state_mutex;
  KeyedOrder<K, H> keyed;
  // Membership and order signals minted on THIS flavor's graph. Ordering is not
  // async-coloured — the move algebra awaits nothing — so the async map carries
  // the same Core surface as the other two flavors.
  Source<uint64_t> membership;
  Source<uint64_t> order_signal;
  uint64_t version = 0;
  uint64_t order_version = 0;
};

// The async keyed reactive collection (`#reactivemap`) generic over the entry
// handle kind `H` (`AsyncCellHandle<V>` input cells, `AsyncSlotHandle<V>` derived
// slots).
//
// Cheap to copy (a `shared_ptr` to shared inner state). See the eventual-
// transparency law above.
template <typename K, typename V, typename H>
class AsyncReactiveMap {
 public:
  using Handle = H;
  using Traits = AsyncMapHandleTraits<H>;

  // Create an empty map bound to `ctx`.
  explicit AsyncReactiveMap(AsyncContext& ctx)
      : inner_(std::make_shared<AsyncReactiveMapInner<K, H>>()) {
    inner_->membership = ctx.context().source(uint64_t(0));
    inner_->order_signal = ctx.context().source(uint64_t(0));
  }

  // -- Shared surface --

  // Get the entry handle for `key`, minting it via `factory(key)` on first access
  // and caching it. For a slot map this is the `AsyncSlotHandle` to drive with
  // `get_async()`.
  H get_or_insert_handle(AsyncContext& ctx, const K& key,
                         std::function<V(const K&)> factory) {
    return mint_with(ctx, key, factory);
  }

  // Non-blocking observe: a value for a cell or resolved slot, `std::nullopt` for
  // a pending or absent slot. Non-minting.
  std::optional<V> observe(AsyncContext& ctx, const K& key) {
    auto h = handle(key);
    if (!h) return std::nullopt;
    return Traits::observe(*h, ctx);
  }

  // Return the existing entry handle for `key`, or `std::nullopt`. Non-minting.
  std::optional<H> handle(const K& key) const {
    std::lock_guard<std::mutex> g(inner_->state_mutex);
    return inner_->keyed.get(key);
  }

  bool is_present(const K& key) const {
    std::lock_guard<std::mutex> g(inner_->state_mutex);
    return inner_->keyed.contains(key);
  }

  std::vector<K> present_keys() const {
    std::lock_guard<std::mutex> g(inner_->state_mutex);
    return inner_->keyed.keys();
  }

  size_t present_count() const {
    std::lock_guard<std::mutex> g(inner_->state_mutex);
    return inner_->keyed.len();
  }

  // -- Core surface: ordering, atomic move, and reactive membership --

  // Reactive snapshot of the keys in their current order. Generic over the read
  // surface for the same reason the other two flavors are: only a `Compute&`
  // registers the edge.
  template <typename Cx>
  std::vector<K> keys(Cx& ctx) {
    (void)ctx.get(inner_->order_signal);
    std::lock_guard<std::mutex> g(inner_->state_mutex);
    return inner_->keyed.keys();
  }

  // Reactive entry count. Subscribes to membership changes only.
  template <typename Cx>
  size_t len(Cx& ctx) {
    (void)ctx.get(inner_->membership);
    std::lock_guard<std::mutex> g(inner_->state_mutex);
    return inner_->keyed.len();
  }

  template <typename Cx>
  bool is_empty(Cx& ctx) { return len(ctx) == 0; }

  // Reactive membership test for `key`.
  template <typename Cx>
  bool contains_key(Cx& ctx, const K& key) {
    (void)ctx.get(inner_->membership);
    std::lock_guard<std::mutex> g(inner_->state_mutex);
    return inner_->keyed.contains(key);
  }

  size_t len_untracked() const { return present_count(); }

  std::optional<size_t> position(const K& key) const {
    std::lock_guard<std::mutex> g(inner_->state_mutex);
    return inner_->keyed.position(key);
  }

  // Atomically move `key` to `index` (`#lzcellmove`). The entry keeps the same
  // node and its lineage; only the order signal is bumped.
  bool move_to(AsyncContext& ctx, const K& key, size_t index) {
    MapMove outcome;
    {
      std::lock_guard<std::mutex> g(inner_->state_mutex);
      outcome = inner_->keyed.move_to(key, index);
    }
    return apply_move(ctx, outcome);
  }

  bool move_before(AsyncContext& ctx, const K& key, const K& anchor) {
    MapMove outcome;
    {
      std::lock_guard<std::mutex> g(inner_->state_mutex);
      outcome = inner_->keyed.move_before(key, anchor);
    }
    return apply_move(ctx, outcome);
  }

  bool move_after(AsyncContext& ctx, const K& key, const K& anchor) {
    MapMove outcome;
    {
      std::lock_guard<std::mutex> g(inner_->state_mutex);
      outcome = inner_->keyed.move_after(key, anchor);
    }
    return apply_move(ctx, outcome);
  }

  // Remove `key`'s entry, disposing the removed node so a stale cached value
  // cannot outlive the removal.
  bool remove(AsyncContext& ctx, const K& key) {
    std::optional<H> h;
    MapMutation mutation;
    {
      std::lock_guard<std::mutex> g(inner_->state_mutex);
      auto removed = inner_->keyed.remove(key);
      h = removed.first;
      mutation = removed.second;
    }
    if (!mutation_changed(mutation)) return false;
    Traits::clear_dependents(*h, ctx);
    bump_membership(ctx);
    return true;
  }

  EntryKind entry_kind() const { return Traits::kind; }

 protected:
  std::shared_ptr<AsyncReactiveMapInner<K, H>> inner_;

  H mint_with(AsyncContext& ctx, const K& key,
              const std::function<V(const K&)>& factory) {
    {
      std::lock_guard<std::mutex> g(inner_->state_mutex);
      if (auto warm = inner_->keyed.get(key)) return *warm;  // warm.
    }
    H handle = Traits::materialize(ctx, key, factory);
    // `H` is not required to be default-constructible (an `AsyncCellHandle`
    // needs a context), so the race outcome is carried in an optional.
    std::optional<H> stored;
    MapMutation mutation;
    {
      std::lock_guard<std::mutex> g(inner_->state_mutex);
      // First writer wins on a race so the key keeps a stable handle.
      auto inserted = inner_->keyed.insert(key, handle);
      stored.emplace(inserted.first);
      mutation = inserted.second;
    }
    // Bump with the map lock released: a set can drive a dependent recompute
    // that re-enters this map.
    if (mutation_changed(mutation)) bump_membership(ctx);
    return *stored;
  }

  bool apply_move(AsyncContext& ctx, MapMove outcome) {
    if (!move_applied(outcome)) return false;
    if (move_changed(outcome)) bump_order(ctx);
    return true;
  }

  void bump_order(AsyncContext& ctx) {
    uint64_t next;
    {
      std::lock_guard<std::mutex> g(inner_->state_mutex);
      next = ++inner_->order_version;
    }
    ctx.context().set(inner_->order_signal, next);
  }

  void bump_membership(AsyncContext& ctx) {
    uint64_t next;
    {
      std::lock_guard<std::mutex> g(inner_->state_mutex);
      next = ++inner_->version;
    }
    ctx.context().set(inner_->membership, next);
    bump_order(ctx);
  }
};

// An async **input-cell** map: every entry is an always-resolved
// `AsyncCellHandle<V>`. Adds cell-only `set`.
template <typename K, typename V>
class AsyncSourceMap : public AsyncReactiveMap<K, V, AsyncCellHandle<V>> {
 public:
  using Base = AsyncReactiveMap<K, V, AsyncCellHandle<V>>;
  using Base::Base;

  // Set the value at `key`, inserting a new input cell if absent. Cell-only.
  void set(AsyncContext& ctx, const K& key, V value) {
    auto h = this->handle(key);
    if (h) {
      h->set(std::move(value));
      return;
    }
    this->get_or_insert_handle(ctx, key,
                               [value](const K&) -> V { return value; });
  }
};

// An async **derived-slot** map: entries are `AsyncSlotHandle<V>` minted lazily
// on access or eagerly via `materialize_all`, resolved via `get_async()`.
template <typename K, typename V>
class AsyncComputedMap : public AsyncReactiveMap<K, V, AsyncSlotHandle<V>> {
 public:
  using Base = AsyncReactiveMap<K, V, AsyncSlotHandle<V>>;
  using Base::Base;

  // **Eager materialization**: pre-mint a derived slot for every key in `keys`.
  void materialize_all(AsyncContext& ctx, const std::vector<K>& keys,
                       std::function<V(const K&)> factory) {
    for (const auto& key : keys)
      this->get_or_insert_handle(ctx, key, factory);
  }
  void materialize_all(AsyncContext& ctx, std::initializer_list<K> keys,
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
using AsyncCellMap [[deprecated("renamed to AsyncSourceMap")]] =
    AsyncSourceMap<K, V>;
template <typename K, typename V>
using AsyncSlotMap [[deprecated("renamed to AsyncComputedMap")]] =
    AsyncComputedMap<K, V>;

}  // namespace lazily

#endif  // LAZILY_ASYNC_REACTIVE_FAMILY_HPP
