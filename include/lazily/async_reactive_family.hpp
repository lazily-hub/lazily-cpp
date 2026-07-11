#ifndef LAZILY_ASYNC_REACTIVE_FAMILY_HPP
#define LAZILY_ASYNC_REACTIVE_FAMILY_HPP

// Async keyed reactive family (`AsyncReactiveFamily`, `#lzmatmode`, async flavor).
//
// The `AsyncContext` analog of `ReactiveFamily`: keys `K` map to per-entry async
// reactive nodes (`AsyncCellHandle<V>` input cells / `AsyncSlotHandle<V>` derived
// slots) allocated per the family's `MaterializationMode`. Like
// `ThreadSafeReactiveFamily` it guards its present-set state behind a
// `std::mutex`, so it can live in a cross-task owner.
//
// The eager/lazy contract and present-set monotonicity are identical to the
// single-threaded family. The transparency law is **eventual**: an async derived
// slot read is empty (`std::nullopt`) while pending and resolves to the canonical
// value — so `observe` returns `std::optional<V>`. Input cells are always resolved
// (`observe` returns a value). Drive a slot to resolution with `get_async()` on
// the handle returned by `get`.
//
// To keep the three families API-parallel the per-key factory is the same sync
// `V(const K&)` as the sync/thread-safe families; a derived slot wraps it as its
// async recomputation. Mirrors the async materialization case in lazily-spec and
// the `AsyncMaterialization` proofs (eventual transparency) in lazily-formal.
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

// Traits abstracting over the two async family handle kinds — `AsyncCellHandle<V>`
// (input cells, always resolved) and `AsyncSlotHandle<V>` (derived slots, resolve
// asynchronously). The `AsyncContext` analog of `FamilyHandleTraits`.
template <typename H>
struct AsyncFamilyHandleTraits;  // primary template intentionally undefined

template <typename V>
struct AsyncFamilyHandleTraits<AsyncCellHandle<V>> {
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
struct AsyncFamilyHandleTraits<AsyncSlotHandle<V>> {
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

template <typename K, typename V, typename H>
struct AsyncReactiveFamilyInner {
  MaterializationMode mode;
  std::function<V(const K&)> factory;
  std::mutex state_mutex;
  std::unordered_map<K, H> materialized;
  std::vector<K> order;
};

// The async unified keyed reactive family (`#lzmatmode`): keys `K` map to
// per-entry async reactive nodes of handle kind `H` (`AsyncCellHandle<V>` input
// cells, `AsyncSlotHandle<V>` derived slots), allocated per its
// `MaterializationMode`.
//
// Cheap to copy (a `shared_ptr` to shared inner state). See the eager/lazy
// contract and the eventual-transparency law above.
template <typename K, typename V, typename H>
class AsyncReactiveFamily {
 public:
  using Handle = H;
  using Traits = AsyncFamilyHandleTraits<H>;

  // -- Builders --

  static AsyncReactiveFamily eager(AsyncContext& ctx, std::vector<K> keys,
                                   std::function<V(const K&)> factory) {
    return build(ctx, MaterializationMode::Eager, std::move(keys),
                 std::move(factory));
  }
  static AsyncReactiveFamily eager(AsyncContext& ctx,
                                   std::initializer_list<K> keys,
                                   std::function<V(const K&)> factory) {
    return eager(ctx, std::vector<K>(keys), std::move(factory));
  }

  static AsyncReactiveFamily lazy(AsyncContext& ctx, std::vector<K> keys,
                                  std::function<V(const K&)> factory) {
    return build(ctx, MaterializationMode::Lazy, std::move(keys),
                 std::move(factory));
  }
  static AsyncReactiveFamily lazy(AsyncContext& ctx,
                                  std::initializer_list<K> keys,
                                  std::function<V(const K&)> factory) {
    return lazy(ctx, std::vector<K>(keys), std::move(factory));
  }

  // Build a family in the **default** mode (eager). Alias for `eager`.
  static AsyncReactiveFamily create(AsyncContext& ctx, std::vector<K> keys,
                                    std::function<V(const K&)> factory) {
    return eager(ctx, std::move(keys), std::move(factory));
  }
  static AsyncReactiveFamily create(AsyncContext& ctx,
                                    std::initializer_list<K> keys,
                                    std::function<V(const K&)> factory) {
    return eager(ctx, std::vector<K>(keys), std::move(factory));
  }

  // -- Access --

  // Get the entry handle for `key`, materializing it on first access. For a slot
  // family this is the `AsyncSlotHandle` to drive with `get_async()`.
  H get(AsyncContext& ctx, const K& key) { return materialize_key(ctx, key); }

  // Non-blocking observe: a value for a cell or resolved slot, `std::nullopt`
  // for a pending slot. Eventual-transparency law: once resolved, this equals
  // the canonical value under either mode.
  std::optional<V> observe(AsyncContext& ctx, const K& key) {
    return Traits::observe(get(ctx, key), ctx);
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

  MaterializationMode mode() const { return inner_->mode; }
  EntryKind entry_kind() const { return Traits::kind; }

 private:
  std::shared_ptr<AsyncReactiveFamilyInner<K, V, H>> inner_;

  static AsyncReactiveFamily build(AsyncContext& ctx, MaterializationMode mode,
                                   std::vector<K> keys,
                                   std::function<V(const K&)> factory) {
    AsyncReactiveFamily fam;
    fam.inner_ = std::make_shared<AsyncReactiveFamilyInner<K, V, H>>();
    fam.inner_->mode = mode;
    fam.inner_->factory = std::move(factory);
    for (auto& key : keys) {
      if (Traits::kind == EntryKind::Cell ||
          mode == MaterializationMode::Eager) {
        fam.materialize_key(ctx, key);
      }
    }
    return fam;
  }

  H materialize_key(AsyncContext& ctx, const K& key) {
    {
      std::lock_guard<std::mutex> g(inner_->state_mutex);
      auto it = inner_->materialized.find(key);
      if (it != inner_->materialized.end()) return it->second;  // warm.
    }
    H handle = Traits::materialize(ctx, key, inner_->factory);
    std::lock_guard<std::mutex> g(inner_->state_mutex);
    // First writer wins on a race so the key keeps a stable handle.
    auto it = inner_->materialized.find(key);
    if (it != inner_->materialized.end()) return it->second;
    inner_->materialized.emplace(key, handle);
    inner_->order.push_back(key);
    return handle;
  }
};

// An async **input-cell** family: every entry is an always-resolved
// `AsyncCellHandle<V>`.
template <typename K, typename V>
using AsyncCellFamily = AsyncReactiveFamily<K, V, AsyncCellHandle<V>>;

// An async **derived-slot** family: entries are `AsyncSlotHandle<V>` governed by
// the family's `MaterializationMode`, resolved via `get_async()`.
template <typename K, typename V>
using AsyncSlotFamily = AsyncReactiveFamily<K, V, AsyncSlotHandle<V>>;

}  // namespace lazily

#endif  // LAZILY_ASYNC_REACTIVE_FAMILY_HPP
