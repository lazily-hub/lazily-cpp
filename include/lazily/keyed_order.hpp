#ifndef LAZILY_KEYED_ORDER_HPP
#define LAZILY_KEYED_ORDER_HPP

// `KeyedOrder<K, H>` — the present set plus its authoritative key order, with
// the atomic-move algebra (`#lzcellmove`).
//
// This is the **graph-agnostic** half of every `ReactiveMap` flavor. It holds no
// context, no factory, and no closure: only `K -> H` handle bookkeeping and the
// key list. That is exactly why ordering and atomic move can bind the
// single-threaded, thread-safe, and async flavors alike — the move algebra
// touches no entry handle and awaits nothing, so it is neither thread- nor
// async-coloured.
//
// What is *not* here, deliberately: reactivity. Membership and order
// *invalidation* is a graph write (`ctx.set` on a version cell), and each flavor
// must mint those cells on **its own** graph. A shared core cannot supply them.
// So each flavor owns a thin shell holding this core, its own interior-mutability
// wrapper, its own version cells, and its own `materialize` / `observe`.
//
// `entries` and `order` are kept in lockstep: every key in `entries` appears
// exactly once in `order` and vice versa. All mutators preserve that on their
// failure paths too — a partially-applied move that left a key in one but not
// the other is a real defect class (lazily-zig's `moveTo` could hit it on
// allocator failure). Reordering here cannot fail: it is a rotate within an
// existing vector, so there is no allocating error path to desync on.
//
// Rust reference: `lazily-rs/src/keyed_order.rs`.

#include <algorithm>
#include <cstddef>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lazily {

// What a present-set mutation did, so the caller knows which version cells to
// bump. A no-op mutation must bump nothing: bumping on a warm `insert` would
// invalidate every `len` / `contains_key` reader on a pure cache hit.
enum class MapMutation {
  // Nothing changed — a warm insert, or a remove of an absent key.
  None,
  // A new key joined the present set.
  Inserted,
  // A key left the present set.
  Removed,
};

inline bool mutation_changed(MapMutation m) { return m != MapMutation::None; }

// What an ordering move did. `Missing` and `Unchanged` are distinct because the
// public `move_*` methods report `false` for a missing key but `true` for a
// no-op move that "succeeded" — while neither may bump the order signal.
enum class MapMove {
  // The key (or anchor) is not in the present set. The move did not apply.
  Missing,
  // The key was already at the requested position. Applied, nothing to bump.
  Unchanged,
  // The order changed. Bump the order signal.
  Reordered,
};

// Whether the move applied at all (the `bool` the public API returns).
inline bool move_applied(MapMove m) { return m != MapMove::Missing; }

// Whether the order actually changed, i.e. whether to bump the order signal.
inline bool move_changed(MapMove m) { return m == MapMove::Reordered; }

template <typename K, typename H> class KeyedOrder {
public:
  KeyedOrder() = default;

  // -- reads (no graph involvement) ---------------------------------------

  std::optional<H> get(const K& key) const {
    auto it = entries_.find(key);
    if (it == entries_.end()) return std::nullopt;
    return it->second;
  }

  bool contains(const K& key) const { return entries_.count(key) > 0; }

  // The authoritative key list, in current order.
  const std::vector<K>& keys() const { return order_; }

  std::size_t len() const { return order_.size(); }

  // Current 0-based position of `key`, or `std::nullopt` if absent.
  std::optional<std::size_t> position(const K& key) const {
    for (std::size_t i = 0; i < order_.size(); ++i) {
      if (order_[i] == key) return i;
    }
    return std::nullopt;
  }

  // -- present-set mutations ----------------------------------------------

  // Insert `handle` under `key`, appending to the order. A warm key keeps its
  // existing handle (cell-identity: a key's node is stable for its lifetime),
  // and reports `None` so the caller bumps nothing.
  std::pair<H, MapMutation> insert(const K& key, H handle) {
    auto it = entries_.find(key);
    if (it != entries_.end()) return {it->second, MapMutation::None};
    entries_.emplace(key, handle);
    order_.push_back(key);
    return {handle, MapMutation::Inserted};
  }

  // Remove `key`, returning its handle so the caller can dispose the node on
  // its own graph. Disposal is per-flavor; the core never touches a handle.
  std::pair<std::optional<H>, MapMutation> remove(const K& key) {
    auto it = entries_.find(key);
    if (it == entries_.end()) return {std::nullopt, MapMutation::None};
    H handle = it->second;
    entries_.erase(it);
    order_.erase(std::remove(order_.begin(), order_.end(), key), order_.end());
    return {std::optional<H>(handle), MapMutation::Removed};
  }

  // -- the move algebra ----------------------------------------------------

  // Move `key` to `index`, clamped to `[0, len)`. The entry keeps the same
  // handle, its dependents, and its CRDT lineage — that is what separates a
  // reorder from a remove + re-mint.
  MapMove move_to(const K& key, std::size_t index) {
    auto from_opt = position(key);
    if (!from_opt) return MapMove::Missing;
    const std::size_t from = *from_opt;
    const std::size_t to = std::min(index, order_.size() - 1);
    if (from == to) return MapMove::Unchanged;
    rotate_within(from, to);
    return MapMove::Reordered;
  }

  // Move `key` to just before `anchor`.
  //
  // The target index is computed on the **pre-removal** list: when `key`
  // currently precedes `anchor`, lifting `key` out shifts `anchor` one slot
  // left, so the insertion point is `anchor_idx - 1`. Getting this wrong lands
  // the key on the far side of its anchor — the defect found in lazily-zig,
  // where `move_before("a", "d")` on `[a,b,c,d]` produced `[b,c,d,a]`.
  MapMove move_before(const K& key, const K& anchor) {
    auto anchor_idx = position(anchor);
    auto from = position(key);
    if (!anchor_idx || !from) return MapMove::Missing;
    const std::size_t target = (*from < *anchor_idx) ? *anchor_idx - 1 : *anchor_idx;
    return move_to(key, target);
  }

  // Move `key` to just after `anchor`. Same pre-removal reasoning.
  MapMove move_after(const K& key, const K& anchor) {
    auto anchor_idx = position(anchor);
    auto from = position(key);
    if (!anchor_idx || !from) return MapMove::Missing;
    const std::size_t target = (*from <= *anchor_idx) ? *anchor_idx : *anchor_idx + 1;
    return move_to(key, target);
  }

private:
  // Move the element at `from` to `to` without reallocating and without an
  // intermediate state where the key is absent from `order_`. A rotate cannot
  // fail, so `entries_` and `order_` can never desync here.
  void rotate_within(std::size_t from, std::size_t to) {
    const auto begin = order_.begin();
    const auto f = static_cast<std::ptrdiff_t>(from);
    const auto t = static_cast<std::ptrdiff_t>(to);
    if (from < to) {
      std::rotate(begin + f, begin + f + 1, begin + t + 1);
    } else {
      std::rotate(begin + t, begin + f, begin + f + 1);
    }
  }

  // Per-key node handles. Each entry is its own reactive node in the owning
  // graph; this core only stores the handle.
  std::unordered_map<K, H> entries_;
  // Authoritative key list. Insertion-ordered until an atomic move reorders it.
  std::vector<K> order_;
};

} // namespace lazily

#endif // LAZILY_KEYED_ORDER_HPP
