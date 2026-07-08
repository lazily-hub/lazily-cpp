#ifndef LAZILY_THREAD_SAFE_HPP
#define LAZILY_THREAD_SAFE_HPP

#include <lazily/context.hpp>

#include <mutex>
#include <thread>

namespace lazily {

class ThreadSafeContext {
 public:
  ThreadSafeContext() : ctx_() {}

  template <typename F>
  auto with_lock(F&& fn) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return fn(ctx_);
  }

  template <typename F>
  auto read(F&& fn) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return fn(ctx_);
  }

  template <typename F>
  auto batch(F&& fn) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return ctx_.batch(std::forward<F>(fn));
  }

  Context& context() { return ctx_; }

  template <typename T>
  CellHandle<T> cell(T value) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return ctx_.cell(std::move(value));
  }

  template <typename T>
  void set_cell(const CellHandle<T>& handle, T value) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    ctx_.set_cell(handle, std::move(value));
  }

  template <typename T>
  T get_cell(const CellHandle<T>& handle) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return ctx_.get_cell(handle);
  }

  template <typename T, typename F>
  SlotHandle<T> slot(F&& compute) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return ctx_.slot<T>(std::forward<F>(compute));
  }

  template <typename T, typename F>
  SlotHandle<T> memo(F&& compute) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return ctx_.memo<T>(std::forward<F>(compute));
  }

  template <typename T, typename F>
  SignalHandle<T> signal(F&& compute) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return ctx_.signal<T>(std::forward<F>(compute));
  }

  template <typename F>
  EffectHandle effect(F&& run) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return ctx_.effect(std::forward<F>(run));
  }

  template <typename F>
  EffectHandle effect_void(F&& run) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return ctx_.effect_void(std::forward<F>(run));
  }

  void dispose_effect(const EffectHandle& handle) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    ctx_.dispose_effect(handle);
  }

  template <typename T>
  T get(const SlotHandle<T>& handle) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return ctx_.get(handle);
  }

  template <typename T>
  bool is_set(const SlotHandle<T>& handle) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return ctx_.is_set(handle);
  }

  bool is_batching() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return ctx_.is_batching();
  }

 private:
  Context ctx_;
  std::recursive_mutex mutex_;
};

// Pure batch-flush kernel (faithful port of Lean ThreadSafe model)
struct NodeEntry {
  std::shared_ptr<void> value;
  std::type_index type_id;
  std::string state;
  NodeEntry() : type_id(std::type_index(typeid(void))), state("clean") {}
  NodeEntry(std::shared_ptr<void> v, std::type_index t, std::string s)
      : value(std::move(v)), type_id(t), state(std::move(s)) {}
};

struct BatchWrite {
  std::shared_ptr<void> node_id;
  std::shared_ptr<void> value;
  std::type_index type_id;
  BatchWrite(std::shared_ptr<void> id, std::shared_ptr<void> v, std::type_index t)
      : node_id(std::move(id)), value(std::move(v)), type_id(t) {}
};

}  // namespace lazily

#endif  // LAZILY_THREAD_SAFE_HPP
