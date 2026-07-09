#ifndef LAZILY_CONTEXT_HPP
#define LAZILY_CONTEXT_HPP

#include <lazily/types.hpp>
#include <lazily/small_fn.hpp>
#include <lazily/small_vec.hpp>
#include <lazily/rc_ptr.hpp>

#include <cassert>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <typeindex>
#include <unordered_set>
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
using CleanupFn = std::function<void()>;

inline bool edge_insert(EdgeVec& edges, SlotId id) {
  for (auto& e : edges)
    if (e == id) return false;
  edges.push_back(id);
  return true;
}

inline bool edge_remove(EdgeVec& edges, SlotId id) {
  for (size_t i = 0; i < edges.size(); ++i) {
    if (edges[i] == id) {
      edges[i] = edges.back();
      edges.pop_back();
      return true;
    }
  }
  return false;
}

inline void deque_erase(std::deque<SlotId>& dq, SlotId id) {
  for (size_t i = 0; i < dq.size(); ++i) {
    if (dq[i] == id) {
      dq[i] = dq.back();
      dq.pop_back();
      return;
    }
  }
}

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
    std::type_index type_id;
    ComputeFnPtr compute;
    std::optional<EqualsFn> equals;
    EdgeVec dependencies;
    EdgeVec dependents;
    bool dirty;
    bool force_recompute;
    bool in_progress;

    SlotNode()
        : type_id(std::type_index(typeid(void))),
          dirty(false),
          force_recompute(false),
          in_progress(false) {}
  };

  struct CellNode {
    AnyValue value;
    std::type_index type_id;
    EdgeVec dependents;

    CellNode() : type_id(std::type_index(typeid(void))) {}
  };

  struct EffectNode {
    EffectFnPtr run;
    EdgeVec dependencies;
    std::optional<CleanupFn> cleanup;
    bool force_run;

    EffectNode() : force_run(false) {}
  };

  using Node = std::variant<SlotNode, CellNode, EffectNode>;

 public:
  ContextImpl() = default;
  ContextImpl(const ContextImpl&) = delete;
  ContextImpl& operator=(const ContextImpl&) = delete;

  // -- Slot API --
  template <typename T, typename F>
  SlotHandle<T> slot(F&& compute) {
    return slot_with_equals<T>(std::forward<F>(compute), std::nullopt);
  }

  template <typename T, typename F>
  SlotHandle<T> computed(F&& compute) {
    return slot<T>(std::forward<F>(compute));
  }

  template <typename T, typename F>
  SlotHandle<T> memo(F&& compute) {
    auto eq = EqualsFn([](const void* a, const void* b) {
      return *static_cast<const T*>(a) == *static_cast<const T*>(b);
    });
    return slot_with_equals<T>(std::forward<F>(compute), std::move(eq));
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
    node.type_id = std::type_index(typeid(T));
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
    if (!n) return std::nullopt;
    const SlotNode* slot = std::get_if<SlotNode>(&n.value());
    if (!slot || !slot->value || slot->dirty || slot->force_recompute)
      return std::nullopt;
    return *Traits::template cast<T>(slot->value);
  }

  template <typename T>
  std::optional<T> peek_cell(const CellHandle<T>& handle) const {
    auto idx = node_index(handle.id);
    if (!idx || *idx >= nodes_.size()) return std::nullopt;
    auto& n = nodes_[*idx];
    if (!n) return std::nullopt;
    const CellNode* cell = std::get_if<CellNode>(&n.value());
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
      invalidate_cell_dependents_now(handle.id);
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
    EdgeVec old_deps;
    std::optional<CleanupFn> cleanup;
    {
      deque_erase(pending_effects_, handle.id);
      scheduled_effects_.erase(handle.id);
      auto idx = node_index(handle.id);
      if (!idx || *idx >= nodes_.size() || !nodes_[*idx]) return;
      auto& node_opt = nodes_[*idx];
      auto* eff = std::get_if<EffectNode>(&node_opt.value());
      if (!eff) return;
      old_deps = std::move(eff->dependencies);
      cleanup = std::move(eff->cleanup);
      nodes_[*idx].reset();
      free_ids_.push_back(handle.id.value);
    }
    for (auto dep_id : old_deps)
      remove_dependent_edge(dep_id, handle.id);
    if (cleanup && *cleanup) (*cleanup)();
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
  std::vector<std::optional<Node>> nodes_;
  uint64_t next_id_ = 0;
  std::vector<uint64_t> free_ids_;
  std::deque<SlotId> pending_effects_;
  std::unordered_set<SlotId> scheduled_effects_;
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
    return n ? &n.value() : nullptr;
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
  SlotHandle<T> slot_with_equals(F&& compute, std::optional<EqualsFn> eq) {
    auto id = alloc_id();
    SlotNode node;
    node.type_id = std::type_index(typeid(T));
    auto compute_fn =
        ComputeFn([f = std::forward<F>(compute)](ContextImpl& ctx) -> AnyValue {
          return Traits::template make<T>(f(ctx));
        });
    node.compute = ComputeFnPtr(new RcBox<ComputeFn>(std::move(compute_fn)),
                                typename ComputeFnPtr::adopt_t{});
    node.equals = std::move(eq);
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
    if (!enter_refresh(id)) return false;
    struct RefreshGuard {
      ContextImpl* ctx;
      SlotId id;
      ~RefreshGuard() {
        if (auto* slot = ctx->get_slot_node(id))
          slot->in_progress = false;
      }
    } guard{this, id};

    EdgeVec dependencies;
    {
      auto* slot = get_slot_node(id);
      if (!slot) return false;
      dependencies = slot->dependencies;
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
    EdgeVec old_deps;
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
      if (slot->value && slot->equals) {
        unchanged = (*slot->equals)(slot->value.raw(), result.raw());
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
    EdgeVec dependents;
    {
      auto* slot = get_slot_node(id);
      if (!slot) return;
      dependents = slot->dependents;
    }
    for (auto dep_id : dependents)
      invalidate_dependent_from_changed_value(dep_id);
  }

  void invalidate_dependent_from_changed_value(SlotId id) {
    auto* node = get_node(id);
    bool is_effect = node && std::holds_alternative<EffectNode>(*node);
    if (is_effect) {
      schedule_effect(id, true);
    } else {
      mark_slot_dirty(id, true);
    }
  }

  void mark_slot_dirty(SlotId id, bool force_recompute) {
    EdgeVec dependents;
    bool should_propagate;
    {
      auto* slot = get_slot_node(id);
      if (!slot) return;
      should_propagate = !slot->dirty || (force_recompute && !slot->force_recompute);
      slot->dirty = true;
      if (force_recompute)
        slot->force_recompute = true;
      dependents = slot->dependents;
    }
    if (!should_propagate) return;
    for (auto dep_id : dependents) {
      auto* node = get_node(dep_id);
      bool is_effect = node && std::holds_alternative<EffectNode>(*node);
      if (is_effect) {
        schedule_effect(dep_id, false);
      } else {
        mark_slot_dirty(dep_id, false);
      }
    }
  }

  void invalidate_cell_dependents_now(SlotId id) {
    EdgeVec dependents;
    {
      auto* cell = get_cell_node(id);
      if (!cell) return;
      dependents = cell->dependents;
    }
    for (auto dep_id : dependents)
      invalidate_dependent_from_changed_value(dep_id);
  }

  void clear_cell_dependents_now(SlotId id) {
    EdgeVec dependents;
    {
      auto* cell = get_cell_node(id);
      if (!cell) return;
      dependents = cell->dependents;
    }
    for (auto dep_id : dependents)
      clear_dependent_now(dep_id);
  }

  void clear_dependent_now(SlotId id) {
    auto* node = get_node(id);
    bool is_effect = node && std::holds_alternative<EffectNode>(*node);
    if (is_effect) {
      schedule_effect(id, true);
    } else {
      clear_slot_now(id);
    }
  }

  void clear_slot_now(SlotId id) {
    EdgeVec dependents;
    {
      auto* slot = get_slot_node(id);
      if (!slot) return;
      if (!slot->value && !slot->dirty) return;
      slot->value.reset();
      slot->dirty = false;
      slot->force_recompute = false;
      dependents = slot->dependents;
    }
    for (auto dep_id : dependents)
      clear_dependent_now(dep_id);
  }

  void schedule_effect(SlotId id, bool force) {
    auto* node = get_node(id);
    if (!node) return;
    auto* eff = std::get_if<EffectNode>(node);
    if (!eff) return;
    if (force) eff->force_run = true;
    if (scheduled_effects_.insert(id).second) {
      pending_effects_.push_back(id);
    }
  }

  void remove_pending_effect(SlotId id) {
    deque_erase(pending_effects_, id);
    scheduled_effects_.erase(id);
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
        id = pending_effects_.front();
        pending_effects_.pop_front();
        scheduled_effects_.erase(id);
      }
      run_effect(id);
    }
  }

  void run_effect(SlotId id) {
    if (!effect_should_run(id)) return;
    remove_pending_effect(id);

    EffectFnPtr run;
    EdgeVec old_deps;
    std::optional<CleanupFn> cleanup;
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
    if (cleanup && *cleanup) (*cleanup)();

    push_tracking_frame(id);
    auto next_cleanup = (*run).value(*this);
    pop_tracking_frame();

    auto* node = get_node(id);
    if (node) {
      auto* effect = std::get_if<EffectNode>(node);
      if (effect) {
        effect->cleanup = next_cleanup ? std::optional<CleanupFn>(std::move(next_cleanup)) : std::nullopt;
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
