#ifndef LAZILY_WORK_QUEUE_HPP
#define LAZILY_WORK_QUEUE_HPP

#include <lazily/context.hpp>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
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
  CellHandle<uint64_t> pending_len;
  CellHandle<uint64_t> is_empty;
  CellHandle<uint64_t> in_flight_len;
  CellHandle<uint64_t> dead_letter_len;
};

template <typename T> struct WorkQueueCellInner {
  std::deque<WorkQueueItem<T>> pending;
  std::unordered_map<uint64_t, WorkQueueDelivery<T>> in_flight;
  std::vector<WorkQueueDeadLetter<T>> dead_letters;
  uint64_t next_item_id = 0;
  uint64_t next_delivery_id = 0;
  WorkQueueReaderHandles readers;
  uint64_t pending_version = 0;
  uint64_t empty_version = 0;
  uint64_t in_flight_version = 0;
  uint64_t dead_letter_version = 0;
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
template <typename T> class WorkQueueCell {
public:
  WorkQueueCell(Context &ctx, uint64_t visibility_timeout,
                uint64_t max_deliveries)
      : inner_(std::make_shared<WorkQueueCellInner<T>>()),
        visibility_timeout_(visibility_timeout),
        max_deliveries_(max_deliveries) {
    if (visibility_timeout == 0)
      throw std::invalid_argument("visibility_timeout must be positive");
    if (max_deliveries == 0)
      throw std::invalid_argument("max_deliveries must be at least one");
    inner_->readers.pending_len = ctx.cell(uint64_t(0));
    inner_->readers.is_empty = ctx.cell(uint64_t(0));
    inner_->readers.in_flight_len = ctx.cell(uint64_t(0));
    inner_->readers.dead_letter_len = ctx.cell(uint64_t(0));
  }

  uint64_t push(Context &ctx, T value) {
    const Counts before = counts();
    if (inner_->next_item_id == std::numeric_limits<uint64_t>::max())
      throw std::overflow_error("work queue item ids exhausted");
    const uint64_t item_id = inner_->next_item_id++;
    inner_->pending.push_back({item_id, std::move(value), 0});
    invalidate(ctx, before);
    return item_id;
  }

  std::optional<WorkQueueDelivery<T>> claim(Context &ctx, std::string worker,
                                            uint64_t now) {
    if (inner_->pending.empty())
      return std::nullopt;
    if (inner_->next_delivery_id == std::numeric_limits<uint64_t>::max())
      throw std::overflow_error("work queue delivery ids exhausted");
    if (now > std::numeric_limits<uint64_t>::max() - visibility_timeout_)
      throw std::overflow_error("work queue deadline overflow");
    const Counts before = counts();
    WorkQueueItem<T> item = std::move(inner_->pending.front());
    inner_->pending.pop_front();
    WorkQueueDelivery<T> delivery{
        inner_->next_delivery_id++, item.item_id,
        std::move(item.value),      std::move(worker),
        item.attempts + 1,          now + visibility_timeout_};
    inner_->in_flight.emplace(delivery.delivery_id, delivery);
    invalidate(ctx, before);
    return delivery;
  }

  bool ack(Context &ctx, const std::string &worker, uint64_t delivery_id) {
    const auto found = inner_->in_flight.find(delivery_id);
    if (found == inner_->in_flight.end() || found->second.worker != worker)
      return false;
    const Counts before = counts();
    inner_->in_flight.erase(found);
    invalidate(ctx, before);
    return true;
  }

  bool nack(Context &ctx, const std::string &worker, uint64_t delivery_id) {
    const auto found = inner_->in_flight.find(delivery_id);
    if (found == inner_->in_flight.end() || found->second.worker != worker)
      return false;
    const Counts before = counts();
    WorkQueueDelivery<T> delivery = std::move(found->second);
    inner_->in_flight.erase(found);
    fail(std::move(delivery), WorkQueueDeadLetterReason::Nack);
    invalidate(ctx, before);
    return true;
  }

  size_t reap_expired(Context &ctx, uint64_t now) {
    std::vector<uint64_t> expired;
    for (const auto &entry : inner_->in_flight) {
      if (entry.second.deadline < now)
        expired.push_back(entry.first);
    }
    if (expired.empty())
      return 0;
    std::sort(expired.begin(), expired.end());
    const Counts before = counts();
    for (uint64_t delivery_id : expired) {
      auto found = inner_->in_flight.find(delivery_id);
      WorkQueueDelivery<T> delivery = std::move(found->second);
      inner_->in_flight.erase(found);
      fail(std::move(delivery), WorkQueueDeadLetterReason::Expired);
    }
    invalidate(ctx, before);
    return expired.size();
  }

  size_t pending_len(Context &ctx) const {
    (void)ctx.get_cell(inner_->readers.pending_len);
    return inner_->pending.size();
  }

  bool is_empty(Context &ctx) const {
    (void)ctx.get_cell(inner_->readers.is_empty);
    return inner_->pending.empty();
  }

  size_t in_flight_len(Context &ctx) const {
    (void)ctx.get_cell(inner_->readers.in_flight_len);
    return inner_->in_flight.size();
  }

  size_t dead_letter_len(Context &ctx) const {
    (void)ctx.get_cell(inner_->readers.dead_letter_len);
    return inner_->dead_letters.size();
  }

  WorkQueueReaderHandles reader_handles() const { return inner_->readers; }

  std::vector<WorkQueueItem<T>> pending_items() const {
    return {inner_->pending.begin(), inner_->pending.end()};
  }

  std::vector<WorkQueueDelivery<T>> in_flight_deliveries() const {
    std::vector<WorkQueueDelivery<T>> result;
    result.reserve(inner_->in_flight.size());
    for (const auto &entry : inner_->in_flight)
      result.push_back(entry.second);
    std::sort(result.begin(), result.end(),
              [](const auto &left, const auto &right) {
                return left.delivery_id < right.delivery_id;
              });
    return result;
  }

  std::vector<WorkQueueDeadLetter<T>> dead_letter_items() const {
    return inner_->dead_letters;
  }

private:
  struct Counts {
    size_t pending;
    size_t in_flight;
    size_t dead_letters;
  };

  Counts counts() const {
    return {inner_->pending.size(), inner_->in_flight.size(),
            inner_->dead_letters.size()};
  }

  void invalidate(Context &ctx, Counts before) {
    const Counts after = counts();
    ctx.batch([&](Context &batched) {
      if (before.pending != after.pending) {
        batched.set_cell(inner_->readers.pending_len,
                         ++inner_->pending_version);
      }
      if ((before.pending == 0) != (after.pending == 0)) {
        batched.set_cell(inner_->readers.is_empty, ++inner_->empty_version);
      }
      if (before.in_flight != after.in_flight) {
        batched.set_cell(inner_->readers.in_flight_len,
                         ++inner_->in_flight_version);
      }
      if (before.dead_letters != after.dead_letters) {
        batched.set_cell(inner_->readers.dead_letter_len,
                         ++inner_->dead_letter_version);
      }
    });
  }

  void fail(WorkQueueDelivery<T> delivery, WorkQueueDeadLetterReason reason) {
    if (delivery.attempt >= max_deliveries_) {
      inner_->dead_letters.push_back({delivery.item_id,
                                      std::move(delivery.value),
                                      delivery.attempt, reason});
    } else {
      inner_->pending.push_back(
          {delivery.item_id, std::move(delivery.value), delivery.attempt});
    }
  }

  std::shared_ptr<WorkQueueCellInner<T>> inner_;
  uint64_t visibility_timeout_;
  uint64_t max_deliveries_;
};

} // namespace lazily

#endif // LAZILY_WORK_QUEUE_HPP
