#ifndef LAZILY_SMALL_FN_HPP
#define LAZILY_SMALL_FN_HPP

#include <cstddef>
#include <new>
#include <utility>

namespace lazily {

/// Type-erased callable with small-buffer optimisation.
///
/// Stores closures up to \p BufSize bytes inline (no heap allocation).
/// Larger closures fall back to the heap automatically.
///
/// Unlike std::function, the closure is stored inline when it fits, eliminating
/// the per-node heap allocation that std::function incurs for captured lambdas.
template <typename Sig, size_t BufSize = 64>
class SmallFn;

template <typename R, typename... Args, size_t BufSize>
class SmallFn<R(Args...), BufSize> {
  struct VTable {
    R (*invoke)(void*, Args...);
    void (*destroy)(void*) noexcept;
    void (*move)(void* src, void* dst) noexcept;
  };

  alignas(std::max_align_t) unsigned char buffer_[BufSize];
  const VTable* vtable_ = nullptr;

  template <typename F>
  static const VTable* inline_vtable() {
    using FType = F;
    static const VTable vt{
        [](void* p, Args... args) -> R {
          return (*static_cast<FType*>(p))(std::forward<Args>(args)...);
        },
        [](void* p) noexcept { static_cast<FType*>(p)->~FType(); },
        [](void* src, void* dst) noexcept {
          new (dst) FType(std::move(*static_cast<FType*>(src)));
          static_cast<FType*>(src)->~FType();
        }};
    return &vt;
  }

  template <typename F>
  static const VTable* heap_vtable() {
    using FType = F;
    static const VTable vt{
        [](void* p, Args... args) -> R {
          FType*& ref = *reinterpret_cast<FType**>(p);
          return (*ref)(std::forward<Args>(args)...);
        },
        [](void* p) noexcept {
          FType*& ref = *reinterpret_cast<FType**>(p);
          delete ref;
          ref = nullptr;
        },
        [](void* src, void* dst) noexcept {
          FType** sp = reinterpret_cast<FType**>(src);
          FType** dp = reinterpret_cast<FType**>(dst);
          *dp = *sp;
          *sp = nullptr;
        }};
    return &vt;
  }

 public:
  SmallFn() noexcept = default;
  SmallFn(std::nullptr_t) noexcept {}

  template <typename F, typename = std::enable_if_t<
                            !std::is_same_v<std::decay_t<F>, SmallFn> &&
                            !std::is_same_v<std::decay_t<F>, std::nullptr_t>>>
  SmallFn(F&& f) {
    using FType = std::decay_t<F>;
    if constexpr (sizeof(FType) <= BufSize &&
                  alignof(FType) <= alignof(std::max_align_t)) {
      vtable_ = inline_vtable<FType>();
      new (buffer_) FType(std::forward<F>(f));
    } else {
      vtable_ = heap_vtable<FType>();
      FType* ptr = new FType(std::forward<F>(f));
      new (buffer_) FType*(ptr);
    }
  }

  SmallFn(SmallFn&& other) noexcept {
    if (other.vtable_) {
      other.vtable_->move(other.buffer_, buffer_);
      vtable_ = other.vtable_;
      other.vtable_ = nullptr;
    }
  }

  SmallFn& operator=(SmallFn&& other) noexcept {
    if (this != &other) {
      reset();
      if (other.vtable_) {
        other.vtable_->move(other.buffer_, buffer_);
        vtable_ = other.vtable_;
        other.vtable_ = nullptr;
      }
    }
    return *this;
  }

  SmallFn(const SmallFn&) = delete;
  SmallFn& operator=(const SmallFn&) = delete;

  ~SmallFn() { reset(); }

  void reset() noexcept {
    if (vtable_) {
      vtable_->destroy(buffer_);
      vtable_ = nullptr;
    }
  }

  explicit operator bool() const noexcept { return vtable_ != nullptr; }

  R operator()(Args... args) {
    return vtable_->invoke(buffer_, std::forward<Args>(args)...);
  }
};

}  // namespace lazily

#endif  // LAZILY_SMALL_FN_HPP
