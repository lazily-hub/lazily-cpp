#ifndef LAZILY_REACTIVE_FAMILY_HPP
#define LAZILY_REACTIVE_FAMILY_HPP

// The unified keyed reactive collection `ReactiveMap<K, V, H>` and its
// `SourceMap` / `ComputedMap` specializations (`#reactivemap`).
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
//   - `SourceMap<K, V>` = `ReactiveMap<K, V, Source<V>>` — **input-cell**
//     entries. Adds cell-only `set` and eager value-minting (`entry` /
//     `entry_with`).
//   - `ComputedMap<K, V>` = `ReactiveMap<K, V, Computed<V>>` — **derived-slot**
//     entries. `get_or_insert_with` mints a slot on first access (**lazy
//     materialization**); a slot's value is derived, so `ComputedMap` has **no
//     `set`**. Eager materialization is a pre-mint loop over the keyset
//     (`materialize_all`); lazy is mint-on-access. There is **no eager/lazy mode
//     flag**.
//
// The shared surface — `get_or_insert_with` / `remove` / `move_*` / membership /
// order / `keys` / `len` / `contains_key` — lives on the generic `ReactiveMap`.
// `set` and eager value-minting are the `SourceMap`-only specialization; the
// pre-mint eager helper is the `ComputedMap`-only specialization.
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

#include <lazily/cell.hpp>
#include <lazily/context.hpp>
#include <lazily/keyed_order.hpp>

namespace lazily {

// Which kind of reactive node a `ReactiveMap` entry is — the handle-kind axis the
// map abstracts over. Mirrors `EntryKind` in lazily-formal.
//
// The members carry the v2 kernel's node-kind names. There is deliberately NO
// deprecated alias for the pre-v2 spellings `EntryKind::Cell` / `EntryKind::Slot`:
// C++ has no way to add a name inside a scoped enum's scope after the fact, so
// keeping those spellings compiling would mean turning `EntryKind` from an
// `enum class` into a wrapper class — which would break `switch`, break
// `static_cast` to the underlying type, and change the very values this rename
// must leave alone. A one-line compile error naming the new member is strictly
// better than that. (lazily-rs can keep `EntryKind::Cell` as a deprecated
// associated const because Rust allows constants in a type's namespace; C++
// enum classes have no equivalent.)
enum class EntryKind {
  // An **input** cell (`Source`) — always materialized on `get`.
  Source,
  // A **derived** slot (`Computed`) — materialized eagerly (pre-mint) or lazily
  // on first read.
  Computed,
};

// Traits abstracting over the two map handle kinds — `Source<V>` (input
// cells) and `Computed<V>` (derived slots). Mirrors the sealed `MapHandle`
// trait in the Rust reference. Only these two specializations exist; bindings do
// not add new kinds.
template <typename H> struct MapHandleTraits; // primary template intentionally undefined

template <typename V> struct MapHandleTraits<Source<V>> {
  static constexpr EntryKind kind = EntryKind::Source;

  // An input has no derivation: materialize by setting its value directly.
  template <typename Compute> static Source<V> materialize(Context& ctx, Compute&& compute) {
    return ctx.template source<V>(compute(ctx));
  }

  template <typename Cx> static V observe(const Source<V>& h, Cx& ctx) { return ctx.get(h); }

  // Detach the entry's node on removal — clear its cached value and dependents.
  static void clear_dependents(const Source<V>& h, Context& ctx) { h.clear_dependents(ctx); }
};

template <typename V> struct MapHandleTraits<Computed<V>> {
  static constexpr EntryKind kind = EntryKind::Computed;

  // A derived node: the same node an eager pre-mint would allocate. `compute` is
  // stored as the slot's recomputation.
  template <typename Compute> static Computed<V> materialize(Context& ctx, Compute&& compute) {
    return ctx.template slot<V>(std::forward<Compute>(compute));
  }

  template <typename Cx> static V observe(const Computed<V>& h, Cx& ctx) { return ctx.get(h); }

  static void clear_dependents(const Computed<V>& h, Context& ctx) { h.clear(ctx); }
};

template <typename K, typename H> struct ReactiveMapInner {
  // Present set + key order + the move algebra. Graph-agnostic and shared with
  // the thread-safe and async flavors; see `keyed_order.hpp`.
  KeyedOrder<K, H> keyed;
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
// The two specializations a binding exposes are `SourceMap` (input cells) and
// `ComputedMap` (derived slots).
template <typename K, typename V, typename H> class ReactiveMap {
public:
  using Handle = H;
  using Traits = MapHandleTraits<H>;

  // Create an empty collection bound to `ctx`.
  explicit ReactiveMap(Context& ctx) : inner_(std::make_shared<ReactiveMapInner<K, H>>()) {
    inner_->membership = ctx.source(uint64_t(0));
    inner_->version = 0;
    inner_->order_signal = ctx.source(uint64_t(0));
    inner_->order_version = 0;
  }

  // -- Shared surface --

  // Get the value at `key`, minting the entry via `factory(key)` first if the key
  // is absent — the mint-on-access recipe. For a `ComputedMap` this is the lazy
  // materialization pull; for a `SourceMap` it seeds an input cell. Bumps reactive
  // membership only on insert.
  V get_or_insert_with(Context& ctx, const K& key, std::function<V(const K&)> factory) {
    if (auto warm = inner_->keyed.get(key)) return Traits::observe(*warm, ctx);
    K k = key;
    H handle = mint_with(ctx, key, [factory, k](auto&) -> V { return factory(k); });
    return Traits::observe(handle, ctx);
  }

  // Return the existing entry handle for `key`, or `std::nullopt`. Non-reactive.
  std::optional<H> handle(const K& key) const { return inner_->keyed.get(key); }

  // Read the value at `key` if present. Reactive on that entry only.
  template <typename Cx> std::optional<V> get(Cx& ctx, const K& key) {
    auto h = inner_->keyed.get(key);
    if (!h) return std::nullopt;
    return Traits::observe(*h, ctx);
  }

  // Remove `key`'s entry. Bumps reactive membership and clears the removed
  // entry's dependents. Returns whether the key was present.
  bool remove(Context& ctx, const K& key) {
    auto [handle, mutation] = inner_->keyed.remove(key);
    if (!mutation_changed(mutation)) return false;
    Traits::clear_dependents(*handle, ctx);
    bump_membership(ctx);
    return true;
  }

  // Reactive snapshot of the keys in their current order. Subscribes the caller
  // to order changes (add/remove and move/reorder), not to per-entry value
  // changes.
  template <typename Cx> std::vector<K> keys(Cx& ctx) {
    (void)ctx.get(inner_->order_signal);
    return inner_->keyed.keys();
  }

  // The currently-materialized (present) keys, in first-materialization order.
  // Non-reactive; the present set only grows.
  std::vector<K> present_keys() const { return inner_->keyed.keys(); }

  // Number of currently-materialized (present) entries. Non-reactive.
  size_t present_count() const { return inner_->keyed.len(); }

  // Whether `key` is currently materialized (present in the allocated set).
  // Non-reactive.
  bool is_present(const K& key) const { return inner_->keyed.contains(key); }

  // Current 0-based position of `key` in the order, or `std::nullopt` if absent.
  // Non-reactive.
  std::optional<size_t> position(const K& key) const { return inner_->keyed.position(key); }

  // Atomically move `key` to `index` in the order (`#lzcellmove`). The entry
  // keeps the same node, its dependents, and its CRDT lineage. Only the order
  // signal is bumped. `index` is clamped to `[0, len)`.
  bool move_to(Context& ctx, const K& key, size_t index) {
    return apply_move(ctx, inner_->keyed.move_to(key, index));
  }

  // Atomically move `key` to just before `anchor` in the order (`#lzcellmove`).
  bool move_before(Context& ctx, const K& key, const K& anchor) {
    return apply_move(ctx, inner_->keyed.move_before(key, anchor));
  }

  // Atomically move `key` to just after `anchor` in the order (`#lzcellmove`).
  bool move_after(Context& ctx, const K& key, const K& anchor) {
    return apply_move(ctx, inner_->keyed.move_after(key, anchor));
  }

  // Reactive entry count. Subscribes the caller to membership changes only.
  template <typename Cx> size_t len(Cx& ctx) {
    (void)ctx.get(inner_->membership);
    return inner_->keyed.len();
  }

  // Reactive emptiness check. Subscribes the caller to membership changes.
  template <typename Cx> bool is_empty(Cx& ctx) { return len(ctx) == 0; }

  // Reactive membership test for `key`. Subscribes the caller to membership
  // changes (add/remove of any key), not to value changes.
  template <typename Cx> bool contains_key(Cx& ctx, const K& key) {
    (void)ctx.get(inner_->membership);
    return inner_->keyed.contains(key);
  }

  // Non-reactive count. Does not subscribe the caller to anything.
  size_t len_untracked() const { return inner_->keyed.len(); }

  // This map's entry kind (`EntryKind::Source` for a `SourceMap`, `EntryKind::Computed`
  // for a `ComputedMap`).
  EntryKind entry_kind() const { return Traits::kind; }

protected:
  std::shared_ptr<ReactiveMapInner<K, H>> inner_;

  // Mint the entry node for `key` (via `Traits::materialize`) on first access,
  // caching the handle and bumping reactive membership. Re-minting an existing
  // key returns the cached handle.
  //
  // `compute` is a generic `(auto&)` value factory. For a `ComputedMap` it is
  // stored as the derived slot's recompute (invoked with a `Compute&`, the
  // value-threaded tracking surface — `#lzcellkernel`); for a `SourceMap` it is
  // evaluated once eagerly against the `Context&` to seed the input cell.
  template <typename ComputeFn> H mint_with(Context& ctx, const K& key, ComputeFn compute) {
    if (auto warm = inner_->keyed.get(key)) return *warm; // warm.
    H handle = Traits::materialize(ctx, compute);
    auto [stored, mutation] = inner_->keyed.insert(key, handle);
    if (mutation_changed(mutation)) bump_membership(ctx);
    return stored;
  }

  // Bump the order signal only when the order actually changed. A no-op move
  // still reports success to the caller but must invalidate no reader.
  bool apply_move(Context& ctx, MapMove outcome) {
    if (!move_applied(outcome)) return false;
    if (move_changed(outcome)) bump_order(ctx);
    return true;
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
// The `SourceMap` specialization of `ReactiveMap` adds cell-only `set` and eager
// value-minting (`entry` / `entry_with`) on top of the shared reactive keyed
// surface.
template <typename K, typename V> class SourceMap : public ReactiveMap<K, V, Source<V>> {
public:
  using Base = ReactiveMap<K, V, Source<V>>;
  using Base::Base;

  // Return the value cell for `key`, minting it with `default_fn` on first
  // access. Adding a new key bumps reactive membership; re-fetching does not.
  Source<V> entry_with(Context& ctx, const K& key, std::function<V()> default_fn) {
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
  // does not exist yet. Cell-only: an input is settable; a `ComputedMap` slot is not.
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

// Exact-key dependency state carried by one stable per-key source.
template <typename V> struct DependencyAvailability {
  std::optional<V> value;

  static DependencyAvailability unavailable() { return {}; }
  static DependencyAvailability available(V value) {
    return DependencyAvailability{std::move(value)};
  }

  bool operator==(const DependencyAvailability& other) const { return value == other.value; }
  bool operator!=(const DependencyAvailability& other) const { return !(*this == other); }
};

// A keyed family whose absent state is reactively observable.
template <typename K, typename V>
class DependencyMap : public SourceMap<K, DependencyAvailability<V>> {
public:
  using Availability = DependencyAvailability<V>;
  using Base = SourceMap<K, Availability>;
  using Base::Base;

  Availability observe_dependency(Context& ctx, const K& key) {
    auto handle = this->entry(ctx, key, Availability::unavailable());
    return ctx.get(handle);
  }

  Availability observe_dependency(Compute& compute, const K& key) {
    auto handle = this->entry(compute.untracked(), key, Availability::unavailable());
    return compute.get(handle);
  }

  void publish(Context& ctx, const K& key, V value) {
    this->set(ctx, key, Availability::available(std::move(value)));
  }

  void unpublish(Context& ctx, const K& key) { this->set(ctx, key, Availability::unavailable()); }
};

// A keyed **derived-slot** collection: every entry is a `Computed<V>` whose
// value is derived. `get_or_insert_with` mints a slot on first access (lazy
// materialization); `materialize_all` pre-mints the keyset (eager). A slot's
// value is derived, so `ComputedMap` has **no `set`**.
template <typename K, typename V> class ComputedMap : public ReactiveMap<K, V, Computed<V>> {
public:
  using Base = ReactiveMap<K, V, Computed<V>>;
  using Base::Base;

  // **Eager materialization**: pre-mint a derived slot for every key in `keys`
  // via `factory`, up front. Observationally identical to minting each key lazily
  // on first read — it only changes *when* the nodes are allocated.
  void materialize_all(Context& ctx, const std::vector<K>& keys,
                       std::function<V(const K&)> factory) {
    for (const auto& key : keys)
      this->get_or_insert_with(ctx, key, factory);
  }
  void materialize_all(Context& ctx, std::initializer_list<K> keys,
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
using CellMap [[deprecated("renamed to SourceMap")]] = SourceMap<K, V>;
template <typename K, typename V>
using SlotMap [[deprecated("renamed to ComputedMap")]] = ComputedMap<K, V>;

} // namespace lazily

#endif // LAZILY_REACTIVE_FAMILY_HPP
