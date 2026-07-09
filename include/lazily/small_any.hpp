#ifndef LAZILY_SMALL_ANY_HPP
#define LAZILY_SMALL_ANY_HPP

#include <cstddef>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

namespace lazily {

/// Type-erased value with small-buffer optimisation — the value analogue of
/// `SmallFn`. Stores trivially-copyable values up to \p BufSize bytes inline
/// (no heap allocation); larger or non-trivially-copyable values fall back to
/// the heap.
///
/// This replaces `RcPtr<RcAny>` for Context value storage (optimization B):
/// the common reactive value (int / double / small POD) is stored inline in
/// the node with zero allocation per cell/slot/write, instead of one
/// `new RcBox<T>` (plus malloc-block header) per value. Because Context values
/// are single-owner and only ever moved (never shared/copied), no refcount is
/// needed — `RcPtr`'s refcount was vestigial here.
///
/// Move-only (matches actual usage). Trivially-copyable types use a memcpy
/// relocate + no-op destroy fast path; others use the heap path (one
/// allocation at construction, pointer relocated on move).
template <size_t BufSize = 2 * sizeof(void*)>
class SmallAny {
  struct VTable {
    const void* (*raw)(const SmallAny*);
    void (*destroy)(SmallAny*) noexcept;
    void (*move)(SmallAny* src, SmallAny* dst) noexcept;
  };

  alignas(std::max_align_t) unsigned char buffer_[BufSize];
  const VTable* vtable_ = nullptr;

  template <typename T>
  static const VTable* inline_vtable() {
    static const VTable vt{
        [](const SmallAny* s) -> const void* { return s->buffer_; },
        [](SmallAny*) noexcept {},  // trivially-copyable ⇒ trivial destructor
        [](SmallAny* src, SmallAny* dst) noexcept {
          std::memcpy(dst->buffer_, src->buffer_, BufSize);
        }};
    return &vt;
  }

  template <typename T>
  static const VTable* heap_vtable() {
    using P = T*;
    static const VTable vt{
        [](const SmallAny* s) -> const void* {
          return *reinterpret_cast<P*>(const_cast<unsigned char*>(s->buffer_));
        },
        [](SmallAny* s) noexcept {
          P& p = *reinterpret_cast<P*>(s->buffer_);
          delete p;
          p = nullptr;
        },
        [](SmallAny* src, SmallAny* dst) noexcept {
          P* sp = reinterpret_cast<P*>(src->buffer_);
          P* dp = reinterpret_cast<P*>(dst->buffer_);
          *dp = *sp;
          *sp = nullptr;
        }};
    return &vt;
  }

 public:
  SmallAny() noexcept = default;
  SmallAny(std::nullptr_t) noexcept {}

  template <typename T, typename... Args>
  static SmallAny make(Args&&... args) {
    SmallAny s;
    if constexpr (sizeof(T) <= BufSize &&
                  alignof(T) <= alignof(std::max_align_t) &&
                  std::is_trivially_copyable_v<T>) {
      s.vtable_ = inline_vtable<T>();
      new (s.buffer_) T(std::forward<Args>(args)...);
    } else {
      s.vtable_ = heap_vtable<T>();
      new (s.buffer_) T*(new T(std::forward<Args>(args)...));
    }
    return s;
  }

  SmallAny(const SmallAny&) = delete;
  SmallAny& operator=(const SmallAny&) = delete;

  SmallAny(SmallAny&& other) noexcept {
    if (other.vtable_) {
      other.vtable_->move(&other, this);
      vtable_ = other.vtable_;
      other.vtable_ = nullptr;
    }
  }
  SmallAny& operator=(SmallAny&& other) noexcept {
    if (this != &other) {
      reset();
      if (other.vtable_) {
        other.vtable_->move(&other, this);
        vtable_ = other.vtable_;
        other.vtable_ = nullptr;
      }
    }
    return *this;
  }
  ~SmallAny() { reset(); }

  void reset() noexcept {
    if (vtable_) {
      vtable_->destroy(this);
      vtable_ = nullptr;
    }
  }

  explicit operator bool() const noexcept { return vtable_ != nullptr; }
  const void* raw() const noexcept {
    return vtable_ ? vtable_->raw(this) : nullptr;
  }

  template <typename T>
  const T* as() const noexcept {
    return static_cast<const T*>(raw());
  }
  template <typename T>
  T* as_mut() noexcept {
    return static_cast<T*>(const_cast<void*>(raw()));
  }
};

}  // namespace lazily

#endif  // LAZILY_SMALL_ANY_HPP
