#ifndef LAZILY_ASYNC_CONTEXT_HPP
#define LAZILY_ASYNC_CONTEXT_HPP

#include <lazily/context.hpp>
#include <lazily/types.hpp>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace lazily {

enum class AsyncSlotState { Empty, Computing, Resolved, Error };

template <typename T>
struct AsyncSlotNode {
  std::function<T()> compute;
  std::optional<T> value;
  std::optional<std::string> error;
  AsyncSlotState state = AsyncSlotState::Empty;
  int revision = 0;
  std::function<bool(const T&, const T&)> equals;
};

template <typename T>
struct AsyncSlotHandle {
  uint64_t id;
  std::shared_ptr<AsyncSlotNode<T>> node;

  AsyncSlotState state() const { return node->state; }
  int revision() const { return node->revision; }

  std::optional<T> get() {
    if (node->state == AsyncSlotState::Resolved && node->value) {
      return node->value;
    }
    return std::nullopt;
  }

  std::future<T> get_async() {
    return std::async(std::launch::async, [this]() -> T {
      node->state = AsyncSlotState::Computing;
      node->revision++;
      try {
        T result = node->compute();
        if (node->equals && node->value && node->equals(*node->value, result)) {
          // Memo equality guard — suppress
        } else {
          node->value = result;
        }
        node->state = AsyncSlotState::Resolved;
        node->error.reset();
        return *node->value;
      } catch (const std::exception& e) {
        node->state = AsyncSlotState::Error;
        node->error = e.what();
        throw;
      }
    });
  }
};

template <typename T>
struct AsyncCellHandle {
  Context* ctx;
  CellHandle<T> cell;
  AsyncCellHandle(Context& c, T value) : ctx(&c), cell(c.cell(std::move(value))) {}
  T peek() { return ctx->get_cell(cell); }
  T get() { return ctx->get_cell(cell); }
  void set(T value) { ctx->set_cell(cell, std::move(value)); }
};

struct AsyncEffectHandle {
  std::function<void()> cleanup_fn;
  std::atomic<bool> disposed{false};
  std::atomic<bool> running{false};
  std::atomic<bool> rerun_scheduled{false};

  void dispose() {
    disposed.store(true);
    if (cleanup_fn) cleanup_fn();
  }
};

class AsyncContext {
 public:
  using EffectBody = std::function<std::function<void()>()>;

  AsyncContext() = default;
  ~AsyncContext() { dispose(); }

  AsyncContext(const AsyncContext&) = delete;
  AsyncContext& operator=(const AsyncContext&) = delete;

  template <typename T>
  AsyncCellHandle<T> cell(T value) {
    return AsyncCellHandle<T>(ctx_, std::move(value));
  }

  template <typename T>
  AsyncSlotHandle<T> slot(std::function<T()> compute) {
    auto id = next_id_++;
    auto node = std::make_shared<AsyncSlotNode<T>>();
    node->compute = std::move(compute);
    return AsyncSlotHandle<T>{id, node};
  }

  template <typename T>
  AsyncSlotHandle<T> memo(std::function<T()> compute,
                          std::function<bool(const T&, const T&)> eq) {
    auto id = next_id_++;
    auto node = std::make_shared<AsyncSlotNode<T>>();
    node->compute = std::move(compute);
    node->equals = std::move(eq);
    return AsyncSlotHandle<T>{id, node};
  }

  std::shared_ptr<AsyncEffectHandle> effect(EffectBody body) {
    auto handle = std::make_shared<AsyncEffectHandle>();
    run_effect(handle, body);
    return handle;
  }

  void dispose() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& eff : effects_) {
      eff->dispose();
    }
    effects_.clear();
  }

  Context& context() { return ctx_; }

  void batch(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    ctx_.batch([&](Context&) { fn(); });
  }

 private:
  Context ctx_;
  std::mutex mutex_;
  std::atomic<uint64_t> next_id_{0};
  std::vector<std::shared_ptr<AsyncEffectHandle>> effects_;

  void run_effect(std::shared_ptr<AsyncEffectHandle> handle, EffectBody body) {
    if (handle->disposed.load()) return;
    if (handle->running.exchange(true)) {
      handle->rerun_scheduled.store(true);
      return;
    }
    std::thread([this, handle, body]() {
      if (handle->cleanup_fn) {
        handle->cleanup_fn();
        handle->cleanup_fn = nullptr;
      }
      auto cleanup = body();
      handle->cleanup_fn = cleanup;
      handle->running.store(false);
      if (handle->rerun_scheduled.exchange(false) && !handle->disposed.load()) {
        run_effect(handle, body);
      }
    }).detach();
  }
};

}  // namespace lazily

#endif  // LAZILY_ASYNC_CONTEXT_HPP
