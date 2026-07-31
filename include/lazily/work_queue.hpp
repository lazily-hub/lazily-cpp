#ifndef LAZILY_WORK_QUEUE_HPP
#define LAZILY_WORK_QUEUE_HPP

#include <lazily/async_context.hpp>
#include <lazily/cell.hpp>
#include <lazily/context.hpp>
#include <lazily/queue.hpp>
#include <lazily/thread_safe.hpp>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lazily {

template <typename T> struct WorkQueueItem {
  uint64_t item_id;
  T value;
  uint64_t attempts;
};

template <typename T> struct WorkQueueDelivery {
  uint64_t delivery_id;
  uint64_t item_id;
  T value;
  std::string worker;
  uint64_t attempt;
  uint64_t deadline;
};

enum class WorkQueueDeadLetterReason { Nack, Expired };

template <typename T> struct WorkQueueDeadLetter {
  uint64_t item_id;
  T value;
  uint64_t attempts;
  WorkQueueDeadLetterReason reason;
};

struct WorkQueueReaderHandles {
  Computed<size_t> pending_len;
  Computed<bool> is_empty;
  Computed<size_t> in_flight_len;
  Computed<size_t> dead_letter_len;
};

template <typename T> struct WorkQueueCellInner {
  mutable std::mutex mutex;
  std::deque<WorkQueueItem<T>> pending;
  std::unordered_map<uint64_t, WorkQueueDelivery<T>> in_flight;
  std::vector<WorkQueueDeadLetter<T>> dead_letters;
  uint64_t next_item_id = 0;
  uint64_t next_delivery_id = 0;
  WorkQueueReaderHandles readers;
};

/// Process-local competing-consumer queue with leased exclusive claims.
///
/// Logical item ids survive retries while every claim gets a fresh delivery
/// id. Nack and expired deliveries requeue at the tail until `max_deliveries`
/// is reached, then move to the dead-letter list. A lease remains live at its
/// deadline and expires only when `deadline < now`.
///
/// This class is a local serialization point. Distributed/HA deployments must
/// put a consensus-backed leader or adapter in front of it.
template <typename OwnerContext, typename T> class BasicWorkQueueCell {
public:
  BasicWorkQueueCell(OwnerContext& ctx, uint64_t visibility_timeout, uint64_t max_deliveries)
      : inner_(std::make_shared<WorkQueueCellInner<T>>()), visibility_timeout_(visibility_timeout),
        max_deliveries_(max_deliveries) {
    if (visibility_timeout == 0) throw std::invalid_argument("visibility_timeout must be positive");
    if (max_deliveries == 0) throw std::invalid_argument("max_deliveries must be at least one");
    const auto inner = inner_;
    auto& graph = queue_detail::graph(ctx);
    inner_->readers.pending_len = graph.template computed<size_t>([inner](Compute&) {
      std::lock_guard<std::mutex> lock(inner->mutex);
      return inner->pending.size();
    });
    inner_->readers.is_empty = graph.template computed<bool>([inner](Compute&) {
      std::lock_guard<std::mutex> lock(inner->mutex);
      return inner->pending.empty();
    });
    inner_->readers.in_flight_len = graph.template computed<size_t>([inner](Compute&) {
      std::lock_guard<std::mutex> lock(inner->mutex);
      return inner->in_flight.size();
    });
    inner_->readers.dead_letter_len = graph.template computed<size_t>([inner](Compute&) {
      std::lock_guard<std::mutex> lock(inner->mutex);
      return inner->dead_letters.size();
    });
  }

  uint64_t push(OwnerContext& ctx, T value) {
    Counts before;
    Counts after;
    uint64_t item_id;
    {
      std::lock_guard<std::mutex> lock(inner_->mutex);
      before = counts_locked();
      if (inner_->next_item_id == std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("work queue item ids exhausted");
      item_id = inner_->next_item_id++;
      inner_->pending.push_back({item_id, std::move(value), 0});
      after = counts_locked();
    }
    invalidate(ctx, before, after);
    return item_id;
  }

  std::optional<WorkQueueDelivery<T>> claim(OwnerContext& ctx, std::string worker, uint64_t now) {
    Counts before;
    Counts after;
    WorkQueueDelivery<T> delivery;
    {
      std::lock_guard<std::mutex> lock(inner_->mutex);
      if (inner_->pending.empty()) return std::nullopt;
      if (inner_->next_delivery_id == std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("work queue delivery ids exhausted");
      if (now > std::numeric_limits<uint64_t>::max() - visibility_timeout_)
        throw std::overflow_error("work queue deadline overflow");
      before = counts_locked();
      WorkQueueItem<T> item = std::move(inner_->pending.front());
      inner_->pending.pop_front();
      delivery = WorkQueueDelivery<T>{inner_->next_delivery_id++, item.item_id,
                                      std::move(item.value),      std::move(worker),
                                      item.attempts + 1,          now + visibility_timeout_};
      inner_->in_flight.emplace(delivery.delivery_id, delivery);
      after = counts_locked();
    }
    invalidate(ctx, before, after);
    return delivery;
  }

  bool ack(OwnerContext& ctx, const std::string& worker, uint64_t delivery_id) {
    Counts before;
    Counts after;
    {
      std::lock_guard<std::mutex> lock(inner_->mutex);
      const auto found = inner_->in_flight.find(delivery_id);
      if (found == inner_->in_flight.end() || found->second.worker != worker) return false;
      before = counts_locked();
      inner_->in_flight.erase(found);
      after = counts_locked();
    }
    invalidate(ctx, before, after);
    return true;
  }

  bool nack(OwnerContext& ctx, const std::string& worker, uint64_t delivery_id) {
    Counts before;
    Counts after;
    {
      std::lock_guard<std::mutex> lock(inner_->mutex);
      const auto found = inner_->in_flight.find(delivery_id);
      if (found == inner_->in_flight.end() || found->second.worker != worker) return false;
      before = counts_locked();
      WorkQueueDelivery<T> delivery = std::move(found->second);
      inner_->in_flight.erase(found);
      fail_locked(std::move(delivery), WorkQueueDeadLetterReason::Nack);
      after = counts_locked();
    }
    invalidate(ctx, before, after);
    return true;
  }

  size_t reap_expired(OwnerContext& ctx, uint64_t now) {
    std::vector<uint64_t> expired;
    Counts before;
    Counts after;
    {
      std::lock_guard<std::mutex> lock(inner_->mutex);
      for (const auto& entry : inner_->in_flight) {
        if (entry.second.deadline < now) expired.push_back(entry.first);
      }
      if (expired.empty()) return 0;
      std::sort(expired.begin(), expired.end());
      before = counts_locked();
      for (uint64_t delivery_id : expired) {
        auto found = inner_->in_flight.find(delivery_id);
        WorkQueueDelivery<T> delivery = std::move(found->second);
        inner_->in_flight.erase(found);
        fail_locked(std::move(delivery), WorkQueueDeadLetterReason::Expired);
      }
      after = counts_locked();
    }
    invalidate(ctx, before, after);
    return expired.size();
  }

  template <typename Cx> size_t pending_len(Cx& ctx) const {
    return queue_detail::read(ctx, inner_->readers.pending_len);
  }

  template <typename Cx> bool is_empty(Cx& ctx) const {
    return queue_detail::read(ctx, inner_->readers.is_empty);
  }

  template <typename Cx> size_t in_flight_len(Cx& ctx) const {
    return queue_detail::read(ctx, inner_->readers.in_flight_len);
  }

  template <typename Cx> size_t dead_letter_len(Cx& ctx) const {
    return queue_detail::read(ctx, inner_->readers.dead_letter_len);
  }

  WorkQueueReaderHandles reader_handles() const { return inner_->readers; }

  std::vector<WorkQueueItem<T>> pending_items() const {
    std::lock_guard<std::mutex> lock(inner_->mutex);
    return {inner_->pending.begin(), inner_->pending.end()};
  }

  std::vector<WorkQueueDelivery<T>> in_flight_deliveries() const {
    std::lock_guard<std::mutex> lock(inner_->mutex);
    std::vector<WorkQueueDelivery<T>> result;
    result.reserve(inner_->in_flight.size());
    for (const auto& entry : inner_->in_flight)
      result.push_back(entry.second);
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
      return left.delivery_id < right.delivery_id;
    });
    return result;
  }

  std::vector<WorkQueueDeadLetter<T>> dead_letter_items() const {
    std::lock_guard<std::mutex> lock(inner_->mutex);
    return inner_->dead_letters;
  }

private:
  struct Counts {
    size_t pending;
    size_t in_flight;
    size_t dead_letters;
  };

  Counts counts_locked() const {
    return {inner_->pending.size(), inner_->in_flight.size(), inner_->dead_letters.size()};
  }

  void invalidate(OwnerContext& ctx, Counts before, Counts after) {
    queue_detail::batch(ctx, [&](Context& graph) {
      if (before.pending != after.pending) {
        inner_->readers.pending_len.clear(graph);
      }
      if ((before.pending == 0) != (after.pending == 0)) {
        inner_->readers.is_empty.clear(graph);
      }
      if (before.in_flight != after.in_flight) {
        inner_->readers.in_flight_len.clear(graph);
      }
      if (before.dead_letters != after.dead_letters) {
        inner_->readers.dead_letter_len.clear(graph);
      }
    });
  }

  void fail_locked(WorkQueueDelivery<T> delivery, WorkQueueDeadLetterReason reason) {
    if (delivery.attempt >= max_deliveries_) {
      inner_->dead_letters.push_back(
          {delivery.item_id, std::move(delivery.value), delivery.attempt, reason});
    } else {
      inner_->pending.push_back({delivery.item_id, std::move(delivery.value), delivery.attempt});
    }
  }

  std::shared_ptr<WorkQueueCellInner<T>> inner_;
  uint64_t visibility_timeout_;
  uint64_t max_deliveries_;
};

template <typename T> class WorkQueueCell : public BasicWorkQueueCell<Context, T> {
  using Base = BasicWorkQueueCell<Context, T>;

public:
  WorkQueueCell(Context& ctx, uint64_t visibility_timeout, uint64_t max_deliveries)
      : Base(ctx, visibility_timeout, max_deliveries) {}
};

template <typename T>
class ThreadSafeWorkQueueCell : public BasicWorkQueueCell<ThreadSafeContext, T> {
  using Base = BasicWorkQueueCell<ThreadSafeContext, T>;

public:
  ThreadSafeWorkQueueCell(ThreadSafeContext& ctx, uint64_t visibility_timeout,
                          uint64_t max_deliveries)
      : Base(ctx, visibility_timeout, max_deliveries) {}
};

template <typename T> class AsyncWorkQueueCell : public BasicWorkQueueCell<AsyncContext, T> {
  using Base = BasicWorkQueueCell<AsyncContext, T>;

public:
  AsyncWorkQueueCell(AsyncContext& ctx, uint64_t visibility_timeout, uint64_t max_deliveries)
      : Base(ctx, visibility_timeout, max_deliveries) {}
};

} // namespace lazily

#endif // LAZILY_WORK_QUEUE_HPP
