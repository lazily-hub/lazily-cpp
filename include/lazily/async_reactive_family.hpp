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
#include <lazily/reactive_family.hpp>

namespace lazily {

// Traits abstracting over the two async map handle kinds — `AsyncCellHandle<V>`
// (input cells, always resolved) and `AsyncSlotHandle<V>` (derived slots, resolve
// asynchronously). The `AsyncContext` analog of `MapHandleTraits`.
template <typename H>
struct AsyncMapHandleTraits;  // primary template intentionally undefined

template <typename V>
struct AsyncMapHandleTraits<AsyncCellHandle<V>> {
  static constexpr EntryKind kind = EntryKind::Cell;

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
};

template <typename V>
struct AsyncMapHandleTraits<AsyncSlotHandle<V>> {
  static constexpr EntryKind kind = EntryKind::Slot;

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
};

template <typename K, typename H>
struct AsyncReactiveMapInner {
  std::mutex state_mutex;
  std::unordered_map<K, H> materialized;
  std::vector<K> order;
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
  explicit AsyncReactiveMap(AsyncContext&)
      : inner_(std::make_shared<AsyncReactiveMapInner<K, H>>()) {}

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
    std::optional<H> h;
    {
      std::lock_guard<std::mutex> g(inner_->state_mutex);
      auto it = inner_->materialized.find(key);
      if (it != inner_->materialized.end()) h = it->second;
    }
    if (!h) return std::nullopt;
    return Traits::observe(*h, ctx);
  }

  // Return the existing entry handle for `key`, or `std::nullopt`. Non-minting.
  std::optional<H> handle(const K& key) const {
    std::lock_guard<std::mutex> g(inner_->state_mutex);
    auto it = inner_->materialized.find(key);
    if (it == inner_->materialized.end()) return std::nullopt;
    return it->second;
  }

  bool is_present(const K& key) const {
    std::lock_guard<std::mutex> g(inner_->state_mutex);
    return inner_->materialized.count(key) > 0;
  }

  std::vector<K> present_keys() const {
    std::lock_guard<std::mutex> g(inner_->state_mutex);
    return inner_->order;
  }

  size_t present_count() const {
    std::lock_guard<std::mutex> g(inner_->state_mutex);
    return inner_->order.size();
  }

  EntryKind entry_kind() const { return Traits::kind; }

 protected:
  std::shared_ptr<AsyncReactiveMapInner<K, H>> inner_;

  H mint_with(AsyncContext& ctx, const K& key,
              const std::function<V(const K&)>& factory) {
    {
      std::lock_guard<std::mutex> g(inner_->state_mutex);
      auto it = inner_->materialized.find(key);
      if (it != inner_->materialized.end()) return it->second;  // warm.
    }
    H handle = Traits::materialize(ctx, key, factory);
    std::lock_guard<std::mutex> g(inner_->state_mutex);
    // First writer wins on a race so the key keeps a stable handle.
    auto it = inner_->materialized.find(key);
    if (it != inner_->materialized.end()) return it->second;
    inner_->materialized.emplace(key, handle);
    inner_->order.push_back(key);
    return handle;
  }
};

// An async **input-cell** map: every entry is an always-resolved
// `AsyncCellHandle<V>`. Adds cell-only `set`.
template <typename K, typename V>
class AsyncCellMap : public AsyncReactiveMap<K, V, AsyncCellHandle<V>> {
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
class AsyncSlotMap : public AsyncReactiveMap<K, V, AsyncSlotHandle<V>> {
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

}  // namespace lazily

#endif  // LAZILY_ASYNC_REACTIVE_FAMILY_HPP
