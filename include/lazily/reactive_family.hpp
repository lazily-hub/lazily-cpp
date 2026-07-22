#ifndef LAZILY_REACTIVE_FAMILY_HPP
#define LAZILY_REACTIVE_FAMILY_HPP

// The unified keyed reactive collection `ReactiveMap<K, V, H>` and its
// `CellMap` / `SlotMap` specializations (`#reactivemap`).
//
// `Context` addresses nodes by opaque `SlotId`. `ReactiveMap` adds a *keyed*
// layer on top: a hash collection whose **membership is itself reactive**, with
// one independently-tracked reactive node per entry.
//
// # One primitive, two specializations
//
// There is a single keyed primitive, generic over the entry's **handle kind**
// `H` (the `MapHandleTraits` trait, implemented for `Source<V>` input cells
// and `Computed<V>` derived slots):
//
//   - `CellMap<K, V>` = `ReactiveMap<K, V, Source<V>>` — **input-cell**
//     entries. Adds cell-only `set` and eager value-minting (`entry` /
//     `entry_with`).
//   - `SlotMap<K, V>` = `ReactiveMap<K, V, Computed<V>>` — **derived-slot**
//     entries. `get_or_insert_with` mints a slot on first access (**lazy
//     materialization**); a slot's value is derived, so `SlotMap` has **no
//     `set`**. Eager materialization is a pre-mint loop over the keyset
//     (`materialize_all`); lazy is mint-on-access. There is **no eager/lazy mode
//     flag**.
//
// The shared surface — `get_or_insert_with` / `remove` / `move_*` / membership /
// order / `keys` / `len` / `contains_key` — lives on the generic `ReactiveMap`.
// `set` and eager value-minting are the `CellMap`-only specialization; the
// pre-mint eager helper is the `SlotMap`-only specialization.
//
// Each entry is its own reactive node, so a reader that depends on entry `a` is
// not invalidated when entry `b` changes. Membership (the set of keys) is
// tracked by a dedicated version cell, so `keys` / `len` readers recompute only
// when keys are added, removed, or (for `keys`) reordered.
//
// Spec: lazily-spec `cell-model.md` § "Keyed cell collections". Rust reference:
// `lazily-rs/src/cell_family.rs`.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include <lazily/context.hpp>
#include <lazily/cell.hpp>

namespace lazily {

// Which kind of reactive node a `ReactiveMap` entry is — the handle-kind axis the
// map abstracts over. Mirrors `EntryKind` in lazily-formal.
enum class EntryKind {
  // An **input** cell (`Source`) — always materialized on `get`.
  Cell,
  // A **derived** slot (`Computed`) — materialized eagerly (pre-mint) or lazily
  // on first read.
  Slot,
};

// Traits abstracting over the two map handle kinds — `Source<V>` (input
// cells) and `Computed<V>` (derived slots). Mirrors the sealed `MapHandle`
// trait in the Rust reference. Only these two specializations exist; bindings do
// not add new kinds.
template <typename H>
struct MapHandleTraits;  // primary template intentionally undefined

template <typename V>
struct MapHandleTraits<Source<V>> {
  static constexpr EntryKind kind = EntryKind::Cell;

  // An input has no derivation: materialize by setting its value directly.
  template <typename Compute>
  static Source<V> materialize(Context& ctx, Compute&& compute) {
    return ctx.template source<V>(compute(ctx));
  }

  template <typename Cx>
  static V observe(const Source<V>& h, Cx& ctx) {
    return ctx.get(h);
  }

  // Detach the entry's node on removal — clear its cached value and dependents.
  static void clear_dependents(const Source<V>& h, Context& ctx) {
    h.clear_dependents(ctx);
  }
};

template <typename V>
struct MapHandleTraits<Computed<V>> {
  static constexpr EntryKind kind = EntryKind::Slot;

  // A derived node: the same node an eager pre-mint would allocate. `compute` is
  // stored as the slot's recomputation.
  template <typename Compute>
  static Computed<V> materialize(Context& ctx, Compute&& compute) {
    return ctx.template slot<V>(std::forward<Compute>(compute));
  }

  template <typename Cx>
  static V observe(const Computed<V>& h, Cx& ctx) { return ctx.get(h); }

  static void clear_dependents(const Computed<V>& h, Context& ctx) {
    h.clear(ctx);
  }
};

template <typename K, typename H>
struct ReactiveMapInner {
  // Per-key reactive nodes. Each entry is its own reactive node.
  std::unordered_map<K, H> entries;
  // Insertion-ordered authoritative key list (snapshot returned by `keys`).
  std::vector<K> order;
  // Reactive *set-membership* signal, bumped only when the set of keys changes.
  Source<uint64_t> membership;
  // Untracked mirror of the membership version.
  uint64_t version;
  // Reactive *order* signal, bumped on add/remove and on move/reorder.
  Source<uint64_t> order_signal;
  // Untracked mirror of the order version.
  uint64_t order_version;
};

// A keyed reactive collection generic over the entry handle kind `H`: a hash map
// of `K -> H` with reactive membership and independently-tracked per-entry nodes.
//
// Cheap to copy (a `shared_ptr` to shared inner state) so it can be captured by
// compute/effect closures. Operations run against the owning `Context`.
//
// The two specializations a binding exposes are `CellMap` (input cells) and
// `SlotMap` (derived slots).
template <typename K, typename V, typename H>
class ReactiveMap {
 public:
  using Handle = H;
  using Traits = MapHandleTraits<H>;

  // Create an empty collection bound to `ctx`.
  explicit ReactiveMap(Context& ctx)
      : inner_(std::make_shared<ReactiveMapInner<K, H>>()) {
    inner_->membership = ctx.source(uint64_t(0));
    inner_->version = 0;
    inner_->order_signal = ctx.source(uint64_t(0));
    inner_->order_version = 0;
  }

  // -- Shared surface --

  // Get the value at `key`, minting the entry via `factory(key)` first if the key
  // is absent — the mint-on-access recipe. For a `SlotMap` this is the lazy
  // materialization pull; for a `CellMap` it seeds an input cell. Bumps reactive
  // membership only on insert.
  V get_or_insert_with(Context& ctx, const K& key,
                       std::function<V(const K&)> factory) {
    auto it = inner_->entries.find(key);
    if (it != inner_->entries.end()) return Traits::observe(it->second, ctx);
    K k = key;
    H handle =
        mint_with(ctx, key, [factory, k](auto&) -> V { return factory(k); });
    return Traits::observe(handle, ctx);
  }

  // Return the existing entry handle for `key`, or `std::nullopt`. Non-reactive.
  std::optional<H> handle(const K& key) const {
    auto it = inner_->entries.find(key);
    if (it == inner_->entries.end()) return std::nullopt;
    return it->second;
  }

  // Read the value at `key` if present. Reactive on that entry only.
  template <typename Cx>
  std::optional<V> get(Cx& ctx, const K& key) {
    auto it = inner_->entries.find(key);
    if (it == inner_->entries.end()) return std::nullopt;
    return Traits::observe(it->second, ctx);
  }

  // Remove `key`'s entry. Bumps reactive membership and clears the removed
  // entry's dependents. Returns whether the key was present.
  bool remove(Context& ctx, const K& key) {
    auto it = inner_->entries.find(key);
    if (it == inner_->entries.end()) return false;
    H handle = it->second;
    inner_->entries.erase(it);
    inner_->order.erase(
        std::remove(inner_->order.begin(), inner_->order.end(), key),
        inner_->order.end());
    Traits::clear_dependents(handle, ctx);
    bump_membership(ctx);
    return true;
  }

  // Reactive snapshot of the keys in their current order. Subscribes the caller
  // to order changes (add/remove and move/reorder), not to per-entry value
  // changes.
  template <typename Cx>
  std::vector<K> keys(Cx& ctx) {
    (void)ctx.get(inner_->order_signal);
    return inner_->order;
  }

  // The currently-materialized (present) keys, in first-materialization order.
  // Non-reactive; the present set only grows.
  std::vector<K> present_keys() const { return inner_->order; }

  // Number of currently-materialized (present) entries. Non-reactive.
  size_t present_count() const { return inner_->order.size(); }

  // Whether `key` is currently materialized (present in the allocated set).
  // Non-reactive.
  bool is_present(const K& key) const {
    return inner_->entries.count(key) > 0;
  }

  // Current 0-based position of `key` in the order, or `std::nullopt` if absent.
  // Non-reactive.
  std::optional<size_t> position(const K& key) const {
    for (size_t i = 0; i < inner_->order.size(); ++i) {
      if (inner_->order[i] == key) return i;
    }
    return std::nullopt;
  }

  // Atomically move `key` to `index` in the order (`#lzcellmove`). The entry
  // keeps the same node, its dependents, and its CRDT lineage. Only the order
  // signal is bumped. `index` is clamped to `[0, len)`.
  bool move_to(Context& ctx, const K& key, size_t index) {
    auto from_opt = position(key);
    if (!from_opt) return false;
    size_t from = *from_opt;
    size_t to = std::min(index, inner_->order.size() - 1);
    if (from == to) return true;
    K k = std::move(inner_->order[from]);
    inner_->order.erase(inner_->order.begin() + from);
    inner_->order.insert(inner_->order.begin() + to, std::move(k));
    bump_order(ctx);
    return true;
  }

  // Atomically move `key` to just before `anchor` in the order (`#lzcellmove`).
  bool move_before(Context& ctx, const K& key, const K& anchor) {
    auto anchor_idx = position(anchor);
    auto from = position(key);
    if (!anchor_idx || !from) return false;
    size_t target = (*from < *anchor_idx) ? *anchor_idx - 1 : *anchor_idx;
    return move_to(ctx, key, target);
  }

  // Atomically move `key` to just after `anchor` in the order (`#lzcellmove`).
  bool move_after(Context& ctx, const K& key, const K& anchor) {
    auto anchor_idx = position(anchor);
    auto from = position(key);
    if (!anchor_idx || !from) return false;
    size_t target = (*from <= *anchor_idx) ? *anchor_idx : *anchor_idx + 1;
    return move_to(ctx, key, target);
  }

  // Reactive entry count. Subscribes the caller to membership changes only.
  template <typename Cx>
  size_t len(Cx& ctx) {
    (void)ctx.get(inner_->membership);
    return inner_->order.size();
  }

  // Reactive emptiness check. Subscribes the caller to membership changes.
  template <typename Cx>
  bool is_empty(Cx& ctx) { return len(ctx) == 0; }

  // Reactive membership test for `key`. Subscribes the caller to membership
  // changes (add/remove of any key), not to value changes.
  template <typename Cx>
  bool contains_key(Cx& ctx, const K& key) {
    (void)ctx.get(inner_->membership);
    return inner_->entries.count(key) > 0;
  }

  // Non-reactive count. Does not subscribe the caller to anything.
  size_t len_untracked() const { return inner_->order.size(); }

  // This map's entry kind (`EntryKind::Cell` for a `CellMap`, `EntryKind::Slot`
  // for a `SlotMap`).
  EntryKind entry_kind() const { return Traits::kind; }

 protected:
  std::shared_ptr<ReactiveMapInner<K, H>> inner_;

  // Mint the entry node for `key` (via `Traits::materialize`) on first access,
  // caching the handle and bumping reactive membership. Re-minting an existing
  // key returns the cached handle.
  //
  // `compute` is a generic `(auto&)` value factory. For a `SlotMap` it is
  // stored as the derived slot's recompute (invoked with a `Compute&`, the
  // value-threaded tracking surface — `#lzcellkernel`); for a `CellMap` it is
  // evaluated once eagerly against the `Context&` to seed the input cell.
  template <typename ComputeFn>
  H mint_with(Context& ctx, const K& key, ComputeFn compute) {
    auto it = inner_->entries.find(key);
    if (it != inner_->entries.end()) return it->second;  // warm.
    H handle = Traits::materialize(ctx, compute);
    inner_->entries.emplace(key, handle);
    inner_->order.push_back(key);
    bump_membership(ctx);
    return handle;
  }

  // Bump the *order* signal (invalidates `keys` readers).
  void bump_order(Context& ctx) {
    inner_->order_version++;
    ctx.set(inner_->order_signal, inner_->order_version);
  }

  // Bump set-membership (invalidates `len`/`contains_key` readers). Always paired
  // with an order bump because add/remove change order too.
  void bump_membership(Context& ctx) {
    inner_->version++;
    ctx.set(inner_->membership, inner_->version);
    bump_order(ctx);
  }
};

// A keyed **input-cell** collection: every entry is a settable `Source<V>`.
//
// The `CellMap` specialization of `ReactiveMap` adds cell-only `set` and eager
// value-minting (`entry` / `entry_with`) on top of the shared reactive keyed
// surface.
template <typename K, typename V>
class CellMap : public ReactiveMap<K, V, Source<V>> {
 public:
  using Base = ReactiveMap<K, V, Source<V>>;
  using Base::Base;

  // Return the value cell for `key`, minting it with `default_fn` on first
  // access. Adding a new key bumps reactive membership; re-fetching does not.
  Source<V> entry_with(Context& ctx, const K& key,
                           std::function<V()> default_fn) {
    auto h = this->handle(key);
    if (h) return *h;
    V value = default_fn();
    return this->mint_with(ctx, key, [value](auto&) -> V { return value; });
  }

  // Return the value cell for `key`, minting it with `default_val` on first
  // access. Convenience wrapper over `entry_with`.
  Source<V> entry(Context& ctx, const K& key, V default_val) {
    return entry_with(ctx, key, [default_val]() { return default_val; });
  }

  // Set the value at `key`, inserting a new entry (and bumping membership) if it
  // does not exist yet. Cell-only: an input is settable; a `SlotMap` slot is not.
  void set(Context& ctx, const K& key, V value) {
    auto h = this->handle(key);
    if (h) {
      ctx.set(*h, std::move(value));
      return;
    }
    entry_with(ctx, key, [value]() { return value; });
  }

  // Reconcile the map to `new_seq` (keyed, move-minimized). Declared here;
  // defined out-of-line in collections.hpp where the reconcile machinery lives.
  void reconcile(Context& ctx, const std::vector<std::pair<K, V>>& new_seq);
};

// A keyed **derived-slot** collection: every entry is a `Computed<V>` whose
// value is derived. `get_or_insert_with` mints a slot on first access (lazy
// materialization); `materialize_all` pre-mints the keyset (eager). A slot's
// value is derived, so `SlotMap` has **no `set`**.
template <typename K, typename V>
class SlotMap : public ReactiveMap<K, V, Computed<V>> {
 public:
  using Base = ReactiveMap<K, V, Computed<V>>;
  using Base::Base;

  // **Eager materialization**: pre-mint a derived slot for every key in `keys`
  // via `factory`, up front. Observationally identical to minting each key lazily
  // on first read — it only changes *when* the nodes are allocated.
  void materialize_all(Context& ctx, const std::vector<K>& keys,
                       std::function<V(const K&)> factory) {
    for (const auto& key : keys) this->get_or_insert_with(ctx, key, factory);
  }
  void materialize_all(Context& ctx, std::initializer_list<K> keys,
                       std::function<V(const K&)> factory) {
    materialize_all(ctx, std::vector<K>(keys), std::move(factory));
  }
};

}  // namespace lazily

#endif  // LAZILY_REACTIVE_FAMILY_HPP
