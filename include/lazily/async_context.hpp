#ifndef LAZILY_ASYNC_CONTEXT_HPP
#define LAZILY_ASYNC_CONTEXT_HPP

#include <lazily/context.hpp>
#include <lazily/cell.hpp>
#include <lazily/rc_ptr.hpp>
#include <lazily/small_fn.hpp>
#include <lazily/types.hpp>

#include <atomic>
#include <condition_variable>
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

// AsyncContext stores its closures and nodes on the library's own
// SmallFn/RcPtr primitives (#lzcppasyncmodernize), mirroring how the sync
// Context already does this (context.hpp:118-126):
//   - compute/equals/cleanup closures → SmallFn (inline storage for typical
//     small lambdas; no per-slot std::function heap alloc);
//   - AsyncSlotNode<T> / AsyncEffectHandle → RcBox<T> held by RcPtr<RcBox<T>>
//     (intrusive non-atomic refcount — same model as Context's ComputeFnPtr
//     / EffectFnPtr). AsyncContext is single-owner; the refcount is mutated
//     only on the owner thread while the async compute thread accesses the
//     node through a stable raw pointer captured by value.
template <typename T>
struct AsyncSlotNode {
  SmallFn<T()> compute;
  std::optional<T> value;
  std::optional<std::string> error;
  AsyncSlotState state = AsyncSlotState::Empty;
  int revision = 0;
  SmallFn<bool(const T&, const T&)> equals;
};

template <typename T>
struct AsyncSlotHandle {
  using NodePtr = RcPtr<RcBox<AsyncSlotNode<T>>>;

  uint64_t id;
  NodePtr node;

  AsyncSlotState state() const { return node->value.state; }
  int revision() const { return node->value.revision; }

  std::optional<T> get() {
    if (node->value.state == AsyncSlotState::Resolved && node->value.value) {
      return node->value.value;
    }
    return std::nullopt;
  }

  std::future<T> get_async() {
    return std::async(std::launch::async, [this]() -> T {
      node->value.state = AsyncSlotState::Computing;
      node->value.revision++;
      try {
        T result = node->value.compute();
        if (node->value.equals && node->value.value &&
            node->value.equals(*node->value.value, result)) {
          // Memo equality guard — suppress
        } else {
          node->value.value = result;
        }
        node->value.state = AsyncSlotState::Resolved;
        node->value.error.reset();
        return *node->value.value;
      } catch (const std::exception& e) {
        node->value.state = AsyncSlotState::Error;
        node->value.error = e.what();
        throw;
      }
    });
  }
};

template <typename T>
struct AsyncCellHandle {
  Context* ctx;
  Source<T> cell;  // #lzcellkernel: was CellHandle<T>
  AsyncCellHandle(Context& c, T value) : ctx(&c), cell(c.source(std::move(value))) {}
  T peek() { return ctx->get(cell); }
  T get() { return ctx->get(cell); }
  void set(T value) { ctx->set(cell, std::move(value)); }
};

struct AsyncEffectHandle {
  SmallFn<void()> cleanup_fn;
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
  // Effect body returns a cleanup closure. Both layers use SmallFn so the
  // caller's body lambda is stored inline when it fits (no std::function
  // heap alloc). The cleanup uses the same 32-byte inline buffer as the
  // sync Context's CleanupFn.
  using EffectCleanup = SmallFn<void(), 32>;
  using EffectBody = SmallFn<EffectCleanup()>;
  using EffectHandlePtr = RcPtr<RcBox<AsyncEffectHandle>>;

  AsyncContext() = default;
  ~AsyncContext() { dispose(); }

  AsyncContext(const AsyncContext&) = delete;
  AsyncContext& operator=(const AsyncContext&) = delete;

  template <typename T>
  AsyncCellHandle<T> cell(T value) {
    return AsyncCellHandle<T>(ctx_, std::move(value));
  }

  // Accept any callable (std::function, lambdas, function pointers) via
  // SmallFn's converting constructor; T must be explicit.
  template <typename T, typename F>
  AsyncSlotHandle<T> slot(F&& compute) {
    auto id = next_id_++;
    auto* raw = new RcBox<AsyncSlotNode<T>>();
    raw->value.compute = std::forward<F>(compute);
    typename AsyncSlotHandle<T>::NodePtr node(
        raw, typename AsyncSlotHandle<T>::NodePtr::adopt_t{});
    return AsyncSlotHandle<T>{id, std::move(node)};
  }

  template <typename T, typename F, typename E>
  AsyncSlotHandle<T> memo(F&& compute, E&& eq) {
    auto id = next_id_++;
    auto* raw = new RcBox<AsyncSlotNode<T>>();
    raw->value.compute = std::forward<F>(compute);
    raw->value.equals = std::forward<E>(eq);
    typename AsyncSlotHandle<T>::NodePtr node(
        raw, typename AsyncSlotHandle<T>::NodePtr::adopt_t{});
    return AsyncSlotHandle<T>{id, std::move(node)};
  }

  EffectHandlePtr effect(EffectBody body) {
    auto* raw = new RcBox<AsyncEffectHandle>();
    EffectHandlePtr handle(raw, typename EffectHandlePtr::adopt_t{});
    {
      std::lock_guard<std::mutex> lock(mutex_);
      effects_.push_back(handle);
    }
    run_effect(handle, std::move(body));
    return handle;
  }

  void dispose() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& eff : effects_) {
      eff->value.dispose();
    }
    effects_.clear();
  }

  Context& context() { return ctx_; }

  template <typename F>
  void batch(F&& fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    ctx_.batch([&](Context&) { fn(); });
  }

 private:
  Context ctx_;
  std::mutex mutex_;
  std::atomic<uint64_t> next_id_{0};
  std::vector<EffectHandlePtr> effects_;

  void run_effect(EffectHandlePtr handle, EffectBody body) {
    if (handle->value.disposed.load()) return;
    if (handle->value.running.exchange(true)) {
      handle->value.rerun_scheduled.store(true);
      return;
    }
    // Loop replaces the original recursive self-call: SmallFn is move-only,
    // so the body closure is moved once into the worker thread and reused
    // across re-runs. The loop re-enters whenever `rerun_scheduled` was set
    // while the previous run was in flight and the handle has not been
    // disposed. `running` toggles per iteration so a future external caller
    // can observe the in-flight state.
    std::thread([handle, body = std::move(body)]() mutable {
      do {
        if (handle->value.cleanup_fn) {
          handle->value.cleanup_fn();
          handle->value.cleanup_fn = EffectCleanup{};
        }
        auto cleanup = body();
        handle->value.cleanup_fn = std::move(cleanup);
        handle->value.running.store(false);
      } while (!handle->value.disposed.load() &&
               handle->value.rerun_scheduled.exchange(false) &&
               handle->value.running.exchange(true));
    }).detach();
  }
};

}  // namespace lazily

#endif  // LAZILY_ASYNC_CONTEXT_HPP
