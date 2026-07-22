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
// Its two specializations are `ThreadSafeCellMap` (input cells) and
// `ThreadSafeSlotMap` (derived slots). Rust reference:
// `lazily-rs/src/thread_safe_reactive_family.rs`.

#include <cstddef>
#include <lazily/cell.hpp>
#include <functional>
#include <initializer_list>
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
template <typename H>
struct ThreadSafeMapHandleTraits;  // primary template intentionally undefined

template <typename V>
struct ThreadSafeMapHandleTraits<Source<V>> {
  static constexpr EntryKind kind = EntryKind::Cell;

  // An input has no derivation: materialize by setting its value directly.
  template <typename K>
  static Source<V> materialize(ThreadSafeContext& ctx, const K& key,
                                   const std::function<V(const K&)>& factory) {
    return ctx.template cell<V>(factory(key));
  }

  static V observe(const Source<V>& h, ThreadSafeContext& ctx) {
    return ctx.get(h);
  }
};

template <typename V>
struct ThreadSafeMapHandleTraits<Computed<V>> {
  static constexpr EntryKind kind = EntryKind::Slot;

  // A derived node: the same node an eager pre-mint would allocate. The factory
  // is captured as the slot's recomputation; it runs on first read, off the map
  // lock.
  template <typename K>
  static Computed<V> materialize(ThreadSafeContext& ctx, const K& key,
                                   const std::function<V(const K&)>& factory) {
    K k = key;
    return ctx.template computed<V>(
        [factory, k](Context&) -> V { return factory(k); });
  }

  static V observe(const Computed<V>& h, ThreadSafeContext& ctx) {
    return ctx.get(h);
  }
};

template <typename K, typename H>
struct ThreadSafeReactiveMapInner {
  // Present-set state guarded by `state_mutex`.
  std::mutex state_mutex;
  std::unordered_map<K, H> materialized;
  std::vector<K> order;
};

// The thread-safe keyed reactive collection (`#reactivemap`) generic over the
// entry handle kind `H` (`Source<V>` for input cells, `Computed<V>` for
// derived slots).
//
// Cheap to copy (a `shared_ptr` to shared inner state) so it can be captured by
// compute/effect closures and stored in a cross-thread owner. Operations run
// against the owning `ThreadSafeContext`.
template <typename K, typename V, typename H>
class ThreadSafeReactiveMap {
 public:
  using Handle = H;
  using Traits = ThreadSafeMapHandleTraits<H>;

  // Create an empty map bound to `ctx`.
  explicit ThreadSafeReactiveMap(ThreadSafeContext&)
      : inner_(std::make_shared<ThreadSafeReactiveMapInner<K, H>>()) {}

  // -- Shared surface --

  // Get the entry handle for `key`, minting it via `factory(key)` on first access
  // (the lazy pull) and caching it. Returns the same handle on repeat.
  H get_or_insert_handle(ThreadSafeContext& ctx, const K& key,
                         std::function<V(const K&)> factory) {
    return mint_with(ctx, key, factory);
  }

  // Get the value at `key`, minting the entry via `factory(key)` first if absent.
  // For a `ThreadSafeSlotMap` this is the lazy materialization pull.
  V get_or_insert_with(ThreadSafeContext& ctx, const K& key,
                       std::function<V(const K&)> factory) {
    return Traits::observe(get_or_insert_handle(ctx, key, factory), ctx);
  }

  // Observe `key`'s value if the entry is present, else `std::nullopt`.
  // Non-minting.
  std::optional<V> observe(ThreadSafeContext& ctx, const K& key) {
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

  // Whether `key` is currently materialized (present in the allocated set).
  bool is_present(const K& key) const {
    std::lock_guard<std::mutex> g(inner_->state_mutex);
    return inner_->materialized.count(key) > 0;
  }

  // The currently-materialized keys, in first-materialization order. The present
  // set only grows.
  std::vector<K> present_keys() const {
    std::lock_guard<std::mutex> g(inner_->state_mutex);
    return inner_->order;
  }

  // Number of currently-materialized entries.
  size_t present_count() const {
    std::lock_guard<std::mutex> g(inner_->state_mutex);
    return inner_->order.size();
  }

  // This map's entry kind (`EntryKind::Cell` / `EntryKind::Slot`).
  EntryKind entry_kind() const { return Traits::kind; }

 protected:
  std::shared_ptr<ThreadSafeReactiveMapInner<K, H>> inner_;

  H mint_with(ThreadSafeContext& ctx, const K& key,
              const std::function<V(const K&)>& factory) {
    // Fast path: already allocated. Release the map lock before touching `ctx` so
    // a slot recompute triggered later can never re-enter this lock.
    {
      std::lock_guard<std::mutex> g(inner_->state_mutex);
      auto it = inner_->materialized.find(key);
      if (it != inner_->materialized.end()) return it->second;  // warm.
    }
    H handle = Traits::materialize(ctx, key, factory);
    std::lock_guard<std::mutex> g(inner_->state_mutex);
    // Lost a materialization race for this key: first writer wins so the key keeps
    // a stable handle (cell-identity). Our freshly-allocated node is orphaned in
    // `ctx` (unreferenced, never observed) — a rare, harmless cost.
    auto it = inner_->materialized.find(key);
    if (it != inner_->materialized.end()) return it->second;
    inner_->materialized.emplace(key, handle);
    inner_->order.push_back(key);
    return handle;
  }
};

// A thread-safe **input-cell** map: every entry is an always-materialized
// `Source<V>`. Adds cell-only `set`. The `Send + Sync` analog of `CellMap`.
template <typename K, typename V>
class ThreadSafeCellMap
    : public ThreadSafeReactiveMap<K, V, Source<V>> {
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
    this->get_or_insert_handle(ctx, key,
                               [value](const K&) -> V { return value; });
  }
};

// A thread-safe **derived-slot** map: entries are `Computed<V>` minted lazily
// on access or eagerly via `materialize_all`.
template <typename K, typename V>
class ThreadSafeSlotMap
    : public ThreadSafeReactiveMap<K, V, Computed<V>> {
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

}  // namespace lazily

#endif  // LAZILY_THREAD_SAFE_REACTIVE_FAMILY_HPP
