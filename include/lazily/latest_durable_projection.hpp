#ifndef LAZILY_LATEST_DURABLE_PROJECTION_HPP
#define LAZILY_LATEST_DURABLE_PROJECTION_HPP

#include <lazily/async_context.hpp>
#include <lazily/context.hpp>
#include <lazily/latest_durable_projection_core.hpp>
#include <lazily/thread_safe.hpp>

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace lazily {
namespace latest_durable_detail {

inline Context& graph(Context& context) { return context; }
inline Context& graph(ThreadSafeContext& context) { return context.context(); }
inline Context& graph(AsyncContext& context) { return context.context(); }

template <typename F> void batch(Context& context, F&& fn) {
  context.batch([&](Context& graph) { fn(graph); });
}
template <typename F> void batch(ThreadSafeContext& context, F&& fn) {
  context.batch([&](Context& graph) { fn(graph); });
}
template <typename F> void batch(AsyncContext& context, F&& fn) {
  context.context().batch([&](Context& graph) { fn(graph); });
}

template <typename Cx, typename T> T read(Cx& context, const Computed<T>& handle) {
  return context.get(handle);
}
template <typename T> T read(AsyncContext& context, const Computed<T>& handle) {
  return context.context().get(handle);
}

template <typename K, typename T> struct ProjectionInner {
  explicit ProjectionInner(std::uint64_t generation) : core(generation) {}
  mutable std::mutex core_mutex;
  mutable std::mutex reader_mutex;
  LatestDurableProjectionCore<K, T> core;
  std::unordered_map<K, Computed<LatestDurableEntry<K, T>>> readers;
  Computed<std::uint64_t> generation;
};

} // namespace latest_durable_detail

/// Flavor-neutral reactive shell over `LatestDurableProjectionCore`.
///
/// The shell exposes memoized entry/generation projections. Sink I/O deliberately
/// remains application-owned and should share the caller's per-key mutation lane.
template <typename OwnerContext, typename K, typename T> class BasicLatestDurableProjection {
public:
  BasicLatestDurableProjection(OwnerContext& context, std::uint64_t initial_generation)
      : inner_(std::make_shared<latest_durable_detail::ProjectionInner<K, T>>(initial_generation)) {
    Context& graph = latest_durable_detail::graph(context);
    const auto inner = inner_;
    inner_->generation = graph.template computed<std::uint64_t>([inner](Compute&) {
      std::lock_guard<std::mutex> lock(inner->core_mutex);
      return inner->core.generation();
    });
  }

  LatestDurableUpsertResult upsert_desired(OwnerContext& context, K key, std::uint64_t epoch,
                                           T value) {
    return mutate(context, key,
                  [&] { return inner_->core.upsert_desired(key, epoch, std::move(value)); });
  }

  LatestDurableClaimResult<K, T> claim(OwnerContext& context, const K& key,
                                       std::uint64_t generation) {
    return mutate(context, key, [&] { return inner_->core.claim(key, generation); });
  }

  LatestDurableAckResult ack_applied(OwnerContext& context, const K& key, std::uint64_t generation,
                                     std::uint64_t epoch) {
    return mutate(context, key, [&] { return inner_->core.ack_applied(key, generation, epoch); });
  }

  LatestDurableFailureResult fail_retryable(OwnerContext& context, const K& key,
                                            std::uint64_t generation, std::uint64_t epoch) {
    return mutate(context, key,
                  [&] { return inner_->core.fail_retryable(key, generation, epoch); });
  }

  LatestDurableReconnectResult reconnect(OwnerContext& context, std::uint64_t new_generation) {
    std::vector<K> changed;
    LatestDurableReconnectResult result;
    {
      std::lock_guard<std::mutex> lock(inner_->core_mutex);
      const auto keys = inner_->core.known_keys();
      std::vector<LatestDurableEntry<K, T>> before;
      before.reserve(keys.size());
      for (const auto& key : keys)
        before.push_back(inner_->core.snapshot(key));
      result = inner_->core.reconnect(new_generation);
      for (std::size_t index = 0; index < keys.size(); ++index)
        if (before[index] != inner_->core.snapshot(keys[index])) changed.push_back(keys[index]);
    }

    std::vector<SlotId> roots;
    {
      std::lock_guard<std::mutex> lock(inner_->reader_mutex);
      for (const auto& key : changed) {
        const auto it = inner_->readers.find(key);
        if (it != inner_->readers.end()) roots.push_back(it->second.id());
      }
    }
    if (result.kind == LatestDurableReconnectKind::Advanced)
      roots.push_back(inner_->generation.id());
    invalidate(context, roots);
    return result;
  }

  Computed<LatestDurableEntry<K, T>> entry_handle(OwnerContext& context, const K& key) {
    {
      std::lock_guard<std::mutex> lock(inner_->reader_mutex);
      const auto it = inner_->readers.find(key);
      if (it != inner_->readers.end()) return it->second;
    }
    Context& graph = latest_durable_detail::graph(context);
    const auto inner = inner_;
    auto reader = graph.template computed<LatestDurableEntry<K, T>>([inner, key](Compute&) {
      std::lock_guard<std::mutex> lock(inner->core_mutex);
      return inner->core.snapshot(key);
    });
    std::lock_guard<std::mutex> lock(inner_->reader_mutex);
    return inner_->readers.emplace(key, reader).first->second;
  }

  Computed<std::uint64_t> generation_handle() const { return inner_->generation; }

  LatestDurableEntry<K, T> entry(OwnerContext& context, const K& key) {
    return latest_durable_detail::read(context, entry_handle(context, key));
  }

  std::uint64_t generation(OwnerContext& context) {
    return latest_durable_detail::read(context, inner_->generation);
  }

  LatestDurableEntry<K, T> snapshot(const K& key) const {
    std::lock_guard<std::mutex> lock(inner_->core_mutex);
    return inner_->core.snapshot(key);
  }

protected:
  std::shared_ptr<latest_durable_detail::ProjectionInner<K, T>> inner_;

private:
  template <typename F>
  auto mutate(OwnerContext& context, const K& key, F&& operation) -> decltype(operation()) {
    bool changed = false;
    decltype(operation()) result;
    {
      std::lock_guard<std::mutex> lock(inner_->core_mutex);
      const auto before = inner_->core.snapshot(key);
      result = operation();
      changed = before != inner_->core.snapshot(key);
    }
    if (changed) {
      std::vector<SlotId> roots;
      {
        std::lock_guard<std::mutex> lock(inner_->reader_mutex);
        const auto it = inner_->readers.find(key);
        if (it != inner_->readers.end()) roots.push_back(it->second.id());
      }
      invalidate(context, roots);
    }
    return result;
  }

  static void invalidate(OwnerContext& context, const std::vector<SlotId>& roots) {
    if (roots.empty()) return;
    latest_durable_detail::batch(context, [&](Context& graph) {
      for (const SlotId id : roots)
        graph.clear_slot(id);
    });
  }
};

/// Single-threaded projection shell.
template <typename K, typename T>
class LatestDurableProjection : public BasicLatestDurableProjection<Context, K, T> {
  using Base = BasicLatestDurableProjection<Context, K, T>;

public:
  LatestDurableProjection(Context& context, std::uint64_t initial_generation)
      : Base(context, initial_generation) {}
};

/// Lock-backed projection shell.
template <typename K, typename T>
class ThreadSafeLatestDurableProjection
    : public BasicLatestDurableProjection<ThreadSafeContext, K, T> {
  using Base = BasicLatestDurableProjection<ThreadSafeContext, K, T>;

public:
  ThreadSafeLatestDurableProjection(ThreadSafeContext& context, std::uint64_t initial_generation)
      : Base(context, initial_generation) {}
};

/// Async-context projection shell. Admission operations remain synchronous.
template <typename K, typename T>
class AsyncLatestDurableProjection : public BasicLatestDurableProjection<AsyncContext, K, T> {
  using Base = BasicLatestDurableProjection<AsyncContext, K, T>;

public:
  AsyncLatestDurableProjection(AsyncContext& context, std::uint64_t initial_generation)
      : Base(context, initial_generation) {}
};

} // namespace lazily

#endif // LAZILY_LATEST_DURABLE_PROJECTION_HPP
