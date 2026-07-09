#ifndef LAZILY_THREAD_SAFE_HPP
#define LAZILY_THREAD_SAFE_HPP

#include <lazily/context.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <thread>
#include <typeindex>

namespace lazily {

// ═══════════════════════════════════════════════════════════════════════════
// Lock policies for BasicThreadSafeContext.
//
// Two policies ship:
//   - RecursiveLockPolicy (default): a recursive_mutex. Every operation takes
//     an exclusive lock. No single-thread overhead, but reads serialize with
//     writes and each other — concurrent readers do NOT scale.
//   - RwLockPolicy (opt-in): a shared_mutex. Cached reads run under a SHARED
//     lock (concurrent readers scale ~1.7× at 16 threads); mutations run under
//     an EXCLUSIVE lock. The exclusive acquire is heavier, so write-heavy
//     single-thread paths regress ~2× vs the recursive policy. Choose by
//     workload: read-heavy + concurrent → Rw; everything else → Recursive.
//
// Re-entrancy: Context is single-threaded by design and its recompute/effect
// cascades invoke user callbacks that may call back into wrapper methods. A
// plain shared_mutex is not recursive, so the RW policy tracks the exclusive
// owner via a token: a wrapper call by the thread already holding the exclusive
// lock bypasses locking and operates on the inner Context directly. The
// recursive policy ignores the token (recursive_mutex is natively re-entrant).
// ═══════════════════════════════════════════════════════════════════════════

struct RecursiveLockPolicy {
  using mutex_type = std::recursive_mutex;
  static constexpr bool kSharedReads = false;

  struct read_guard {
    std::lock_guard<mutex_type> g;
    explicit read_guard(mutex_type& m) : g(m) {}
  };
  // Exclusive guard. (token/me unused — recursion is native.)
  struct write_guard {
    std::lock_guard<mutex_type> g;
    write_guard(mutex_type& m, std::atomic<uint64_t>& /*token*/, uint64_t /*me*/)
        : g(m) {}
  };
};

struct RwLockPolicy {
  using mutex_type = std::shared_mutex;
  static constexpr bool kSharedReads = true;

  struct read_guard {
    std::shared_lock<mutex_type> g;
    explicit read_guard(mutex_type& m) : g(m) {}
  };
  // Exclusive guard that publishes/clears the owner token for re-entrancy.
  struct write_guard {
    std::unique_lock<mutex_type> g;
    std::atomic<uint64_t>* token;
    write_guard(mutex_type& m, std::atomic<uint64_t>& t, uint64_t me)
        : g(m), token(&t) {
      t.store(me, std::memory_order_release);
    }
    ~write_guard() { token->store(0, std::memory_order_release); }
  };
};

template <typename Policy = RecursiveLockPolicy>
class BasicThreadSafeContext {
 public:
  BasicThreadSafeContext() : ctx_() {}

  /// Exclusive write access. Re-entrant for the owning writer thread.
  template <typename F>
  auto write(F&& fn) -> decltype(fn(std::declval<Context&>())) {
    if (am_writer()) return fn(ctx_);
    typename Policy::write_guard g(mutex_, writer_token_, my_token());
    return fn(ctx_);
  }

  /// Read access. Shared for RwLockPolicy, exclusive for RecursiveLockPolicy.
  /// Re-entrant writers bypass.
  template <typename F>
  auto read(F&& fn) -> decltype(fn(std::declval<Context&>())) {
    if (am_writer()) return fn(ctx_);
    typename Policy::read_guard g(mutex_);
    return fn(ctx_);
  }

  Context& context() { return ctx_; }

  // -- Mutating API (exclusive) --
  template <typename T>
  CellHandle<T> cell(T value) {
    return write([&](Context& c) { return c.cell(std::move(value)); });
  }
  template <typename T>
  void set_cell(const CellHandle<T>& handle, T value) {
    write([&](Context& c) { c.set_cell(handle, std::move(value)); });
  }
  template <typename F>
  auto batch(F&& fn) {
    return write([&](Context& c) { return c.batch(std::forward<F>(fn)); });
  }

  template <typename T, typename F>
  SlotHandle<T> slot(F&& compute) {
    return write(
        [&](Context& c) { return c.template slot<T>(std::forward<F>(compute)); });
  }
  template <typename T, typename F>
  SlotHandle<T> computed(F&& compute) {
    return slot<T>(std::forward<F>(compute));
  }
  template <typename T, typename F>
  SlotHandle<T> memo(F&& compute) {
    return write(
        [&](Context& c) { return c.template memo<T>(std::forward<F>(compute)); });
  }
  template <typename T, typename F>
  SignalHandle<T> signal(F&& compute) {
    return write(
        [&](Context& c) { return c.template signal<T>(std::forward<F>(compute)); });
  }
  template <typename F>
  EffectHandle effect(F&& run) {
    return write([&](Context& c) { return c.effect(std::forward<F>(run)); });
  }
  template <typename F>
  EffectHandle effect_void(F&& run) {
    return write([&](Context& c) { return c.effect_void(std::forward<F>(run)); });
  }
  void dispose_effect(const EffectHandle& handle) {
    write([&](Context& c) { c.dispose_effect(handle); });
  }
  template <typename T>
  void dispose_signal(const SignalHandle<T>& handle) {
    write([&](Context& c) { c.dispose_signal(handle); });
  }

  // -- Read API --
  template <typename T>
  T get_cell(const CellHandle<T>& handle) {
    if (am_writer()) return ctx_.get_cell(handle);
    if constexpr (Policy::kSharedReads) {
      typename Policy::read_guard g(mutex_);
      auto v = ctx_.peek_cell<T>(handle);
      assert(v && "get_cell on non-cell");
      return std::move(*v);
    } else {
      typename Policy::read_guard g(mutex_);
      return ctx_.get_cell(handle);
    }
  }

  template <typename T>
  T get(const SlotHandle<T>& handle) {
    if (am_writer()) return ctx_.get(handle);
    if constexpr (Policy::kSharedReads) {
      {
        typename Policy::read_guard g(mutex_);
        auto cached = ctx_.try_get_cached<T>(handle);
        if (cached) return std::move(*cached);
      }
      typename Policy::write_guard g(mutex_, writer_token_, my_token());
      return ctx_.get(handle);
    } else {
      typename Policy::read_guard g(mutex_);
      return ctx_.get(handle);
    }
  }

  template <typename T>
  T get_signal(const SignalHandle<T>& handle) {
    return get<T>(handle.slot);
  }

  template <typename T>
  std::shared_ptr<T> get_cell_rc(const CellHandle<T>& handle) {
    if (am_writer()) return ctx_.get_cell_rc(handle);
    if constexpr (Policy::kSharedReads) {
      typename Policy::read_guard g(mutex_);
      auto v = ctx_.peek_cell<T>(handle);
      assert(v && "get_cell_rc on non-cell");
      return std::make_shared<T>(std::move(*v));
    } else {
      typename Policy::read_guard g(mutex_);
      return ctx_.get_cell_rc(handle);
    }
  }

  template <typename T>
  std::shared_ptr<T> get_rc(const SlotHandle<T>& handle) {
    if (am_writer()) return ctx_.get_rc(handle);
    if constexpr (Policy::kSharedReads) {
      {
        typename Policy::read_guard g(mutex_);
        auto cached = ctx_.try_get_cached<T>(handle);
        if (cached) return std::make_shared<T>(std::move(*cached));
      }
      typename Policy::write_guard g(mutex_, writer_token_, my_token());
      return ctx_.get_rc(handle);
    } else {
      typename Policy::read_guard g(mutex_);
      return ctx_.get_rc(handle);
    }
  }

  template <typename T>
  bool is_set(const SlotHandle<T>& handle) {
    return read([&](Context& c) { return c.is_set(handle); });
  }
  bool is_batching() {
    return read([&](Context& c) { return c.is_batching(); });
  }
  bool is_effect_active(const EffectHandle& handle) {
    return read([&](Context& c) { return c.is_effect_active(handle); });
  }

 private:
  Context ctx_;
  typename Policy::mutex_type mutex_;
  // Token of the thread holding the exclusive write lock (0 = none). Meaningful
  // only for RwLockPolicy; read racily by non-owners, which is safe (a non-owner
  // never equals itself, so it always takes the normal path regardless of
  // staleness). RecursiveLockPolicy never sets it, so am_writer() stays false
  // and relies on native recursion instead.
  std::atomic<uint64_t> writer_token_{0};

  static uint64_t my_token() {
    static std::atomic<uint64_t> counter{0};
    thread_local uint64_t tok =
        counter.fetch_add(1, std::memory_order_relaxed) + 1;
    return tok;
  }
  bool am_writer() const {
    return writer_token_.load(std::memory_order_acquire) == my_token();
  }
};

/// Default thread-safe context — recursive_mutex (no single-thread overhead,
/// reads serialize). Unchanged behavior from v0.3.0.
using ThreadSafeContext = BasicThreadSafeContext<RecursiveLockPolicy>;

/// Read/write-lock thread-safe context — shared_mutex. Cached reads scale across
/// cores (~1.7× at 16 threads); exclusive writes are heavier (~2× single-thread
/// regression vs the recursive default). Choose for read-heavy concurrent loads.
using RwThreadSafeContext = BasicThreadSafeContext<RwLockPolicy>;

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
