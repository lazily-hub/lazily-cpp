#ifndef LAZILY_QUEUE_HPP
#define LAZILY_QUEUE_HPP

#include <lazily/async_context.hpp>
#include <lazily/cell.hpp>
#include <lazily/context.hpp>
#include <lazily/thread_safe.hpp>

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
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

inline Context &graph(Context &ctx) { return ctx; }
inline Context &graph(ThreadSafeContext &ctx) { return ctx.context(); }
inline Context &graph(AsyncContext &ctx) { return ctx.context(); }

template <typename F> void batch(Context &ctx, F &&fn) {
  ctx.batch([&](Context &graph) { fn(graph); });
}

template <typename F> void batch(ThreadSafeContext &ctx, F &&fn) {
  ctx.batch([&](Context &graph) { fn(graph); });
}

template <typename F> void batch(AsyncContext &ctx, F &&fn) {
  ctx.context().batch([&](Context &graph) { fn(graph); });
}

template <typename Cx, typename T> T read(Cx &ctx, const Computed<T> &handle) {
  return ctx.get(handle);
}
template <typename T> T read(AsyncContext &ctx, const Computed<T> &handle) {
  return ctx.context().get(handle);
}
template <typename Cx, typename T, typename M>
T read(Cx &ctx, const Source<T, M> &handle) {
  return ctx.get(handle);
}
template <typename T, typename M>
T read(AsyncContext &ctx, const Source<T, M> &handle) {
  return ctx.context().get(handle);
}
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
//   - Reactive shell (this class): owns memoized derived reader kinds and the
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
// `queue[N]` reader; per-position reactivity is the domain of `SourceMap`.

template <typename T, typename Storage> struct QueueCellInner {
  mutable std::mutex mutex;
  Storage storage;
  std::optional<size_t> capacity;
  Computed<std::optional<T>> head;
  Computed<size_t> len;
  Computed<bool> is_empty;
  Computed<bool> is_full;
  Source<bool> closed;

  explicit QueueCellInner(Storage s) : storage(std::move(s)) {
    if constexpr (queue_detail::has_capacity<Storage>::value)
      capacity = storage.capacity();
  }
};

template <typename OwnerContext, typename T,
          typename Storage = VecDequeStorage<T>>
class BasicQueueCell {
public:
  using value_type = T;

  explicit BasicQueueCell(OwnerContext &ctx) : BasicQueueCell(ctx, Storage{}) {}

  BasicQueueCell(OwnerContext &ctx, Storage storage)
      : inner_(
            std::make_shared<QueueCellInner<T, Storage>>(std::move(storage))) {
    const auto inner = inner_;
    auto &graph = queue_detail::graph(ctx);
    inner_->head = graph.template computed<std::optional<T>>(
        [inner](Compute &) -> std::optional<T> {
          std::lock_guard<std::mutex> lock(inner->mutex);
          if constexpr (queue_detail::has_head<Storage>::value)
            return inner->storage.head();
          return std::nullopt;
        });
    inner_->len = graph.template computed<size_t>([inner](Compute &) {
      std::lock_guard<std::mutex> lock(inner->mutex);
      return inner->storage.len();
    });
    inner_->is_empty = graph.template computed<bool>([inner](Compute &) {
      std::lock_guard<std::mutex> lock(inner->mutex);
      return inner->storage.len() == 0;
    });
    inner_->is_full = graph.template computed<bool>([inner](Compute &) {
      std::lock_guard<std::mutex> lock(inner->mutex);
      if constexpr (queue_detail::has_is_full<Storage>::value)
        return inner->storage.is_full();
      return false;
    });
    {
      std::lock_guard<std::mutex> lock(inner_->mutex);
      inner_->closed = graph.source(inner_->storage.is_closed());
    }
  }

  // -- Mutating ops --

  // try_push: returns Ok / Full / Closed. No invalidation on Full or Closed.
  PushResult try_push(OwnerContext &ctx, T v) {
    size_t len_before;
    PushResult result;
    {
      std::lock_guard<std::mutex> lock(inner_->mutex);
      len_before = inner_->storage.len();
      result = inner_->storage.try_push(std::move(v));
    }
    if (result == PushResult::Ok)
      invalidate_readers(ctx, len_before, len_before + 1, len_before == 0);
    return result;
  }

  // try_pop: returns Value / Empty / Closed. No invalidation on Empty or
  // Closed.
  PopResult<T> try_pop(OwnerContext &ctx) {
    size_t len_before;
    PopResult<T> result;
    {
      std::lock_guard<std::mutex> lock(inner_->mutex);
      len_before = inner_->storage.len();
      result = inner_->storage.try_pop();
    }
    if (result.is_value())
      invalidate_readers(ctx, len_before, len_before - 1, true);
    return result;
  }

  // push / pop convenience wrappers (happy path; assert success).
  void push(OwnerContext &ctx, T v) {
    PushResult r = try_push(ctx, std::move(v));
    assert(r == PushResult::Ok && "lazily queue push failed (Full/Closed)");
  }

  std::optional<T> pop(OwnerContext &ctx) {
    PopResult<T> r = try_pop(ctx);
    if (r.is_value())
      return std::move(r.value);
    return std::nullopt;
  }

  // Close is idempotent and terminal: the first close flips `closed` to true
  // and invalidates closed readers; subsequent closes are no-ops.
  void close(OwnerContext &ctx) {
    bool newly_closed;
    {
      std::lock_guard<std::mutex> lock(inner_->mutex);
      const bool was = inner_->storage.is_closed();
      inner_->storage.close();
      newly_closed = !was && inner_->storage.is_closed();
    }
    if (newly_closed)
      queue_detail::graph(ctx).set(inner_->closed, true);
  }

  // -- Reactive reads (each establishes a dependency on its memoized reader)
  // --

  template <typename Cx> std::optional<T> head(Cx &ctx) {
    return queue_detail::read(ctx, inner_->head);
  }

  template <typename Cx> size_t len(Cx &ctx) {
    return queue_detail::read(ctx, inner_->len);
  }

  template <typename Cx> bool is_empty(Cx &ctx) {
    return queue_detail::read(ctx, inner_->is_empty);
  }

  template <typename Cx> bool is_full(Cx &ctx) {
    return queue_detail::read(ctx, inner_->is_full);
  }

  template <typename Cx> bool closed(Cx &ctx) {
    return queue_detail::read(ctx, inner_->closed);
  }

  // -- Non-reactive introspection (no dependency registered) --

  std::optional<size_t> capacity() const { return inner_->capacity; }
  size_t len_untracked() const {
    std::lock_guard<std::mutex> lock(inner_->mutex);
    return inner_->storage.len();
  }
  bool is_closed_untracked() const {
    std::lock_guard<std::mutex> lock(inner_->mutex);
    return inner_->storage.is_closed();
  }

  // Access the underlying storage (for backends that expose
  // snapshot/serialize). Only the single-threaded flavor should expose this
  // unguarded seam; retained for source compatibility.
  Storage &storage() { return inner_->storage; }
  const Storage &storage() const { return inner_->storage; }

  Computed<std::optional<T>> head_handle() const { return inner_->head; }
  Computed<size_t> len_handle() const { return inner_->len; }
  Computed<bool> is_empty_handle() const { return inner_->is_empty; }
  Computed<bool> is_full_handle() const { return inner_->is_full; }
  Source<bool> closed_handle() const { return inner_->closed; }

protected:
  std::shared_ptr<QueueCellInner<T, Storage>> inner_;

  void invalidate_readers(OwnerContext &ctx, size_t len_before,
                          size_t len_after, bool head_changed) {
    const bool empty_changed = (len_before == 0) != (len_after == 0);
    const bool full_changed =
        inner_->capacity &&
        ((len_before >= *inner_->capacity) != (len_after >= *inner_->capacity));
    queue_detail::batch(ctx, [&](Context &graph) {
      inner_->len.clear(graph);
      if (empty_changed)
        inner_->is_empty.clear(graph);
      if (full_changed)
        inner_->is_full.clear(graph);
      if (head_changed)
        inner_->head.clear(graph);
    });
  }
};

template <typename T, typename Storage = VecDequeStorage<T>>
class QueueCell : public BasicQueueCell<Context, T, Storage> {
  using Base = BasicQueueCell<Context, T, Storage>;

public:
  explicit QueueCell(Context &ctx) : Base(ctx) {}
  QueueCell(Context &ctx, Storage storage) : Base(ctx, std::move(storage)) {}
  static QueueCell bounded(Context &ctx, size_t capacity) {
    return QueueCell(ctx, Storage(capacity));
  }
};

template <typename T, typename Storage = VecDequeStorage<T>>
class ThreadSafeQueueCell
    : public BasicQueueCell<ThreadSafeContext, T, Storage> {
  using Base = BasicQueueCell<ThreadSafeContext, T, Storage>;

public:
  explicit ThreadSafeQueueCell(ThreadSafeContext &ctx) : Base(ctx) {}
  ThreadSafeQueueCell(ThreadSafeContext &ctx, Storage storage)
      : Base(ctx, std::move(storage)) {}
  static ThreadSafeQueueCell bounded(ThreadSafeContext &ctx, size_t capacity) {
    return ThreadSafeQueueCell(ctx, Storage(capacity));
  }
};

template <typename T, typename Storage = VecDequeStorage<T>>
class AsyncQueueCell : public BasicQueueCell<AsyncContext, T, Storage> {
  using Base = BasicQueueCell<AsyncContext, T, Storage>;

public:
  explicit AsyncQueueCell(AsyncContext &ctx) : Base(ctx) {}
  AsyncQueueCell(AsyncContext &ctx, Storage storage)
      : Base(ctx, std::move(storage)) {}
  static AsyncQueueCell bounded(AsyncContext &ctx, size_t capacity) {
    return AsyncQueueCell(ctx, Storage(capacity));
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

template <typename T> struct TopicCellInner {
  mutable std::mutex core_mutex;
  mutable std::mutex reader_mutex;
  size_t base_offset = 0;
  std::deque<T> elements;
  std::unordered_map<std::string, TopicSubscription> subscriptions;
  std::unordered_map<std::string, Computed<std::vector<T>>> readers;
};
} // namespace queue_detail

/// Broadcast topic whose stable subscribers own independent absolute cursors.
/// Durable offline subscribers retain data; ephemeral subscribers disappear on
/// disconnect. `gc` is safe by construction and invalidates no reader.
template <typename OwnerContext, typename T> class BasicTopicCell {
public:
  explicit BasicTopicCell(OwnerContext &ctx)
      : inner_(std::make_shared<queue_detail::TopicCellInner<T>>()) {
    (void)ctx;
  }

  BasicTopicCell(OwnerContext &ctx, const TopicSnapshot<T> &snapshot)
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

  TopicSubscribeOutcome
  subscribe(OwnerContext &ctx, const std::string &subscriber_id,
            TopicDurability durability = TopicDurability::Durable) {
    TopicSubscribeOutcome outcome;
    bool invalidate = false;
    {
      std::lock_guard<std::mutex> lock(inner_->core_mutex);
      auto found = inner_->subscriptions.find(subscriber_id);
      if (found != inner_->subscriptions.end()) {
        auto &subscription = found->second;
        if (subscription.connected)
          return TopicSubscribeOutcome::AlreadySubscribed;
        if (subscription.durability != TopicDurability::Durable)
          throw std::logic_error("only durable subscriptions can reconnect");
        subscription.connected = true;
        outcome = TopicSubscribeOutcome::Reconnected;
        invalidate = true;
      } else {
        inner_->subscriptions.emplace(
            subscriber_id, queue_detail::TopicSubscription{tail_offset_locked(),
                                                           durability, true});
        outcome = TopicSubscribeOutcome::Subscribed;
        // A caller may have pre-minted and primed a reader_handle for this id.
        // Creating the subscription changes that reader even though the usual
        // subscribe path only mints it below.
        invalidate = true;
      }
    }
    auto reader = ensure_reader(ctx, subscriber_id);
    if (invalidate)
      invalidate_one(ctx, reader);
    return outcome;
  }

  void reconnect(OwnerContext &ctx, const std::string &subscriber_id) {
    (void)subscribe(ctx, subscriber_id, TopicDurability::Durable);
  }

  void disconnect(OwnerContext &ctx, const std::string &subscriber_id) {
    bool changed = false;
    bool remove_reader = false;
    {
      std::lock_guard<std::mutex> lock(inner_->core_mutex);
      auto found = inner_->subscriptions.find(subscriber_id);
      if (found == inner_->subscriptions.end() || !found->second.connected)
        return;
      found->second.connected = false;
      remove_reader = found->second.durability == TopicDurability::Ephemeral;
      if (remove_reader)
        inner_->subscriptions.erase(found);
      changed = true;
    }
    if (!changed)
      return;
    std::optional<Computed<std::vector<T>>> reader;
    {
      std::lock_guard<std::mutex> lock(inner_->reader_mutex);
      auto found = inner_->readers.find(subscriber_id);
      if (found != inner_->readers.end()) {
        reader = found->second;
        if (remove_reader)
          inner_->readers.erase(found);
      }
    }
    if (reader)
      invalidate_one(ctx, *reader);
  }

  size_t publish(OwnerContext &ctx, T value) {
    size_t offset;
    std::vector<std::string> connected;
    {
      std::lock_guard<std::mutex> lock(inner_->core_mutex);
      offset = tail_offset_locked();
      inner_->elements.push_back(std::move(value));
      for (const auto &entry : inner_->subscriptions)
        if (entry.second.connected)
          connected.push_back(entry.first);
    }
    std::vector<Computed<std::vector<T>>> readers;
    {
      std::lock_guard<std::mutex> lock(inner_->reader_mutex);
      for (const auto &id : connected) {
        auto found = inner_->readers.find(id);
        if (found != inner_->readers.end())
          readers.push_back(found->second);
      }
    }
    queue_detail::batch(ctx, [&](Context &graph) {
      for (const auto &reader : readers)
        reader.clear(graph);
    });
    return offset;
  }

  template <typename Cx>
  std::vector<T> read_stream(Cx &ctx, const std::string &subscriber_id) {
    std::optional<Computed<std::vector<T>>> reader;
    {
      std::lock_guard<std::mutex> lock(inner_->reader_mutex);
      auto found = inner_->readers.find(subscriber_id);
      if (found != inner_->readers.end())
        reader = found->second;
    }
    return reader ? queue_detail::read(ctx, *reader) : std::vector<T>{};
  }

  template <typename Cx>
  std::optional<T> read(Cx &ctx, const std::string &subscriber_id) {
    auto stream = read_stream(ctx, subscriber_id);
    if (stream.empty())
      return std::nullopt;
    return stream.front();
  }

  size_t advance(OwnerContext &ctx, const std::string &subscriber_id,
                 size_t count = 1) {
    size_t cursor;
    bool invalidate = false;
    {
      std::lock_guard<std::mutex> lock(inner_->core_mutex);
      auto found = inner_->subscriptions.find(subscriber_id);
      if (found == inner_->subscriptions.end())
        throw std::out_of_range("invalid TopicCell cursor advance");
      if (!found->second.connected ||
          found->second.cursor == tail_offset_locked())
        return found->second.cursor;
      if (count > tail_offset_locked() - found->second.cursor)
        throw std::out_of_range("invalid TopicCell cursor advance");
      if (count != 0) {
        found->second.cursor += count;
        invalidate = true;
      }
      cursor = found->second.cursor;
    }
    if (invalidate) {
      std::optional<Computed<std::vector<T>>> reader;
      {
        std::lock_guard<std::mutex> lock(inner_->reader_mutex);
        auto found = inner_->readers.find(subscriber_id);
        if (found != inner_->readers.end())
          reader = found->second;
      }
      if (reader)
        invalidate_one(ctx, *reader);
    }
    return cursor;
  }

  /// Drop only the prefix below every durable cursor. No reader invalidation.
  size_t gc() {
    std::lock_guard<std::mutex> lock(inner_->core_mutex);
    size_t frontier = tail_offset_locked();
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

  size_t base_offset() const {
    std::lock_guard<std::mutex> lock(inner_->core_mutex);
    return inner_->base_offset;
  }
  size_t tail_offset() const {
    std::lock_guard<std::mutex> lock(inner_->core_mutex);
    return tail_offset_locked();
  }
  std::vector<T> elements() const {
    std::lock_guard<std::mutex> lock(inner_->core_mutex);
    return std::vector<T>(inner_->elements.begin(), inner_->elements.end());
  }

  std::optional<TopicSubscriptionSnapshot>
  subscription(const std::string &subscriber_id) const {
    std::lock_guard<std::mutex> lock(inner_->core_mutex);
    auto found = inner_->subscriptions.find(subscriber_id);
    if (found == inner_->subscriptions.end())
      return std::nullopt;
    return TopicSubscriptionSnapshot{subscriber_id, found->second.cursor,
                                     found->second.durability,
                                     found->second.connected};
  }

  Computed<std::vector<T>> reader_handle(OwnerContext &ctx,
                                         const std::string &subscriber_id) {
    return ensure_reader(ctx, subscriber_id);
  }

  TopicSnapshot<T> snapshot() const {
    std::lock_guard<std::mutex> lock(inner_->core_mutex);
    TopicSnapshot<T> result;
    result.base_offset = inner_->base_offset;
    result.elements.assign(inner_->elements.begin(), inner_->elements.end());
    for (const auto &entry : inner_->subscriptions) {
      result.subscriptions.push_back(TopicSubscriptionSnapshot{
          entry.first, entry.second.cursor, entry.second.durability,
          entry.second.connected});
    }
    return result;
  }

protected:
  std::shared_ptr<queue_detail::TopicCellInner<T>> inner_;

  size_t tail_offset_locked() const {
    return inner_->base_offset + inner_->elements.size();
  }

  std::vector<T> read_suffix_locked(const std::string &subscriber_id) const {
    auto found = inner_->subscriptions.find(subscriber_id);
    if (found == inner_->subscriptions.end() || !found->second.connected)
      return {};
    const size_t start = found->second.cursor - inner_->base_offset;
    auto begin = inner_->elements.begin();
    std::advance(begin,
                 static_cast<typename std::deque<T>::difference_type>(start));
    return std::vector<T>(begin, inner_->elements.end());
  }

  Computed<std::vector<T>> ensure_reader(OwnerContext &ctx,
                                         const std::string &subscriber_id) {
    {
      std::lock_guard<std::mutex> lock(inner_->reader_mutex);
      auto found = inner_->readers.find(subscriber_id);
      if (found != inner_->readers.end())
        return found->second;
    }
    const auto inner = inner_;
    const auto id = subscriber_id;
    auto reader = queue_detail::graph(ctx).template computed<std::vector<T>>(
        [inner, id](Compute &) {
          std::lock_guard<std::mutex> lock(inner->core_mutex);
          auto found = inner->subscriptions.find(id);
          if (found == inner->subscriptions.end() || !found->second.connected)
            return std::vector<T>{};
          const size_t start = found->second.cursor - inner->base_offset;
          auto begin = inner->elements.begin();
          std::advance(
              begin,
              static_cast<typename std::deque<T>::difference_type>(start));
          return std::vector<T>(begin, inner->elements.end());
        });
    std::lock_guard<std::mutex> lock(inner_->reader_mutex);
    auto inserted = inner_->readers.emplace(subscriber_id, reader);
    return inserted.first->second;
  }

  void invalidate_one(OwnerContext &ctx,
                      const Computed<std::vector<T>> &reader) {
    queue_detail::batch(ctx, [&](Context &graph) { reader.clear(graph); });
  }
};

template <typename T> class TopicCell : public BasicTopicCell<Context, T> {
  using Base = BasicTopicCell<Context, T>;

public:
  explicit TopicCell(Context &ctx) : Base(ctx) {}
  TopicCell(Context &ctx, const TopicSnapshot<T> &snapshot)
      : Base(ctx, snapshot) {}
};

template <typename T>
class ThreadSafeTopicCell : public BasicTopicCell<ThreadSafeContext, T> {
  using Base = BasicTopicCell<ThreadSafeContext, T>;

public:
  explicit ThreadSafeTopicCell(ThreadSafeContext &ctx) : Base(ctx) {}
  ThreadSafeTopicCell(ThreadSafeContext &ctx, const TopicSnapshot<T> &snapshot)
      : Base(ctx, snapshot) {}
};

template <typename T>
class AsyncTopicCell : public BasicTopicCell<AsyncContext, T> {
  using Base = BasicTopicCell<AsyncContext, T>;

public:
  explicit AsyncTopicCell(AsyncContext &ctx) : Base(ctx) {}
  AsyncTopicCell(AsyncContext &ctx, const TopicSnapshot<T> &snapshot)
      : Base(ctx, snapshot) {}
};

} // namespace lazily

#endif // LAZILY_QUEUE_HPP
