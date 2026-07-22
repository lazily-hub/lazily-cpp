#ifndef LAZILY_QUEUE_HPP
#define LAZILY_QUEUE_HPP

#include <lazily/context.hpp>
#include <lazily/cell.hpp>

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lazily {

// -- Optional-capability detection (C++17 void_t idiom, Phase 0 #relaycell) --
// `head` / `capacity` / `is_full` are OPTIONAL QueueStorage capabilities; these
// traits let the shell compile against a raw-channel backend that provides
// none.
namespace queue_detail {
template <typename S, typename = void> struct has_head : std::false_type {};
template <typename S>
struct has_head<S, std::void_t<decltype(std::declval<const S &>().head())>>
    : std::true_type {};

template <typename S, typename = void> struct has_capacity : std::false_type {};
template <typename S>
struct has_capacity<S,
                    std::void_t<decltype(std::declval<const S &>().capacity())>>
    : std::true_type {};

template <typename S, typename = void> struct has_is_full : std::false_type {};
template <typename S>
struct has_is_full<S,
                   std::void_t<decltype(std::declval<const S &>().is_full())>>
    : std::true_type {};
} // namespace queue_detail

// -- Result types (shared across every QueueStorage backend) --

enum class PushResult {
  Ok,
  Full,
  Closed,
};

template <typename T> struct PopResult {
  enum class Kind { Value, Empty, Closed };
  Kind kind;
  std::optional<T> value;

  static PopResult with_value(T v) {
    return {Kind::Value, std::optional<T>(std::move(v))};
  }
  static PopResult empty() { return {Kind::Empty, std::nullopt}; }
  static PopResult closed() { return {Kind::Closed, std::nullopt}; }

  bool is_value() const { return kind == Kind::Value; }
  bool is_empty() const { return kind == Kind::Empty; }
  bool is_closed() const { return kind == Kind::Closed; }
};

// -- VecDequeStorage: the default unbounded / bounded reference backend --
//
// A QueueStorage backend conforms (per lazily-spec/cell-model.md § "Storage
// backend contract"). Minimal REQUIRED contract (Phase 0 #relaycell):
//   try_push(T) -> PushResult      (Ok / Full / Closed)
//   try_pop()   -> PopResult<T>    (Value / Empty / Closed)
//   len()       -> size_t
//   is_closed() -> bool
//   close()     -> void
// OPTIONAL capabilities (detected at compile time via the queue_detail traits):
//   head()      -> std::optional<T>       (gains a head reader; else nullopt)
//   capacity()  -> std::optional<size_t>  (gains a bound; else unbounded)
//   is_full()   -> bool                   (gains the is_full backpressure
//   reader)
// A raw-channel-style backend providing only the five required members is fully
// conforming (no head reader, never full).
// FIFO order is total under SPSC; closure is monotonic (close is idempotent and
// terminal; push after close returns Closed; pop on closed+empty returns
// Closed, distinct from Empty; pop on closed+non-empty drains and returns the
// element). The default overflow policy is reject (try_push at capacity returns
// Full).

template <typename T> class VecDequeStorage {
public:
  VecDequeStorage() = default;
  explicit VecDequeStorage(size_t capacity) : capacity_(capacity) {}

  PushResult try_push(T v) {
    if (closed_)
      return PushResult::Closed;
    if (capacity_ && elements_.size() >= *capacity_)
      return PushResult::Full;
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
    if (elements_.empty())
      return std::nullopt;
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
  Source<uint64_t> head_cell;
  uint64_t head_version;
  Source<uint64_t> len_cell;
  uint64_t len_version;
  Source<uint64_t> empty_cell;
  uint64_t empty_version;
  Source<uint64_t> full_cell;
  uint64_t full_version;
  Source<uint64_t> closed_cell;
  uint64_t closed_version;

  explicit QueueCellInner(Storage s) : storage(std::move(s)) {}
};

template <typename T, typename Storage = VecDequeStorage<T>> class QueueCell {
public:
  using value_type = T;

  // Unbounded default (capacity() == nullopt, is_full() always false).
  explicit QueueCell(Context &ctx) : QueueCell(ctx, Storage{}) {}

  // Custom storage backend (compile-time pluggable seam).
  QueueCell(Context &ctx, Storage storage)
      : inner_(
            std::make_shared<QueueCellInner<T, Storage>>(std::move(storage))) {
    inner_->head_cell = ctx.source(uint64_t(0));
    inner_->head_version = 0;
    inner_->len_cell = ctx.source(uint64_t(0));
    inner_->len_version = 0;
    inner_->empty_cell = ctx.source(uint64_t(0));
    inner_->empty_version = 0;
    inner_->full_cell = ctx.source(uint64_t(0));
    inner_->full_version = 0;
    inner_->closed_cell = ctx.source(uint64_t(0));
    inner_->closed_version = 0;
  }

  // Bounded convenience factory (reject overflow policy).
  static QueueCell bounded(Context &ctx, size_t capacity) {
    return QueueCell(ctx, Storage(capacity));
  }

  // -- Mutating ops --

  // try_push: returns Ok / Full / Closed. No invalidation on Full or Closed.
  PushResult try_push(Context &ctx, T v) {
    const bool was_empty = inner_->storage.len() == 0;
    bool was_full = false;
    if constexpr (queue_detail::has_is_full<Storage>::value) {
      was_full = inner_->storage.is_full();
    }
    PushResult r = inner_->storage.try_push(std::move(v));
    if (r != PushResult::Ok)
      return r;

    bump_len(ctx);
    if (was_empty) {
      bump_head(ctx);
      bump_empty(ctx);
    }
    if constexpr (queue_detail::has_is_full<Storage>::value) {
      if (!was_full && inner_->storage.is_full())
        bump_full(ctx);
    }
    return r;
  }

  // try_pop: returns Value / Empty / Closed. No invalidation on Empty or
  // Closed.
  PopResult<T> try_pop(Context &ctx) {
    if (inner_->storage.len() == 0)
      return inner_->storage.is_closed() ? PopResult<T>::closed()
                                         : PopResult<T>::empty();

    bool was_full = false;
    if constexpr (queue_detail::has_is_full<Storage>::value) {
      was_full = inner_->storage.is_full();
    }
    PopResult<T> r = inner_->storage.try_pop();

    // A successful pop always changes the head value (front -> next, or
    // front -> empty), per the formal `pop_returns_oldest` / popped-head
    // observation laws.
    bump_head(ctx);
    bump_len(ctx);
    if (inner_->storage.len() == 0)
      bump_empty(ctx);
    if constexpr (queue_detail::has_is_full<Storage>::value) {
      if (was_full)
        bump_full(ctx);
    }
    return r;
  }

  // push / pop convenience wrappers (happy path; assert success).
  void push(Context &ctx, T v) {
    PushResult r = try_push(ctx, std::move(v));
    assert(r == PushResult::Ok &&
           "lazily::QueueCell::push failed (Full/Closed)");
  }

  std::optional<T> pop(Context &ctx) {
    PopResult<T> r = try_pop(ctx);
    if (r.is_value())
      return std::move(r.value);
    return std::nullopt;
  }

  // Close is idempotent and terminal: the first close flips `closed` to true
  // and invalidates closed readers; subsequent closes are no-ops.
  void close(Context &ctx) {
    const bool was = inner_->storage.is_closed();
    inner_->storage.close();
    if (!was && inner_->storage.is_closed())
      bump_closed(ctx);
  }

  // -- Reactive reads (each establishes a dependency on its reader-kind cell)
  // --

  template <typename Cx>
  std::optional<T> head(Cx &ctx) {
    (void)ctx.get(inner_->head_cell);
    // `head()` is an OPTIONAL storage capability (Phase 0 #relaycell): a
    // raw-channel backend that cannot peek has no head reader (nullopt).
    if constexpr (queue_detail::has_head<Storage>::value) {
      return inner_->storage.head();
    } else {
      return std::nullopt;
    }
  }

  template <typename Cx>
  size_t len(Cx &ctx) {
    (void)ctx.get(inner_->len_cell);
    return inner_->storage.len();
  }

  template <typename Cx>
  bool is_empty(Cx &ctx) {
    (void)ctx.get(inner_->empty_cell);
    return inner_->storage.len() == 0;
  }

  template <typename Cx>
  bool is_full(Cx &ctx) {
    (void)ctx.get(inner_->full_cell);
    // `is_full()`/`capacity()` are OPTIONAL: a backend without a bound is
    // unbounded and never full.
    if constexpr (queue_detail::has_is_full<Storage>::value) {
      return inner_->storage.is_full();
    } else {
      return false;
    }
  }

  template <typename Cx>
  bool closed(Cx &ctx) {
    (void)ctx.get(inner_->closed_cell);
    return inner_->storage.is_closed();
  }

  // -- Non-reactive introspection (no dependency registered) --

  std::optional<size_t> capacity() const {
    if constexpr (queue_detail::has_capacity<Storage>::value) {
      return inner_->storage.capacity();
    } else {
      return std::nullopt;
    }
  }
  size_t len_untracked() const { return inner_->storage.len(); }
  bool is_closed_untracked() const { return inner_->storage.is_closed(); }

  // Access the underlying storage (for backends that expose
  // snapshot/serialize).
  Storage &storage() { return inner_->storage; }
  const Storage &storage() const { return inner_->storage; }

private:
  std::shared_ptr<QueueCellInner<T, Storage>> inner_;

  void bump_head(Context &ctx) {
    ctx.set(inner_->head_cell, ++inner_->head_version);
  }
  void bump_len(Context &ctx) {
    ctx.set(inner_->len_cell, ++inner_->len_version);
  }
  void bump_empty(Context &ctx) {
    ctx.set(inner_->empty_cell, ++inner_->empty_version);
  }
  void bump_full(Context &ctx) {
    ctx.set(inner_->full_cell, ++inner_->full_version);
  }
  void bump_closed(Context &ctx) {
    ctx.set(inner_->closed_cell, ++inner_->closed_version);
  }
};

// -- TopicCell: broadcast log with independent subscriber cursors --

enum class TopicDurability { Durable, Ephemeral };

enum class TopicSubscribeOutcome { Subscribed, Reconnected, AlreadySubscribed };

struct TopicSubscriptionSnapshot {
std::string subscriber_id;
size_t cursor = 0;
TopicDurability durability = TopicDurability::Durable;
bool connected = false;
};

template <typename T> struct TopicSnapshot {
size_t base_offset = 0;
std::vector<T> elements;
std::vector<TopicSubscriptionSnapshot> subscriptions;
};

namespace queue_detail {
struct TopicSubscription {
size_t cursor = 0;
TopicDurability durability = TopicDurability::Durable;
bool connected = false;
};

struct TopicReader {
Source<uint64_t> cell;
uint64_t version = 0;
};

template <typename T> struct TopicCellInner {
size_t base_offset = 0;
std::deque<T> elements;
std::unordered_map<std::string, TopicSubscription> subscriptions;
std::unordered_map<std::string, TopicReader> readers;
};
} // namespace queue_detail

/// Broadcast topic whose stable subscribers own independent absolute cursors.
/// Durable offline subscribers retain data; ephemeral subscribers disappear on
/// disconnect. `gc` is safe by construction and invalidates no reader.
template <typename T> class TopicCell {
public:
explicit TopicCell(Context &ctx)
: inner_(std::make_shared<queue_detail::TopicCellInner<T>>()) {
(void)ctx;
}

TopicCell(Context &ctx, const TopicSnapshot<T> &snapshot)
: inner_(std::make_shared<queue_detail::TopicCellInner<T>>()) {
inner_->base_offset = snapshot.base_offset;
inner_->elements.assign(snapshot.elements.begin(), snapshot.elements.end());
const size_t tail = tail_offset();
for (const auto &saved : snapshot.subscriptions) {
if (saved.cursor < inner_->base_offset || saved.cursor > tail)
throw std::invalid_argument("TopicCell cursor outside retained log");
if (saved.durability == TopicDurability::Ephemeral && !saved.connected)
throw std::invalid_argument(
"disconnected ephemeral TopicCell subscription must be removed");
inner_->subscriptions.emplace(
saved.subscriber_id,
queue_detail::TopicSubscription{saved.cursor, saved.durability,
saved.connected});
ensure_reader(ctx, saved.subscriber_id);
}
}

TopicSubscribeOutcome subscribe(
Context &ctx, const std::string &subscriber_id,
TopicDurability durability = TopicDurability::Durable) {
auto found = inner_->subscriptions.find(subscriber_id);
if (found != inner_->subscriptions.end()) {
auto &subscription = found->second;
if (subscription.connected)
return TopicSubscribeOutcome::AlreadySubscribed;
if (subscription.durability != TopicDurability::Durable)
throw std::logic_error("only durable subscriptions can reconnect");
subscription.connected = true;
bump_reader(ctx, subscriber_id);
return TopicSubscribeOutcome::Reconnected;
}
inner_->subscriptions.emplace(
subscriber_id,
queue_detail::TopicSubscription{tail_offset(), durability, true});
ensure_reader(ctx, subscriber_id);
bump_reader(ctx, subscriber_id);
return TopicSubscribeOutcome::Subscribed;
}

void reconnect(Context &ctx, const std::string &subscriber_id) {
auto found = inner_->subscriptions.find(subscriber_id);
if (found == inner_->subscriptions.end() ||
found->second.durability != TopicDurability::Durable)
throw std::logic_error("durable subscription not found");
if (!found->second.connected) {
found->second.connected = true;
bump_reader(ctx, subscriber_id);
}
}

void disconnect(Context &ctx, const std::string &subscriber_id) {
auto found = inner_->subscriptions.find(subscriber_id);
if (found == inner_->subscriptions.end() || !found->second.connected)
return;
found->second.connected = false;
const bool ephemeral =
found->second.durability == TopicDurability::Ephemeral;
if (ephemeral)
inner_->subscriptions.erase(found);
bump_reader(ctx, subscriber_id);
}

size_t publish(Context &ctx, T value) {
const size_t offset = tail_offset();
inner_->elements.push_back(std::move(value));
for (const auto &entry : inner_->subscriptions) {
if (entry.second.connected)
bump_reader(ctx, entry.first);
}
return offset;
}

std::vector<T> read_stream(Context &ctx,
const std::string &subscriber_id) {
(void)ctx.get(ensure_reader(ctx, subscriber_id));
auto found = inner_->subscriptions.find(subscriber_id);
if (found == inner_->subscriptions.end() || !found->second.connected)
return {};
const size_t start = found->second.cursor - inner_->base_offset;
auto begin = inner_->elements.begin();
std::advance(begin, static_cast<typename std::deque<T>::difference_type>(start));
return std::vector<T>(begin, inner_->elements.end());
}

std::optional<T> read(Context &ctx, const std::string &subscriber_id) {
auto stream = read_stream(ctx, subscriber_id);
if (stream.empty())
return std::nullopt;
return stream.front();
}

size_t advance(Context &ctx, const std::string &subscriber_id,
size_t count = 1) {
auto found = inner_->subscriptions.find(subscriber_id);
if (found == inner_->subscriptions.end())
throw std::out_of_range("invalid TopicCell cursor advance");
if (!found->second.connected || found->second.cursor == tail_offset())
return found->second.cursor;
if (count > tail_offset() - found->second.cursor)
throw std::out_of_range("invalid TopicCell cursor advance");
if (count != 0) {
found->second.cursor += count;
bump_reader(ctx, subscriber_id);
}
return found->second.cursor;
}

/// Drop only the prefix below every durable cursor. No reader invalidation.
size_t gc() {
size_t frontier = tail_offset();
for (const auto &entry : inner_->subscriptions) {
const auto &subscription = entry.second;
if (subscription.durability == TopicDurability::Durable &&
subscription.cursor < frontier)
frontier = subscription.cursor;
}
const size_t removed = frontier - inner_->base_offset;
for (size_t i = 0; i < removed; ++i)
inner_->elements.pop_front();
inner_->base_offset = frontier;
return removed;
}

void restart() {}

size_t base_offset() const { return inner_->base_offset; }
size_t tail_offset() const {
return inner_->base_offset + inner_->elements.size();
}
std::vector<T> elements() const {
return std::vector<T>(inner_->elements.begin(), inner_->elements.end());
}

std::optional<TopicSubscriptionSnapshot>
subscription(const std::string &subscriber_id) const {
auto found = inner_->subscriptions.find(subscriber_id);
if (found == inner_->subscriptions.end())
return std::nullopt;
return TopicSubscriptionSnapshot{subscriber_id, found->second.cursor,
found->second.durability,
found->second.connected};
}

Source<uint64_t> reader_handle(Context &ctx,
const std::string &subscriber_id) {
return ensure_reader(ctx, subscriber_id);
}

TopicSnapshot<T> snapshot() const {
TopicSnapshot<T> result;
result.base_offset = inner_->base_offset;
result.elements = elements();
for (const auto &entry : inner_->subscriptions) {
result.subscriptions.push_back(TopicSubscriptionSnapshot{
entry.first, entry.second.cursor, entry.second.durability,
entry.second.connected});
}
return result;
}

private:
std::shared_ptr<queue_detail::TopicCellInner<T>> inner_;

Source<uint64_t> ensure_reader(Context &ctx,
const std::string &subscriber_id) {
auto found = inner_->readers.find(subscriber_id);
if (found != inner_->readers.end())
return found->second.cell;
auto inserted = inner_->readers.emplace(
subscriber_id,
queue_detail::TopicReader{ctx.source(uint64_t(0)), uint64_t(0)});
return inserted.first->second.cell;
}

void bump_reader(Context &ctx, const std::string &subscriber_id) {
auto handle = ensure_reader(ctx, subscriber_id);
auto &reader = inner_->readers.at(subscriber_id);
ctx.set(handle, ++reader.version);
}
};

} // namespace lazily

#endif // LAZILY_QUEUE_HPP
