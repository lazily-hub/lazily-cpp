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

// Public projection of the async computed state machine. The formal model keeps
// the storage-oriented `AsyncSlotState` theorem/module vocabulary.
enum class AsyncComputedState { Empty, Computing, Resolved, Error };

// AsyncContext stores its closures and nodes on the library's own
// SmallFn/RcPtr primitives (#lzcppasyncmodernize), mirroring how the sync
// Context already does this (context.hpp:118-126):
//   - compute/equals/cleanup closures → SmallFn (inline storage for typical
//     small lambdas; no per-slot std::function heap alloc);
//   - AsyncComputedNode<T> / AsyncEffectHandle → RcBox<T> held by RcPtr<RcBox<T>>
//     (intrusive non-atomic refcount — same model as Context's ComputeFnPtr
//     / EffectFnPtr). AsyncContext is single-owner; the refcount is mutated
//     only on the owner thread while the async compute thread accesses the
//     node through a stable raw pointer captured by value.
template <typename T>
struct AsyncComputedNode {
  SmallFn<T()> compute;
  std::optional<T> value;
  std::optional<std::string> error;
AsyncComputedState state = AsyncComputedState::Empty;
  int revision = 0;
  SmallFn<bool(const T&, const T&)> equals;
};

template <typename T>
struct AsyncComputed {
using NodePtr = RcPtr<RcBox<AsyncComputedNode<T>>>;

  uint64_t id;
  NodePtr node;

AsyncComputedState state() const { return node->value.state; }
  int revision() const { return node->value.revision; }

  std::optional<T> get() {
if (node->value.state == AsyncComputedState::Resolved && node->value.value) {
      return node->value.value;
    }
    return std::nullopt;
  }

  // Teardown on removal from a keyed collection. An async slot node is not
  // registered in the sync `Context` graph, so there are no graph dependents to
  // clear; what must not survive is the *cached value*. Without this a removed
  // entry keeps serving its last resolved value to any holder of the handle —
  // the defect lazily-cs's `AsyncComputedMap.Remove` still has.
  void clear_dependents() {
    node->value.value.reset();
    node->value.error.reset();
node->value.state = AsyncComputedState::Empty;
  }

  std::future<T> get_async() {
    return std::async(std::launch::async, [this]() -> T {
node->value.state = AsyncComputedState::Computing;
      node->value.revision++;
      try {
        T result = node->value.compute();
        if (node->value.equals && node->value.value &&
            node->value.equals(*node->value.value, result)) {
          // Memo equality guard — suppress
        } else {
          node->value.value = result;
        }
node->value.state = AsyncComputedState::Resolved;
        node->value.error.reset();
        return *node->value.value;
      } catch (const std::exception& e) {
node->value.state = AsyncComputedState::Error;
        node->value.error = e.what();
        throw;
      }
    });
  }
};

template <typename T>
struct AsyncSource {
Context* ctx;
Source<T> cell;  // #lzcellkernel: was CellHandle<T>
AsyncSource(Context& c, T value) : ctx(&c), cell(c.source(std::move(value))) {}
  T peek() { return ctx->get(cell); }
  T get() { return ctx->get(cell); }
  void set(T value) { ctx->set(cell, std::move(value)); }
  // An async cell IS a `Source` on the underlying graph, so its teardown is the
  // ordinary node teardown.
  void clear_dependents() { cell.clear_dependents(*ctx); }
};

// Published v1 compatibility spellings. New implementation code uses the
// canonical async value kinds above.
using AsyncSlotState [[deprecated("use AsyncComputedState")]] = AsyncComputedState;

template <typename T>
using AsyncCellHandle [[deprecated("use AsyncSource")]] = AsyncSource<T>;

template <typename T>
using AsyncSlotHandle [[deprecated("use AsyncComputed")]] = AsyncComputed<T>;

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
AsyncSource<T> source(T value) {
return AsyncSource<T>(ctx_, std::move(value));
}

// Accept any callable (std::function, lambdas, function pointers) via
// SmallFn's converting constructor; T must be explicit.
template <typename T, typename F>
AsyncComputed<T> computed(F&& compute) {
auto id = next_id_++;
auto* raw = new RcBox<AsyncComputedNode<T>>();
raw->value.compute = std::forward<F>(compute);
typename AsyncComputed<T>::NodePtr node(
raw, typename AsyncComputed<T>::NodePtr::adopt_t{});
return AsyncComputed<T>{id, std::move(node)};
}

template <typename T, typename F, typename E>
AsyncComputed<T> computed(F&& compute, E&& eq) {
auto id = next_id_++;
auto* raw = new RcBox<AsyncComputedNode<T>>();
raw->value.compute = std::forward<F>(compute);
raw->value.equals = std::forward<E>(eq);
typename AsyncComputed<T>::NodePtr node(
raw, typename AsyncComputed<T>::NodePtr::adopt_t{});
return AsyncComputed<T>{id, std::move(node)};
}

template <typename T>
[[deprecated("use source")]]
AsyncSource<T> cell(T value) {
return source<T>(std::move(value));
}

template <typename T, typename F>
[[deprecated("use computed")]]
AsyncComputed<T> slot(F&& compute) {
return computed<T>(std::forward<F>(compute));
}

template <typename T, typename F, typename E>
[[deprecated("memo is a guarded computed; use computed(compute, equals)")]]
AsyncComputed<T> memo(F&& compute, E&& eq) {
return computed<T>(std::forward<F>(compute), std::forward<E>(eq));
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
