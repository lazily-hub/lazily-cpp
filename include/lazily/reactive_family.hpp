#ifndef LAZILY_REACTIVE_FAMILY_HPP
#define LAZILY_REACTIVE_FAMILY_HPP

// The unified keyed reactive family (`ReactiveFamily`) and its materialization
// mode (`#lzmatmode`).
//
// A family maps keys `K` to per-entry reactive nodes of a single handle kind:
//
//   - `CellHandle<V>` — an **input** cell. An input has no derivation to defer,
//     so it is **always materialized** (present at build under either mode).
//   - `SlotHandle<V>` — a **derived** slot. Derived nodes are what
//     materialization mode governs.
//
// # Materialization mode
//
// Materialization mode is **orthogonal** to entry kind: it fixes *when a derived
// node's backing storage is allocated*, not what it computes.
//
//   - `MaterializationMode::Eager` (**default**) — every derived node is
//     allocated at build time (the shared high-performance core; a read is a
//     direct node access).
//   - `MaterializationMode::Lazy` (opt-in) — a derived node is allocated on its
//     **first read** ("materialize on pull"), addressed by key. A never-read
//     derived cell is never allocated. Lazy is a keyed overlay on the eager
//     core, not a second engine: the first read builds exactly the node an
//     eager build would have, then caches it.
//
// Entry kind is orthogonal to mode (proved in `lazily-formal`'s `Materialization`
// module as `cell_entries_materialized_in_every_mode` /
// `slot_entries_deferred_under_lazy`): choosing lazy defers only slot entries,
// never cell ones. Across every mode the transparency law
// (`observe(build eager s, id) == observe(build lazy s, id) == s.val(id)`) holds.
//
// Spec: lazily-spec `cell-model.md` § "Materialization mode" +
// `conformance/materialization/*`. Formal: lazily-formal
// `LazilyFormal/Materialization.lean`. Rust reference: `src/reactive_family.rs`.

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <memory>
#include <unordered_map>
#include <vector>

#include <lazily/context.hpp>

namespace lazily {

// The two entry kinds a `ReactiveFamily` holds — the handle-kind axis, kept
// orthogonal to `MaterializationMode`. Mirrors `EntryKind` in lazily-formal.
enum class EntryKind {
  // An **input** cell (`CellHandle`) — always materialized, any mode.
  Cell,
  // A **derived** slot (`SlotHandle`) — materialized eagerly, or lazily on
  // first read.
  Slot,
};

// Materialization strategy for derived (slot) nodes. Mirrors `Mode` in
// lazily-formal; the default is `Eager` (`Mode.default = Mode.eager`).
enum class MaterializationMode {
  // Every declared derived node is allocated at build time. The default.
  Eager,
  // Derived nodes are allocated on first read (materialize on pull), keyed
  // rather than handle-addressed. An opt-in overlay on the eager core.
  Lazy,
};

// The default materialization mode. Implementations MUST default to eager.
inline constexpr MaterializationMode kDefaultMaterializationMode =
    MaterializationMode::Eager;

// Traits abstracting over the two family handle kinds — `CellHandle<V>` (input
// cells) and `SlotHandle<V>` (derived slots). Mirrors the sealed `FamilyHandle`
// trait in the Rust reference. Only these two specializations exist; bindings do
// not add new kinds.
template <typename H>
struct FamilyHandleTraits;  // primary template intentionally undefined

template <typename V>
struct FamilyHandleTraits<CellHandle<V>> {
  static constexpr EntryKind kind = EntryKind::Cell;

  // An input has no derivation: materialize by setting its value directly.
  template <typename Compute>
  static CellHandle<V> materialize(Context& ctx, Compute&& compute) {
    return ctx.template cell<V>(compute(ctx));
  }

  static V observe(const CellHandle<V>& h, Context& ctx) {
    return ctx.get_cell(h);
  }
};

template <typename V>
struct FamilyHandleTraits<SlotHandle<V>> {
  static constexpr EntryKind kind = EntryKind::Slot;

  // A derived node: the same node an eager build would allocate. `compute` is
  // stored as the slot's recomputation.
  template <typename Compute>
  static SlotHandle<V> materialize(Context& ctx, Compute&& compute) {
    return ctx.template computed<V>(std::forward<Compute>(compute));
  }

  static V observe(const SlotHandle<V>& h, Context& ctx) { return ctx.get(h); }
};

template <typename K, typename V, typename H>
struct ReactiveFamilyInner {
  MaterializationMode mode;
  // Canonical per-key value producer (a derived slot's recompute; an input
  // cell's initial value).
  std::function<V(const K&)> factory;
  // Currently-allocated entries (the "present" set). Grows on materialize, never
  // shrinks silently — deferral, not de-allocation.
  std::unordered_map<K, H> materialized;
  // First-materialization order of the present set (stable snapshot).
  std::vector<K> order;
};

// The unified keyed reactive family (`#lzmatmode`): keys `K` map to per-entry
// reactive nodes of handle kind `H` (`CellHandle<V>` for input cells,
// `SlotHandle<V>` for derived slots), allocated per its `MaterializationMode`.
//
// Cheap to copy (a `shared_ptr` to shared inner state) so it can be captured by
// compute/effect closures. Operations run against the owning `Context`, like the
// rest of lazily.
template <typename K, typename V, typename H>
class ReactiveFamily {
 public:
  using Handle = H;
  using Traits = FamilyHandleTraits<H>;

  // -- Builders --

  // Build an **eager** family: every declared key's node is allocated now. This
  // is the default mode (`MaterializationMode::Eager`).
  static ReactiveFamily eager(Context& ctx, std::vector<K> keys,
                              std::function<V(const K&)> factory) {
    return build(ctx, MaterializationMode::Eager, std::move(keys),
                 std::move(factory));
  }
  static ReactiveFamily eager(Context& ctx, std::initializer_list<K> keys,
                              std::function<V(const K&)> factory) {
    return eager(ctx, std::vector<K>(keys), std::move(factory));
  }

  // Build a **lazy** family: derived (slot) entries are deferred to first read;
  // input (cell) entries in `keys` are still materialized at build (cells are
  // always materialized). Pass empty `keys` for a purely on-demand slot family.
  static ReactiveFamily lazy(Context& ctx, std::vector<K> keys,
                             std::function<V(const K&)> factory) {
    return build(ctx, MaterializationMode::Lazy, std::move(keys),
                 std::move(factory));
  }
  static ReactiveFamily lazy(Context& ctx, std::initializer_list<K> keys,
                             std::function<V(const K&)> factory) {
    return lazy(ctx, std::vector<K>(keys), std::move(factory));
  }

  // Build a family in the **default** mode (eager). Alias for `eager`.
  static ReactiveFamily create(Context& ctx, std::vector<K> keys,
                               std::function<V(const K&)> factory) {
    return eager(ctx, std::move(keys), std::move(factory));
  }
  static ReactiveFamily create(Context& ctx, std::initializer_list<K> keys,
                               std::function<V(const K&)> factory) {
    return eager(ctx, std::vector<K>(keys), std::move(factory));
  }

  // -- Access --

  // Get the entry handle for `key`, materializing it on first access (the lazy
  // pull) and caching it. Under eager mode an entry is already present, so this
  // returns the cached handle.
  H get(Context& ctx, const K& key) { return materialize_key(ctx, key); }

  // Observe `key`'s value — the headline transparency law: the returned value is
  // identical under either mode. Materializes the entry if absent.
  V observe(Context& ctx, const K& key) {
    return Traits::observe(get(ctx, key), ctx);
  }

  // Whether `key` is currently materialized (present in the allocated set).
  // Non-reactive.
  bool is_present(const K& key) const {
    return inner_->materialized.count(key) > 0;
  }

  // The currently-materialized keys, in first-materialization order. The present
  // set only grows (deferral, not de-allocation).
  std::vector<K> present_keys() const { return inner_->order; }

  // Number of currently-materialized entries.
  size_t present_count() const { return inner_->order.size(); }

  // This family's materialization mode.
  MaterializationMode mode() const { return inner_->mode; }

  // This family's entry kind (`EntryKind::Cell` for a cell family,
  // `EntryKind::Slot` for a slot family).
  EntryKind entry_kind() const { return Traits::kind; }

 private:
  std::shared_ptr<ReactiveFamilyInner<K, V, H>> inner_;

  static ReactiveFamily build(Context& ctx, MaterializationMode mode,
                              std::vector<K> keys,
                              std::function<V(const K&)> factory) {
    ReactiveFamily fam;
    fam.inner_ = std::make_shared<ReactiveFamilyInner<K, V, H>>();
    fam.inner_->mode = mode;
    fam.inner_->factory = std::move(factory);
    for (auto& key : keys) {
      // buildEager materializes every node; buildLazy materializes only input
      // cells (`present := isInput`). A cell entry is always materialized
      // regardless of mode; a slot entry only under eager.
      if (Traits::kind == EntryKind::Cell ||
          mode == MaterializationMode::Eager) {
        fam.materialize_key(ctx, key);
      }
    }
    return fam;
  }

  H materialize_key(Context& ctx, const K& key) {
    auto it = inner_->materialized.find(key);
    if (it != inner_->materialized.end()) return it->second;  // warm.
    auto factory = inner_->factory;  // copy the shared producer into the closure.
    K k = key;
    H handle = Traits::materialize(
        ctx, [factory, k](Context&) -> V { return factory(k); });
    inner_->materialized.emplace(key, handle);
    inner_->order.push_back(key);
    return handle;
  }
};

}  // namespace lazily

#endif  // LAZILY_REACTIVE_FAMILY_HPP
