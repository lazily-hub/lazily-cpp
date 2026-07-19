#ifndef LAZILY_CONTEXT_HPP
#define LAZILY_CONTEXT_HPP

#include <lazily/types.hpp>
#include <lazily/small_fn.hpp>
#include <lazily/small_vec.hpp>
#include <lazily/rc_ptr.hpp>

#include <cassert>
#include <deque>
#include <memory>
#include <optional>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace lazily {

// ── Forward declarations ──
template <typename Traits = RcTraits>
class ContextImpl;

/// Default Context — uses non-atomic RcPtr (≈ Rust Rc).
using Context = ContextImpl<RcTraits>;

using EdgeVec = SmallVec<SlotId, 2>;
// Dogfood the library's own SmallFn primitive for per-effect cleanup closures.
// SmallFn's null state is `vtable_ == nullptr`, so no optional<> wrapper is
// needed. Capturing cleanups up to 32 bytes inline → no per-effect heap alloc
// (#lzcppsmallfncleanup).
using CleanupFn = SmallFn<void(), 32>;

// ── Dependency-edge set (#lzspecedgeindex) ──
//
// Edge registration must be idempotent, so every insert tests membership. A
// linear scan makes that O(degree), and since a recompute removes and
// re-registers every edge, building or notifying a width-N fan-out costs
// ~N^2/2 comparisons. At N=4096 that is a visible regression; at N=1e6 it does
// not finish. Above a measured degree threshold EdgeSet promotes to a hash
// index so insert/remove stay amortized O(1).
//
// The threshold is NOT portable — it is where a scan over contiguous SlotIds
// crosses the cost of one hash probe, and it moves with both the scan's cache
// behaviour and the hash. lazily-rs measured 170 with SipHash and 40 with a
// multiply-shift finalizer; neither number transfers here.
//
// Measured for this binding by sweeping THIS constant and nothing else
// (benches/pubsub_load.cpp, g++ -O2 -march=native, 3 runs averaged, notify ns
// per subscriber). Each column is a fan-out width; each row a promote setting,
// so a cell is indexed when width >= promote and scanned otherwise:
//
//   promote \ width   24    32    40    48    64    80    96
//              24   25.0  25.6  25.7  25.4  25.0  26.2  26.2
//              32   23.3  25.7  25.9  25.6  24.7  26.2  26.3
//              48   23.2  25.2  30.0  25.6  25.0  25.9  26.0
//              64   22.5  25.1  29.5  33.2  24.3  25.8  26.6
//              96   23.5  25.0  30.1  33.9  39.5  45.9  25.1
//             128   22.9  25.3  30.1  33.8  39.9  45.9  50.2
//
// Read down a column: the indexed cells sit at ~25 ns regardless of width, the
// scanned ones climb with it. At width 24 the scan wins by ~2 ns, at 32 it is a
// tie, and from 40 up the index wins and the gap widens. The crossover is
// therefore degree ~32, and promote is set there.
//
// A first attempt measured this as ~96 by comparing an always-indexed build
// against an always-scanning one. That is the wrong experiment: forcing an
// index onto every one-element dependency list charges its overhead to the wide
// dependents list, inflating the apparent crossover ~3x. Only sweeping the
// threshold in isolation gives the real number.
#ifndef LAZILY_EDGE_INDEX_PROMOTE
#define LAZILY_EDGE_INDEX_PROMOTE 32
#endif
// Demote is deliberately far below promote. A dependent list oscillates by one
// on every recompute — edges are removed and re-registered — so a single shared
// boundary makes a list sitting at the threshold tear down and rebuild its whole
// index on every publish. Measured with promote == demote == 96: 145.5 ns/sub at
// width 96 and 141.0 at 97, against 25.8 at width 98 one rung away, a 5.5x spike
// visible at exactly two widths and nowhere else. The 4x gap here puts the whole
// oscillation band inside the indexed regime.
#ifndef LAZILY_EDGE_INDEX_DEMOTE
#define LAZILY_EDGE_INDEX_DEMOTE 8
#endif

// TRADEOFF, measured not assumed. EdgeSet is 32 bytes against EdgeVec's 24 (one
// owning pointer), and a SlotNode holds two of them, so every node grows 16
// bytes whether or not it is ever wide. On the one benchmark that iterates a
// wide dependent list without touching any edge — set_cell_invalidation /
// high_fan_out / 512, which invalidates 512 lazy slots that are never read —
// that costs 20% (2.14 -> 2.58 us, stable over 7 runs). Everything that
// actually registers edges gets faster, including the low-degree cases:
// dependency_fan_out / 32 is 14% faster and / 256 is 15% faster.
//
// Indexing BOTH directions is what buys that, and it is not optional. A variant
// keeping `dependencies` as a plain EdgeVec removes the 20% entirely, but a slot
// reading N distinct cells then keeps the full O(N^2) on its own dependency
// list — measured identical to the unfixed baseline (ns per dependency, one slot
// reading N cells, recompute):
//
//   N            1024    4096   16384   65536  262144
//   dependents-only index  115.1   400.8  1587.1  5932.0 25126.8
//   both indexed            18.6    21.4    22.7    24.8    55.6
//
// Fan-in degree is degree. Half-fixing it would leave the same defect reachable
// from the mirror shape, so the 16 bytes are paid on every node.

/// Ordered, deduplicated set of edges. Scans while small; hash-indexed when
/// wide. The index is owned by the set, so it moves with it, dies with it, and
/// can never be aliased onto a recycled id — unlike a side table keyed by owner.
class EdgeSet {
 public:
  static constexpr size_t kPromote = LAZILY_EDGE_INDEX_PROMOTE;
  static constexpr size_t kDemote = LAZILY_EDGE_INDEX_DEMOTE;
  static_assert(kDemote <= kPromote, "demote must not exceed promote");

  EdgeSet() = default;
  ~EdgeSet() = default;
  EdgeSet(EdgeSet&&) noexcept = default;
  EdgeSet& operator=(EdgeSet&&) noexcept = default;

  EdgeSet(const EdgeSet& other) : edges_(other.edges_) {
    if (other.index_) rebuild_index();
  }
  EdgeSet& operator=(const EdgeSet& other) {
    if (this != &other) {
      edges_ = other.edges_;
      index_.reset();
      if (other.index_) rebuild_index();
    }
    return *this;
  }

  // Read API mirrors the plain vector it replaced, so every iteration site is
  // unchanged.
  size_t size() const noexcept { return edges_.size(); }
  bool empty() const noexcept { return edges_.empty(); }
  const SlotId* begin() const noexcept { return edges_.begin(); }
  const SlotId* end() const noexcept { return edges_.end(); }
  const SlotId& operator[](size_t i) const noexcept { return edges_[i]; }

  void clear() noexcept {
    edges_.clear();
    index_.reset();
  }

  /// Insert if absent. Returns true if the edge was added.
  bool insert(SlotId id) {
    if (index_) {
      if (lookup(id) != kNpos) return false;
      edges_.push_back(id);
      index_->live++;
      place(id, edges_.size() - 1);
      if (index_->needs_rehash()) rebuild_index();
      return true;
    }
    const size_t n = edges_.size();
    const SlotId* data = edges_.begin();
    for (size_t i = 0; i < n; ++i)
      if (data[i] == id) return false;
    edges_.push_back(id);
    // Promote once the scan stops paying for itself. uint32_t positions cap the
    // index at 4G edges on one node; beyond that keep scanning (unreachable in
    // practice, and correct either way).
    if (edges_.size() >= kPromote && edges_.size() < kPosMax) rebuild_index();
    return true;
  }

  /// Remove if present. Returns true if an edge was removed.
  bool remove(SlotId id) {
    if (!index_) {
      for (size_t i = 0; i < edges_.size(); ++i) {
        if (edges_[i] == id) {
          edges_[i] = edges_[edges_.size() - 1];
          edges_.pop_back();
          return true;
        }
      }
      return false;
    }

    const size_t slot = probe(id);
    if (slot == kNpos) return false;
#ifdef LAZILY_NAIVE_EDGE_REMOVE
    // Audit-only: force the position lookup back to the linear scan this index
    // exists to remove, changing nothing else. Every other line below — the
    // tombstone, the swap, the moved-id fixup, demote and rehash — runs
    // identically, so a measured delta is the scan and only the scan. Never
    // define this in a shipping build; see the #lzspecedgeindex audit.
    size_t pos = kNpos;
    for (size_t i = 0; i < edges_.size(); ++i)
      if (edges_[i] == id) { pos = i; break; }
    if (pos == kNpos) return false;
#else
    const size_t pos = size_t(index_->slots[slot]) - 1;
#endif

    // Tombstone the departing id BEFORE the swap. Afterwards edges_[pos] holds
    // the moved id, so this slot would compare equal to it and the moved id's
    // position could be written to the wrong slot.
    index_->slots[slot] = kTomb;
    index_->live--;
    index_->tomb++;

    const size_t last = edges_.size() - 1;
    if (pos != last) {
      const SlotId moved = edges_[last];
      edges_[pos] = moved;
      const size_t moved_slot = probe(moved);
      // The tombstoned slot is skipped by probe(), so this finds the moved id's
      // own slot and nothing else.
      if (moved_slot != kNpos)
        index_->slots[moved_slot] = static_cast<uint32_t>(pos + 1);
    }
    edges_.pop_back();

    if (edges_.size() <= kDemote)
      index_.reset();  // hysteresis: well below kPromote, so a list oscillating
                       // by one at the promote boundary never rebuilds.
    else if (index_->needs_rehash())
      rebuild_index();
    return true;
  }

 private:
  struct Index {
    std::vector<uint32_t> slots;  // 0 = empty, kTomb = deleted, else pos + 1
    size_t live = 0;
    size_t tomb = 0;
    bool needs_rehash() const {
      return (live + tomb) * 2 >= slots.size() || tomb * 4 >= slots.size();
    }
  };

  static constexpr size_t kNpos = size_t(-1);
  static constexpr uint32_t kTomb = 0xFFFFFFFFu;
  static constexpr size_t kPosMax = 0xFFFFFFFEu;

  // Multiply-shift finalizer. SlotIds are dense and contiguous, so the low bits
  // alone would collide catastrophically under a power-of-two mask; this is
  // also markedly cheaper than a general-purpose string-capable hash.
  static uint64_t mix(uint64_t x) noexcept {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 29;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 32;
    return x;
  }

  /// Slot holding `id`, or kNpos. Stops at the first empty slot; skips
  /// tombstones, which is what keeps deletion from truncating probe chains.
  size_t probe(SlotId id) const {
    const size_t mask = index_->slots.size() - 1;
    size_t i = size_t(mix(id.value)) & mask;
    for (;;) {
      const uint32_t v = index_->slots[i];
      if (v == 0) return kNpos;
      if (v != kTomb && edges_[size_t(v) - 1] == id) return i;
      i = (i + 1) & mask;
    }
  }

  size_t lookup(SlotId id) const {
    const size_t slot = probe(id);
    return slot == kNpos ? kNpos : size_t(index_->slots[slot]) - 1;
  }

  /// Write `pos` for an id already known to be absent.
  void place(SlotId id, size_t pos) {
    const size_t mask = index_->slots.size() - 1;
    size_t i = size_t(mix(id.value)) & mask;
    while (index_->slots[i] != 0 && index_->slots[i] != kTomb)
      i = (i + 1) & mask;
    if (index_->slots[i] == kTomb) index_->tomb--;
    index_->slots[i] = static_cast<uint32_t>(pos + 1);
  }

  void rebuild_index() {
    size_t cap = 16;
    while (cap < edges_.size() * 4) cap <<= 1;  // load factor <= 0.5 after growth
    index_ = std::unique_ptr<Index>(new Index());
    index_->slots.assign(cap, 0);
    index_->live = edges_.size();
    index_->tomb = 0;
    for (size_t i = 0; i < edges_.size(); ++i) place(edges_[i], i);
  }

  EdgeVec edges_;
  // 8 bytes per edge list when unused; the index itself is one uint32 per hash
  // slot at load factor <= 0.5, i.e. ~8 bytes per edge — not a node-per-entry
  // hash map, which would dominate per-subscriber memory at wide fan-out.
  std::unique_ptr<Index> index_;
};

inline bool edge_insert(EdgeSet& edges, SlotId id) { return edges.insert(id); }

inline bool edge_remove(EdgeSet& edges, SlotId id) { return edges.remove(id); }

/// Scheduler queue for pending effects: FIFO at the head, swap-remove in the
/// middle. Scans while shallow, position-indexed when deep — the same shape as
/// EdgeSet, for the same reason (#lzspecedgeindex).
///
/// Removal used to be a bare linear scan. That is O(depth) per dispose, and the
/// shape that hits it is disposing effects from inside a flush — an effect
/// tearing down its siblings while the rest of a wide cascade is still queued.
/// There the depth IS the fan-out width, so a width-W teardown cost W^2/2
/// comparisons: measured 134,225,920 iterations at W=16384 against W^2/2 =
/// 134,217,728.
///
/// The effect_scheduled_ bitset alone does not fix it. It answers "is this
/// queued" in O(1), which skips the scan for an effect that is not queued — but
/// in the biting shape every disposed effect IS queued, so the guard says yes
/// and the scan still runs. Measured: guard alone left iterations unchanged at
/// 134,225,920. The position index is what removes the quadratic; the guard is
/// kept as well because it is free and does fix the not-queued case.
///
/// Positions are absolute (base_ + offset) because pop_front shifts every
/// index; base_ counts pops so stored positions survive them.
class PendingQueue {
 public:
  bool empty() const noexcept { return q_.empty(); }
  size_t size() const noexcept { return q_.size(); }

  void push_back(SlotId id) {
    q_.push_back(id);
    // No promote check here: the index is built lazily by the first erase that
    // needs it, so a queue that is only ever pushed and popped — which is what
    // a plain publish does — never allocates or hashes at all.
    if (index_) (*index_)[id.value] = base_ + q_.size() - 1;
  }

  SlotId pop_front() {
    const SlotId id = q_.front();
    q_.pop_front();
    ++base_;
    if (index_) {
      index_->erase(id.value);
      if (q_.size() <= kDemote) index_.reset();
    }
    return id;
  }

  /// Remove `id` if present. Swap-with-back, matching the previous linear
  /// implementation exactly — removal has never preserved FIFO order here.
  void erase(SlotId id) {
    if (!index_) {
      // Shallow: scanning beats building an index for a queue this size.
      if (q_.size() < kPromote) {
        for (size_t i = 0; i < q_.size(); ++i) {
          if (q_[i] == id) {
            q_[i] = q_.back();
            q_.pop_back();
            return;
          }
        }
        return;
      }
      build_index();  // pay O(depth) once, then every erase in this teardown
                      // is O(1) — that is what turns W^2/2 into W.
    }
    auto it = index_->find(id.value);
    if (it == index_->end()) return;
    const uint64_t abs = it->second;
    const uint64_t last_abs = base_ + q_.size() - 1;
    if (abs != last_abs) {
      const SlotId moved = q_.back();
      q_[size_t(abs - base_)] = moved;
      (*index_)[moved.value] = abs;
    }
    index_->erase(it);
    q_.pop_back();
    if (q_.size() <= kDemote) index_.reset();
  }

 private:
  // Deep enough that the index earns its allocation; a queue this shallow is
  // scanned faster than it is hashed. Hysteresis, as in EdgeSet, so a queue
  // oscillating at the boundary does not rebuild on every publish.
  //
  // Eager promotion on push_back was measured and rejected: it regressed wide
  // notify 28-38 -> 45-57 ns/sub (~1.5x), because a wide cascade pushes every
  // effect through a hash insert and pops it through a hash erase to fix a
  // teardown-only defect that notify never triggers.
  static constexpr size_t kPromote = 64;
  static constexpr size_t kDemote = 16;

  void build_index() {
    index_ = std::unique_ptr<std::unordered_map<uint64_t, uint64_t>>(
        new std::unordered_map<uint64_t, uint64_t>());
    index_->reserve(q_.size() * 2);
    for (size_t i = 0; i < q_.size(); ++i) (*index_)[q_[i].value] = base_ + i;
  }

  std::deque<SlotId> q_;
  uint64_t base_ = 0;  // absolute index of q_.front()
  // Allocated only while the queue is deep, so a shallow queue — the common
  // case — pays one pointer and no hashing.
  std::unique_ptr<std::unordered_map<uint64_t, uint64_t>> index_;
};

// ── Handle types (Traits-independent — just SlotId wrappers) ──

template <typename T>
struct SlotHandle {
  SlotId id;
  SlotHandle() = default;
  explicit SlotHandle(SlotId id_) : id(id_) {}
  T get(Context& ctx) const;
  void clear(Context& ctx) const;
  bool operator==(const SlotHandle<T>& o) const { return id == o.id; }
};

template <typename T>
struct CellHandle {
  SlotId id;
  CellHandle() = default;
  explicit CellHandle(SlotId id_) : id(id_) {}
  T get(Context& ctx) const;
  void set(Context& ctx, T value) const;
  void clear_dependents(Context& ctx) const;
  bool operator==(const CellHandle<T>& o) const { return id == o.id; }
};

struct EffectHandle {
  SlotId id;
  EffectHandle() = default;
  explicit EffectHandle(SlotId id_) : id(id_) {}
  void dispose(Context& ctx) const;
  bool is_active(Context& ctx) const;
};

template <typename T>
struct SignalHandle {
  SlotHandle<T> slot;
  EffectHandle effect;
  SignalHandle() = default;
  SignalHandle(SlotHandle<T> s, EffectHandle e) : slot(s), effect(e) {}
  T get(Context& ctx) const;
  std::shared_ptr<T> get_rc(Context& ctx) const;
  void dispose(Context& ctx) const;
  bool is_active(Context& ctx) const;
};

// ═══════════════════════════════════════════════════════════════════════════
// ContextImpl — the reactive graph engine.
//
// Templated on Traits to select Rc (non-atomic, default) or Arc (atomic)
// reference counting for type-erased values. Context = ContextImpl<RcTraits>.
// ThreadSafeContext wraps Context with a recursive_mutex; the mutex makes
// non-atomic counting safe.
// ═══════════════════════════════════════════════════════════════════════════

template <typename Traits>
class ContextImpl {
 public:
  using AnyValue = typename Traits::AnyValue;
  using ComputeFn = SmallFn<AnyValue(ContextImpl&)>;
  // Non-atomic ref counting for the compute/effect closures (≈ Rust Rc).
  // Safe: Context is single-threaded; ThreadSafeContext guards it with a mutex.
  // Each slot/effect creation previously paid an atomic control-block via
  // std::shared_ptr; RcPtr<RcBox<...>> drops that to a plain integer inc/dec.
  using ComputeFnPtr = RcPtr<RcBox<ComputeFn>>;
  using EqualsFn = bool (*)(const void*, const void*);
  using EffectFn = SmallFn<CleanupFn(ContextImpl&)>;
  using EffectFnPtr = RcPtr<RcBox<EffectFn>>;

 private:
  struct SlotNode {
    AnyValue value;
#ifndef NDEBUG
    std::type_index type_id;
#endif
    ComputeFnPtr compute;
    EqualsFn equals = nullptr;
    EdgeSet dependencies;
    EdgeSet dependents;
    bool dirty;
    bool force_recompute;
    bool in_progress;

    SlotNode()
        :
#ifndef NDEBUG
        type_id(std::type_index(typeid(void))),
#endif
        dirty(false),
        force_recompute(false),
        in_progress(false) {}
  };

  struct CellNode {
    AnyValue value;
#ifndef NDEBUG
    std::type_index type_id;
#endif
    EdgeSet dependents;

    CellNode()
#ifndef NDEBUG
        : type_id(std::type_index(typeid(void)))
#endif
    {}
  };

  struct EffectNode {
    EffectFnPtr run;
    EdgeSet dependencies;
    CleanupFn cleanup;
    bool force_run;

    EffectNode() : force_run(false) {}
  };

  // Pragmatic SoA-flavoured node-storage compaction (#lzcppsoa):
  //
  // The full per-kind SoA split (separate slot/cell/effect vectors with the
  // kind encoded in the SlotId) touches every call site in the engine and is
  // a larger surgery than is safe to land in one cycle. The high-value,
  // low-risk subset is to drop the `std::optional<Node>` wrapper in favour of
  // an in-variant Empty alternative. That removes ~8 bytes per node
  // (`sizeof(optional<variant>) == 104` vs `sizeof(variant) == 96` on the
  // libstdc++ layout used here) — ~8% RSS reduction at scale, and the code
  // stops paying the optional's engaged-flag check / `.value()`
  // indirection on every node access. The Empty state is the variant's
  // default-constructed index, so `nodes_.resize(N)` produces Empty entries
  // for free (same shape as the previous optional default of nullopt).
  struct EmptyNode {};

  using Node = std::variant<EmptyNode, SlotNode, CellNode, EffectNode>;
  using ScheduledEffect = std::pair<SlotId, bool>;
  using EffectList = SmallVec<ScheduledEffect, 4>;

 public:
  ContextImpl() = default;
  ContextImpl(const ContextImpl&) = delete;
  ContextImpl& operator=(const ContextImpl&) = delete;

  // -- Slot API --
  template <typename T, typename F>
  SlotHandle<T> slot(F&& compute) {
    return slot_with_equals<T>(std::forward<F>(compute), nullptr);
  }

  template <typename T, typename F>
  SlotHandle<T> computed(F&& compute) {
    return slot<T>(std::forward<F>(compute));
  }

  template <typename T, typename F>
  SlotHandle<T> memo(F&& compute) {
    EqualsFn eq = [](const void* a, const void* b) {
      return *static_cast<const T*>(a) == *static_cast<const T*>(b);
    };
    return slot_with_equals<T>(std::forward<F>(compute), eq);
  }

  template <typename T>
  T get(const SlotHandle<T>& handle) {
    return get_slot<T>(handle.id);
  }

  template <typename T>
  std::shared_ptr<T> get_rc(const SlotHandle<T>& handle) {
    if (auto parent = current_tracking_frame())
      register_dependency(handle.id, *parent);
    refresh_slot(handle.id);
    auto* slot = get_slot_node(handle.id);
    assert(slot && slot->value && "get_rc on unset/non-slot");
    assert(slot->type_id == std::type_index(typeid(T)));
    return std::make_shared<T>(*Traits::template cast<T>(slot->value));
  }

  // -- Cell API --
  template <typename T>
  CellHandle<T> cell(T value) {
    auto id = alloc_id();
    CellNode node;
    node.value = Traits::template make<T>(std::move(value));
#ifndef NDEBUG
    node.type_id = std::type_index(typeid(T));
#endif
    insert_node(id, Node(std::move(node)));
    return CellHandle<T>(id);
  }

  template <typename T>
  T get_cell(const CellHandle<T>& handle) {
    if (auto parent = current_tracking_frame())
      register_dependency(handle.id, *parent);
    auto* cell = get_cell_node(handle.id);
    assert(cell && "get_cell on non-cell");
    assert(cell->type_id == std::type_index(typeid(T)));
    return *Traits::template cast<T>(cell->value);
  }

  template <typename T>
  std::shared_ptr<T> get_cell_rc(const CellHandle<T>& handle) {
    if (auto parent = current_tracking_frame())
      register_dependency(handle.id, *parent);
    auto* cell = get_cell_node(handle.id);
    assert(cell && "get_cell_rc on non-cell");
    assert(cell->type_id == std::type_index(typeid(T)));
    return std::make_shared<T>(*Traits::template cast<T>(cell->value));
  }

  // ── Read-only cache peeks (no mutation) ──
  //
  // Safe to call under a shared lock: they neither mutate node state nor copy
  // the ref-counted AnyValue handle (they copy the boxed T by value through a
  // stable, immutably-published pointer). Used by ThreadSafeContext's shared
  // read fast path.
  template <typename T>
  std::optional<T> try_get_cached(const SlotHandle<T>& handle) const {
    auto idx = node_index(handle.id);
    if (!idx || *idx >= nodes_.size()) return std::nullopt;
    auto& n = nodes_[*idx];
    if (std::holds_alternative<EmptyNode>(n)) return std::nullopt;
    const SlotNode* slot = std::get_if<SlotNode>(&n);
    if (!slot || !slot->value || slot->dirty || slot->force_recompute)
      return std::nullopt;
    return *Traits::template cast<T>(slot->value);
  }

  template <typename T>
  std::optional<T> peek_cell(const CellHandle<T>& handle) const {
    auto idx = node_index(handle.id);
    if (!idx || *idx >= nodes_.size()) return std::nullopt;
    auto& n = nodes_[*idx];
    if (std::holds_alternative<EmptyNode>(n)) return std::nullopt;
    const CellNode* cell = std::get_if<CellNode>(&n);
    if (!cell || !cell->value) return std::nullopt;
    return *Traits::template cast<T>(cell->value);
  }

  template <typename T>
  void set_cell(const CellHandle<T>& handle, T new_value) {
    bool changed;
    {
      auto* cell = get_cell_node(handle.id);
      assert(cell && "set_cell on non-cell");
      assert(cell->type_id == std::type_index(typeid(T)));
      changed = (*Traits::template cast<T>(cell->value) != new_value);
    }
    if (!changed) return;
    {
      auto* cell = get_cell_node_mut(handle.id);
      cell->value = Traits::template make<T>(std::move(new_value));
    }
    if (is_batching()) {
      batched_cells_.insert(handle.id);
    } else {
      // Store-without-cascade: dirty-mark the dependent cone, then flush
      // effects ONLY when the cone actually contains an active Effect. A cell
      // with no Effect-bearing dependent stores its latest value (already done
      // above, so a late subscriber reads it glitch-free) and dirty-marks lazy
      // Slot dependents, but pays no effect-scheduling flush.
      if (invalidate_cell_dependents_now(handle.id))
        flush_effects();
    }
  }

  // -- Batch API --
  template <typename F>
  auto batch(F&& run) {
    batch_depth_++;
    struct Guard {
      ContextImpl* ctx;
      ~Guard() { ctx->finish_batch(); }
    } guard{this};
    return run(*this);
  }

  bool is_batching() { return batch_depth_ > 0; }

  // -- Effect API --
  template <typename F>
  EffectHandle effect(F&& run) {
    auto id = alloc_id();
    EffectNode node;
    auto run_fn = EffectFn([f = std::forward<F>(run)](ContextImpl& ctx) -> CleanupFn {
      return f(ctx);
    });
    node.run = EffectFnPtr(new RcBox<EffectFn>(std::move(run_fn)),
                           typename EffectFnPtr::adopt_t{});
    node.force_run = true;
    insert_node(id, Node(std::move(node)));
    schedule_effect(id, false);
    flush_effects();
    return EffectHandle(id);
  }

  template <typename F>
  EffectHandle effect_void(F&& run) {
    auto id = alloc_id();
    EffectNode node;
    auto run_fn = EffectFn([f = std::forward<F>(run)](ContextImpl& ctx) -> CleanupFn {
      f(ctx);
      return CleanupFn{};
    });
    node.run = EffectFnPtr(new RcBox<EffectFn>(std::move(run_fn)),
                           typename EffectFnPtr::adopt_t{});
    node.force_run = true;
    insert_node(id, Node(std::move(node)));
    schedule_effect(id, false);
    flush_effects();
    return EffectHandle(id);
  }

  void dispose_effect(const EffectHandle& handle) {
    EdgeSet old_deps;
    CleanupFn cleanup;
    {
      remove_pending_effect(handle.id);
      auto idx = node_index(handle.id);
      if (!idx || *idx >= nodes_.size() ||
          std::holds_alternative<EmptyNode>(nodes_[*idx]))
        return;
      auto& node = nodes_[*idx];
      auto* eff = std::get_if<EffectNode>(&node);
      if (!eff) return;
      old_deps = std::move(eff->dependencies);
      cleanup = std::move(eff->cleanup);
      nodes_[*idx] = EmptyNode{};
      free_ids_.push_back(handle.id.value);
    }
    for (auto dep_id : old_deps)
      remove_dependent_edge(dep_id, handle.id);
    if (cleanup) cleanup();
  }

  bool is_effect_active(const EffectHandle& handle) {
    auto* node = get_node(handle.id);
    return node && std::holds_alternative<EffectNode>(*node);
  }

  // -- Signal API --
  template <typename T, typename F>
  SignalHandle<T> signal(F&& compute) {
    auto slot_handle = this->memo<T>(std::forward<F>(compute));
    auto eff = effect_void([slot_handle](ContextImpl& ctx) {
      ctx.get_rc<T>(slot_handle);
    });
    return SignalHandle<T>(slot_handle, eff);
  }

  template <typename T>
  T get_signal(const SignalHandle<T>& handle) {
    return get<T>(handle.slot);
  }

  template <typename T>
  std::shared_ptr<T> get_signal_rc(const SignalHandle<T>& handle) {
    return get_rc<T>(handle.slot);
  }

  template <typename T>
  void dispose_signal(const SignalHandle<T>& handle) {
    dispose_effect(handle.effect);
  }

  template <typename T>
  bool is_signal_active(const SignalHandle<T>& handle) {
    return is_effect_active(handle.effect);
  }

  // -- Clearing --
  template <typename T>
  bool is_set(const SlotHandle<T>& handle) {
    auto* slot = get_slot_node(handle.id);
    return slot && slot->value && !slot->dirty;
  }

  void clear_slot(SlotId id) {
    if (is_batching()) {
      batched_slots_.insert(id);
      return;
    }
    clear_slot_now(id);
  }

  void flush_effects_after_invalidation() {
    if (!is_batching())
      flush_effects();
  }

  void clear_cell_dependents(SlotId id) {
    if (is_batching()) {
      batched_cell_clears_.insert(id);
      return;
    }
    clear_cell_dependents_now(id);
    flush_effects();
  }

  SlotId alloc_id() {
    if (!free_ids_.empty()) {
      auto id = SlotId(free_ids_.back());
      free_ids_.pop_back();
      return id;
    }
    return SlotId(next_id_++);
  }

  /// Pre-allocate node capacity. Call before bulk-inserting cells/slots
  /// to avoid repeated vector reallocations.
  void reserve(size_t capacity) {
    nodes_.reserve(capacity);
  }

 private:
  // Per #lzcppsoa: a flat vector of `Node` (variant with an Empty alternative
  // standing in for the old `std::optional<Node>` nullopt). Empty entries are
  // produced by `resize` (the variant's default-constructed index is Empty)
  // and re-entered via `alloc_id` from `free_ids_`.
  std::vector<Node> nodes_;
  uint64_t next_id_ = 0;
  std::vector<uint64_t> free_ids_;
  PendingQueue pending_effects_;
  // Mark bitset over the node id space (mirrors BatchSet below): a set bit
  // means the effect node is already queued in pending_effects_. Cheaper than
  // unordered_set (no per-insert bucket alloc, cache-friendly). The bit is
  // cleared on pop/dispose; ids recycle via free_ids_ but always enter clean.
  std::vector<bool> effect_scheduled_;
  bool flushing_effects_ = false;
  int batch_depth_ = 0;
  // Alloc-free batch bookkeeping (optimization E): a mark bitset over the node
  // id space + an insertion-ordered list. Capacity persists across batches, so
  // there is no per-batch or per-insert allocation (vs unordered_set's bucket
  // allocs). Marks are cleared only for touched ids during flush.
  struct BatchSet {
    std::vector<bool> mark;
    std::vector<SlotId> order;
    void insert(SlotId id) {
      if (id.value >= mark.size()) mark.resize(id.value + 1, false);
      if (!mark[id.value]) {
        mark[id.value] = true;
        order.push_back(id);
      }
    }
  };
  BatchSet batched_cells_;
  BatchSet batched_cell_clears_;
  BatchSet batched_slots_;
  std::vector<SlotId> tracking_stack_;

  static std::optional<size_t> node_index(SlotId id) {
    if (id.value > static_cast<uint64_t>(SIZE_MAX)) return std::nullopt;
    return static_cast<size_t>(id.value);
  }

  Node* get_node(SlotId id) {
    auto idx = node_index(id);
    if (!idx || *idx >= nodes_.size()) return nullptr;
    auto& n = nodes_[*idx];
    if (std::holds_alternative<EmptyNode>(n)) return nullptr;
    return &n;
  }

  SlotNode* get_slot_node(SlotId id) {
    auto* node = get_node(id);
    if (!node) return nullptr;
    return std::get_if<SlotNode>(node);
  }

  SlotNode* get_slot_node_mut(SlotId id) { return get_slot_node(id); }

  CellNode* get_cell_node(SlotId id) {
    auto* node = get_node(id);
    if (!node) return nullptr;
    return std::get_if<CellNode>(node);
  }

  CellNode* get_cell_node_mut(SlotId id) { return get_cell_node(id); }

  void insert_node(SlotId id, Node node) {
    auto idx = node_index(id);
    assert(idx && "SlotId overflow");
    if (nodes_.size() <= *idx)
      nodes_.resize(*idx + 1);
    nodes_[*idx] = std::move(node);
  }

  std::optional<SlotId> current_tracking_frame() {
    if (tracking_stack_.empty()) return std::nullopt;
    return tracking_stack_.back();
  }

  void push_tracking_frame(SlotId id) { tracking_stack_.push_back(id); }
  void pop_tracking_frame() { tracking_stack_.pop_back(); }

  void register_dependency(SlotId dependency_id, SlotId dependent_id) {
    if (dependency_id == dependent_id) return;
    auto* dep_node = get_node(dependency_id);
    if (dep_node) {
      if (auto* s = std::get_if<SlotNode>(dep_node))
        edge_insert(s->dependents, dependent_id);
      else if (auto* c = std::get_if<CellNode>(dep_node))
        edge_insert(c->dependents, dependent_id);
    }
    auto* dpt_node = get_node(dependent_id);
    if (dpt_node) {
      if (auto* s = std::get_if<SlotNode>(dpt_node))
        edge_insert(s->dependencies, dependency_id);
      else if (auto* e = std::get_if<EffectNode>(dpt_node))
        edge_insert(e->dependencies, dependency_id);
    }
  }

  void remove_dependent_edge(SlotId dependency_id, SlotId dependent_id) {
    auto* dep_node = get_node(dependency_id);
    if (dep_node) {
      if (auto* s = std::get_if<SlotNode>(dep_node))
        edge_remove(s->dependents, dependent_id);
      else if (auto* c = std::get_if<CellNode>(dep_node))
        edge_remove(c->dependents, dependent_id);
    }
  }

  template <typename T, typename F>
  SlotHandle<T> slot_with_equals(F&& compute, EqualsFn eq) {
    auto id = alloc_id();
    SlotNode node;
#ifndef NDEBUG
    node.type_id = std::type_index(typeid(T));
#endif
    auto compute_fn =
        ComputeFn([f = std::forward<F>(compute)](ContextImpl& ctx) -> AnyValue {
          return Traits::template make<T>(f(ctx));
        });
    node.compute = ComputeFnPtr(new RcBox<ComputeFn>(std::move(compute_fn)),
                                 typename ComputeFnPtr::adopt_t{});
    node.equals = eq;
    insert_node(id, Node(std::move(node)));
    return SlotHandle<T>(id);
  }

  template <typename T>
  T get_slot(SlotId id) {
    if (auto parent = current_tracking_frame())
      register_dependency(id, *parent);
    refresh_slot(id);
    auto* slot = get_slot_node(id);
    assert(slot && slot->value && "get_slot on unset/non-slot");
    assert(slot->type_id == std::type_index(typeid(T)));
    return *Traits::template cast<T>(slot->value);
  }

  bool refresh_slot(SlotId id) {
    // Fast path: clean cache hit. When the slot holds a value and is
    // neither dirty nor force-recompute, no upstream value can have
    // changed since the last compute — invalidation always sets dirty on
    // dependents. The dependency-refresh walk, the cycle guard, and the
    // dirty-flag clear are therefore all unnecessary on this path. This is
    // the hot path for cached slot reads.
    {
      auto* slot = get_slot_node(id);
      if (slot && slot->value && !slot->dirty && !slot->force_recompute)
        return false;
    }

    if (!enter_refresh(id)) return false;
    struct RefreshGuard {
      ContextImpl* ctx;
      SlotId id;
      ~RefreshGuard() {
        if (auto* slot = ctx->get_slot_node(id))
          slot->in_progress = false;
      }
    } guard{this, id};

    // Read-only snapshot: copy the ids, not the EdgeSet, so a wide dependency
    // list does not rebuild a hash index nobody queries (#lzspecedgeindex).
    EdgeVec dependencies;
    {
      auto* slot = get_slot_node(id);
      if (!slot) return false;
      for (SlotId dep : slot->dependencies) dependencies.push_back(dep);
    }

    bool dependency_changed = false;
    for (auto dep_id : dependencies) {
      if (is_slot_node(dep_id) && refresh_slot(dep_id))
        dependency_changed = true;
    }

    bool needs_recompute;
    {
      auto* slot = get_slot_node(id);
      if (!slot) return false;
      needs_recompute = !slot->value || slot->force_recompute || dependency_changed;
    }

    if (!needs_recompute) {
      clear_slot_dirty_flags(id);
      return false;
    }

    return recompute_slot_now(id);
  }

  bool enter_refresh(SlotId id) {
    auto* slot = get_slot_node(id);
    if (!slot) return false;
    assert(!slot->in_progress && "lazily: circular dependency detected");
    slot->in_progress = true;
    return true;
  }

  bool is_slot_node(SlotId id) {
    auto* node = get_node(id);
    return node && std::holds_alternative<SlotNode>(*node);
  }

  void clear_slot_dirty_flags(SlotId id) {
    if (auto* slot = get_slot_node(id)) {
      slot->dirty = false;
      slot->force_recompute = false;
    }
  }

  bool recompute_slot_now(SlotId id) {
    ComputeFnPtr compute;
    EdgeSet old_deps;
    {
      auto* slot = get_slot_node(id);
      if (!slot) return false;
      old_deps = std::move(slot->dependencies);
      compute = slot->compute;
    }
    for (auto dep_id : old_deps)
      remove_dependent_edge(dep_id, id);

    push_tracking_frame(id);
    AnyValue result = (*compute).value(*this);
    pop_tracking_frame();

    bool changed;
    {
      auto* slot = get_slot_node(id);
      if (!slot) return false;
      bool had_value = static_cast<bool>(slot->value);
      bool unchanged = false;
      if (slot->value && slot->equals != nullptr) {
        unchanged = slot->equals(slot->value.raw(), result.raw());
      }
      slot->dirty = false;
      slot->force_recompute = false;
      if (unchanged) {
        changed = false;
      } else {
        slot->value = std::move(result);
        changed = had_value;
      }
    }

    if (changed)
      notify_slot_value_changed(id);
    return changed;
  }

  void notify_slot_value_changed(SlotId id) {
    invalidate_dependents_now(id);
  }

  // Iterative DFS dirty-marking. Roots get force=true; transitive descendants
  // get force=false (matching the former recursive semantics). Returns
  // (effect_id, force) pairs for the caller to schedule after the walk.
  // Stack-based DFS iterates each node's dependents directly without copying
  // the EdgeSet, and mutates dirty/force flags in place (#lzbatchborrow).
  EffectList mark_frontier_locked(const EdgeSet& roots) {
    EffectList effects;
    SmallVec<SlotId, 16> stack;
    SmallVec<bool, 16> force_stack;
    for (SlotId root : roots) {
      stack.push_back(root);
      force_stack.push_back(true);
    }
    while (!stack.empty()) {
      SlotId id = stack.back();
      bool force = force_stack.back();
      stack.pop_back();
      force_stack.pop_back();
      auto* node = get_node(id);
      if (!node) continue;
      auto* slot = std::get_if<SlotNode>(node);
      if (slot) {
        bool should_propagate = !slot->dirty || (force && !slot->force_recompute);
        slot->dirty = true;
        if (force) slot->force_recompute = true;
        if (should_propagate) {
          for (SlotId dep_id : slot->dependents) {
            stack.push_back(dep_id);
            force_stack.push_back(false);
          }
        }
      } else if (std::holds_alternative<EffectNode>(*node)) {
        effects.push_back({id, force});
      }
    }
    return effects;
  }

  // Iterative DFS value-clearing. Clears slot values and dirty flags,
  // collecting effects to schedule. Iterates dependents directly without
  // copying the EdgeSet (#lzbatchborrow).
  EffectList clear_frontier_locked(const EdgeSet& roots) {
    EffectList effects;
    SmallVec<SlotId, 16> stack;
    for (SlotId root : roots) {
      stack.push_back(root);
    }
    while (!stack.empty()) {
      SlotId id = stack.back();
      stack.pop_back();
      auto* node = get_node(id);
      if (!node) continue;
      auto* slot = std::get_if<SlotNode>(node);
      if (slot) {
        if (!slot->value && !slot->dirty) continue;
        slot->value.reset();
        slot->dirty = false;
        slot->force_recompute = false;
        for (SlotId dep_id : slot->dependents) {
          stack.push_back(dep_id);
        }
      } else if (std::holds_alternative<EffectNode>(*node)) {
        effects.push_back({id, true});
      }
    }
    return effects;
  }

  // Iterative batched invalidation: marks all reachable slots dirty via a
  // single stack-based DFS, then schedules collected effects. Returns true
  // iff at least one Effect was scheduled (the store-without-cascade fast
  // path: a cell with no Effect-bearing dependent cone stores its latest
  // value and dirty-marks lazy Slot dependents, but pays no effect flush).
  bool invalidate_dependents_now(SlotId id) {
    auto* node = get_node(id);
    if (!node) return false;
    const EdgeSet* roots;
    if (auto* c = std::get_if<CellNode>(node))
      roots = &c->dependents;
    else if (auto* s = std::get_if<SlotNode>(node))
      roots = &s->dependents;
    else
      return false;
    if (roots->empty()) return false;
    EffectList effects = mark_frontier_locked(*roots);
    bool scheduled = !effects.empty();
    for (auto& entry : effects) schedule_effect(entry.first, entry.second);
    return scheduled;
  }

  bool invalidate_cell_dependents_now(SlotId id) {
    return invalidate_dependents_now(id);
  }

  void clear_cell_dependents_now(SlotId id) {
    auto* cell = get_cell_node(id);
    if (!cell) return;
    EffectList effects = clear_frontier_locked(cell->dependents);
    for (auto& entry : effects) schedule_effect(entry.first, entry.second);
  }

  void clear_slot_now(SlotId id) {
    EdgeSet roots;
    roots.insert(id);
    EffectList effects = clear_frontier_locked(roots);
    for (auto& entry : effects) schedule_effect(entry.first, entry.second);
  }

  void schedule_effect(SlotId id, bool force) {
    auto* node = get_node(id);
    if (!node) return;
    auto* eff = std::get_if<EffectNode>(node);
    if (!eff) return;
    if (force) eff->force_run = true;
    if (id.value >= effect_scheduled_.size())
      effect_scheduled_.resize(id.value + 1, false);
    if (!effect_scheduled_[id.value]) {
      effect_scheduled_[id.value] = true;
      pending_effects_.push_back(id);
    }
  }

  void unschedule_effect(SlotId id) {
    if (id.value < effect_scheduled_.size()) effect_scheduled_[id.value] = false;
  }

  void remove_pending_effect(SlotId id) {
    // The second linear scan on the wide-fan-out notify path, and only visible
    // once the edge index removed the first (#lzspecedgeindex). flush_effects
    // pops the front and unschedules it, then run_effect calls this — which
    // scanned the whole remaining queue to find an id that is provably not in
    // it. With W pending effects that is O(W) per effect, O(W^2) per publish.
    // effect_scheduled_ already answers "is it queued" in O(1); ask it first.
    if (id.value >= effect_scheduled_.size() || !effect_scheduled_[id.value])
      return;
    pending_effects_.erase(id);
    unschedule_effect(id);
  }

  void flush_effects() {
    if (flushing_effects_) return;
    flushing_effects_ = true;
    while (true) {
      SlotId id;
      {
        if (pending_effects_.empty()) {
          flushing_effects_ = false;
          return;
        }
        id = pending_effects_.pop_front();
        unschedule_effect(id);
      }
      run_effect(id);
    }
  }

  void run_effect(SlotId id) {
    if (!effect_should_run(id)) return;
    remove_pending_effect(id);

    EffectFnPtr run;
    EdgeSet old_deps;
    CleanupFn cleanup;
    {
      auto* node = get_node(id);
      if (!node) return;
      auto* effect = std::get_if<EffectNode>(node);
      if (!effect) return;
      old_deps = std::move(effect->dependencies);
      cleanup = std::move(effect->cleanup);
      effect->force_run = false;
      run = effect->run;
    }

    for (auto dep_id : old_deps)
      remove_dependent_edge(dep_id, id);
    if (cleanup) cleanup();

    push_tracking_frame(id);
    auto next_cleanup = (*run).value(*this);
    pop_tracking_frame();

    auto* node = get_node(id);
    if (node) {
      auto* effect = std::get_if<EffectNode>(node);
      if (effect) {
        effect->cleanup = std::move(next_cleanup);
      } else if (next_cleanup) {
        next_cleanup();
      }
    } else if (next_cleanup) {
      next_cleanup();
    }
  }

  bool effect_should_run(SlotId id) {
    auto* node = get_node(id);
    if (!node) return false;
    auto* effect = std::get_if<EffectNode>(node);
    if (!effect) return false;
    if (effect->force_run) return true;
    for (auto dep_id : effect->dependencies) {
      if (is_slot_node(dep_id) && refresh_slot(dep_id))
        return true;
    }
    return false;
  }

  void finish_batch() {
    assert(batch_depth_ > 0);
    batch_depth_--;
    if (batch_depth_ == 0)
      flush_batched_invalidations();
  }

  void flush_batched_invalidations() {
    for (SlotId id : batched_cells_.order) {
      batched_cells_.mark[id.value] = false;
      invalidate_cell_dependents_now(id);
    }
    batched_cells_.order.clear();
    for (SlotId id : batched_cell_clears_.order) {
      batched_cell_clears_.mark[id.value] = false;
      clear_cell_dependents_now(id);
    }
    batched_cell_clears_.order.clear();
    for (SlotId id : batched_slots_.order) {
      batched_slots_.mark[id.value] = false;
      clear_slot_now(id);
    }
    batched_slots_.order.clear();
    flush_effects();
  }
};

// ═══════════════════════════════════════════════════════════════════════════
// Handle method implementations — use Context (= ContextImpl<RcTraits>)
// ═══════════════════════════════════════════════════════════════════════════

template <typename T>
T SlotHandle<T>::get(Context& ctx) const {
  return ctx.get<T>(*this);
}

template <typename T>
void SlotHandle<T>::clear(Context& ctx) const {
  ctx.clear_slot(id);
  ctx.flush_effects_after_invalidation();
}

template <typename T>
T CellHandle<T>::get(Context& ctx) const {
  return ctx.get_cell<T>(*this);
}

template <typename T>
void CellHandle<T>::set(Context& ctx, T value) const {
  ctx.set_cell<T>(*this, std::move(value));
}

template <typename T>
void CellHandle<T>::clear_dependents(Context& ctx) const {
  ctx.clear_cell_dependents(id);
}

inline void EffectHandle::dispose(Context& ctx) const {
  ctx.dispose_effect(*this);
}

inline bool EffectHandle::is_active(Context& ctx) const {
  return ctx.is_effect_active(*this);
}

template <typename T>
T SignalHandle<T>::get(Context& ctx) const {
  return ctx.get_signal<T>(*this);
}

template <typename T>
std::shared_ptr<T> SignalHandle<T>::get_rc(Context& ctx) const {
  return ctx.get_signal_rc<T>(*this);
}

template <typename T>
void SignalHandle<T>::dispose(Context& ctx) const {
  ctx.dispose_signal<T>(*this);
}

template <typename T>
bool SignalHandle<T>::is_active(Context& ctx) const {
  return ctx.is_signal_active<T>(*this);
}

}  // namespace lazily

#endif  // LAZILY_CONTEXT_HPP
