#ifndef LAZILY_QUEUE_HPP
#define LAZILY_QUEUE_HPP

#include <lazily/context.hpp>

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <utility>

namespace lazily {

// -- Result types (shared across every QueueStorage backend) --

enum class PushResult {
  Ok,
  Full,
  Closed,
};

template <typename T>
struct PopResult {
  enum class Kind { Value, Empty, Closed };
  Kind kind;
  std::optional<T> value;

  static PopResult with_value(T v) { return {Kind::Value, std::optional<T>(std::move(v))}; }
  static PopResult empty() { return {Kind::Empty, std::nullopt}; }
  static PopResult closed() { return {Kind::Closed, std::nullopt}; }

  bool is_value() const { return kind == Kind::Value; }
  bool is_empty() const { return kind == Kind::Empty; }
  bool is_closed() const { return kind == Kind::Closed; }
};

// -- VecDequeStorage: the default unbounded / bounded reference backend --
//
// A QueueStorage backend conforms (per lazily-spec/cell-model.md § "Storage
// backend contract") when it provides:
//   try_push(T) -> PushResult      (Ok / Full / Closed)
//   try_pop()   -> PopResult<T>    (Value / Empty / Closed)
//   head()      -> std::optional<T>
//   len()       -> size_t
//   capacity()  -> std::optional<size_t>
//   is_full()   -> bool
//   is_closed() -> bool
//   close()     -> void
// FIFO order is total under SPSC; closure is monotonic (close is idempotent and
// terminal; push after close returns Closed; pop on closed+empty returns Closed,
// distinct from Empty; pop on closed+non-empty drains and returns the element).
// The default overflow policy is reject (try_push at capacity returns Full).

template <typename T>
class VecDequeStorage {
 public:
  VecDequeStorage() = default;
  explicit VecDequeStorage(size_t capacity) : capacity_(capacity) {}

  PushResult try_push(T v) {
    if (closed_) return PushResult::Closed;
    if (capacity_ && elements_.size() >= *capacity_) return PushResult::Full;
    elements_.push_back(std::move(v));
    return PushResult::Ok;
  }

  PopResult<T> try_pop() {
    if (elements_.empty())
      return closed_ ? PopResult<T>::closed() : PopResult<T>::empty();
    T v = std::move(elements_.front());
    elements_.pop_front();
    return PopResult<T>::with_value(std::move(v));
  }

  std::optional<T> head() const {
    if (elements_.empty()) return std::nullopt;
    return elements_.front();
  }

  size_t len() const { return elements_.size(); }
  std::optional<size_t> capacity() const { return capacity_; }

  bool is_full() const {
    return capacity_.has_value() && elements_.size() >= *capacity_;
  }

  bool is_closed() const { return closed_; }

  void close() { closed_ = true; }

 private:
  std::deque<T> elements_;
  std::optional<size_t> capacity_;
  bool closed_ = false;
};

// -- QueueCell: the reactive shell --
//
// The formal counterpart is LazilyFormal/QueueCell.lean and the observable
// contract is pinned by lazily-spec/conformance/collections/queuecell_*.json.
//
// `QueueCell` is specified as a single-producer, single-consumer (SPSC)
// primitive. MPSC (multi-producer, single-consumer) is a *usage rule* on the
// same primitive, not a separate type: multiple producers push to the same
// tail inside a `Context::batch(...)`; the batch boundary serializes the
// pushes into a deterministic order. There is no `MPSCQueueCell` type.
//
// The shell factors into two layers (lazily-spec/cell-model.md § "Reactive
// shell vs storage backend"):
//   - Reactive shell (this class): owns the version cells and the
//     reader-kind-scoped invalidation logic; storage-agnostic.
//   - Storage backend (Storage template param, default VecDequeStorage):
//     owns the actual FIFO data structure; pluggable at compile time.
//
// Invalidation is scoped by reader kind, not by position:
//   - push to empty   -> head + len + is_empty (+ is_full when it fills)
//   - push to nonempty -> len (+ is_full when it fills)
//   - pop (nonempty)  -> head + len (+ is_empty when it empties, + is_full when
//                        it un-fills)
//   - pop (empty)     -> no invalidation (returns Empty / Closed)
//   - try_push Full   -> no invalidation
//   - close (first)   -> closed
//   - close (again)   -> no invalidation (idempotent)
// Neither push nor pop changes `closed`; close changes only `closed`
// (close_preserves_{elements,head,length} per the formal model).
//
// The head reader observes the *current* head value — after a pop it sees the
// next element (or empty), not a stale value. There is no random-access
// `queue[N]` reader; per-position reactivity is the domain of `CellMap`.

template <typename T, typename Storage = VecDequeStorage<T>>
struct QueueCellInner {
  Storage storage;
  CellHandle<uint64_t> head_cell;
  uint64_t head_version;
  CellHandle<uint64_t> len_cell;
  uint64_t len_version;
  CellHandle<uint64_t> empty_cell;
  uint64_t empty_version;
  CellHandle<uint64_t> full_cell;
  uint64_t full_version;
  CellHandle<uint64_t> closed_cell;
  uint64_t closed_version;

  explicit QueueCellInner(Storage s) : storage(std::move(s)) {}
};

template <typename T, typename Storage = VecDequeStorage<T>>
class QueueCell {
 public:
  using value_type = T;

  // Unbounded default (capacity() == nullopt, is_full() always false).
  explicit QueueCell(Context& ctx) : QueueCell(ctx, Storage{}) {}

  // Custom storage backend (compile-time pluggable seam).
  QueueCell(Context& ctx, Storage storage)
      : inner_(std::make_shared<QueueCellInner<T, Storage>>(std::move(storage))) {
    inner_->head_cell = ctx.cell(uint64_t(0));
    inner_->head_version = 0;
    inner_->len_cell = ctx.cell(uint64_t(0));
    inner_->len_version = 0;
    inner_->empty_cell = ctx.cell(uint64_t(0));
    inner_->empty_version = 0;
    inner_->full_cell = ctx.cell(uint64_t(0));
    inner_->full_version = 0;
    inner_->closed_cell = ctx.cell(uint64_t(0));
    inner_->closed_version = 0;
  }

  // Bounded convenience factory (reject overflow policy).
  static QueueCell bounded(Context& ctx, size_t capacity) {
    return QueueCell(ctx, Storage(capacity));
  }

  // -- Mutating ops --

  // try_push: returns Ok / Full / Closed. No invalidation on Full or Closed.
  PushResult try_push(Context& ctx, T v) {
    const bool was_empty = inner_->storage.len() == 0;
    const bool was_full = inner_->storage.is_full();
    PushResult r = inner_->storage.try_push(std::move(v));
    if (r != PushResult::Ok) return r;

    bump_len(ctx);
    if (was_empty) {
      bump_head(ctx);
      bump_empty(ctx);
    }
    if (!was_full && inner_->storage.is_full()) bump_full(ctx);
    return r;
  }

  // try_pop: returns Value / Empty / Closed. No invalidation on Empty or Closed.
  PopResult<T> try_pop(Context& ctx) {
    if (inner_->storage.len() == 0)
      return inner_->storage.is_closed() ? PopResult<T>::closed()
                                         : PopResult<T>::empty();

    const bool was_full = inner_->storage.is_full();
    PopResult<T> r = inner_->storage.try_pop();

    // A successful pop always changes the head value (front -> next, or
    // front -> empty), per the formal `pop_returns_oldest` / popped-head
    // observation laws.
    bump_head(ctx);
    bump_len(ctx);
    if (inner_->storage.len() == 0) bump_empty(ctx);
    if (was_full) bump_full(ctx);
    return r;
  }

  // push / pop convenience wrappers (happy path; assert success).
  void push(Context& ctx, T v) {
    PushResult r = try_push(ctx, std::move(v));
    assert(r == PushResult::Ok && "lazily::QueueCell::push failed (Full/Closed)");
  }

  std::optional<T> pop(Context& ctx) {
    PopResult<T> r = try_pop(ctx);
    if (r.is_value()) return std::move(r.value);
    return std::nullopt;
  }

  // Close is idempotent and terminal: the first close flips `closed` to true
  // and invalidates closed readers; subsequent closes are no-ops.
  void close(Context& ctx) {
    const bool was = inner_->storage.is_closed();
    inner_->storage.close();
    if (!was && inner_->storage.is_closed()) bump_closed(ctx);
  }

  // -- Reactive reads (each establishes a dependency on its reader-kind cell) --

  std::optional<T> head(Context& ctx) {
    (void)ctx.get_cell(inner_->head_cell);
    return inner_->storage.head();
  }

  size_t len(Context& ctx) {
    (void)ctx.get_cell(inner_->len_cell);
    return inner_->storage.len();
  }

  bool is_empty(Context& ctx) {
    (void)ctx.get_cell(inner_->empty_cell);
    return inner_->storage.len() == 0;
  }

  bool is_full(Context& ctx) {
    (void)ctx.get_cell(inner_->full_cell);
    return inner_->storage.is_full();
  }

  bool closed(Context& ctx) {
    (void)ctx.get_cell(inner_->closed_cell);
    return inner_->storage.is_closed();
  }

  // -- Non-reactive introspection (no dependency registered) --

  std::optional<size_t> capacity() const { return inner_->storage.capacity(); }
  size_t len_untracked() const { return inner_->storage.len(); }
  bool is_closed_untracked() const { return inner_->storage.is_closed(); }

  // Access the underlying storage (for backends that expose snapshot/serialize).
  Storage& storage() { return inner_->storage; }
  const Storage& storage() const { return inner_->storage; }

 private:
  std::shared_ptr<QueueCellInner<T, Storage>> inner_;

  void bump_head(Context& ctx) {
    ctx.set_cell(inner_->head_cell, ++inner_->head_version);
  }
  void bump_len(Context& ctx) {
    ctx.set_cell(inner_->len_cell, ++inner_->len_version);
  }
  void bump_empty(Context& ctx) {
    ctx.set_cell(inner_->empty_cell, ++inner_->empty_version);
  }
  void bump_full(Context& ctx) {
    ctx.set_cell(inner_->full_cell, ++inner_->full_version);
  }
  void bump_closed(Context& ctx) {
    ctx.set_cell(inner_->closed_cell, ++inner_->closed_version);
  }
};

}  // namespace lazily

#endif  // LAZILY_QUEUE_HPP
