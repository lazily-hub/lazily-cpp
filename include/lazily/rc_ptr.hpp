#ifndef LAZILY_RC_PTR_HPP
#define LAZILY_RC_PTR_HPP

#include <atomic>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace lazily {

// ═══════════════════════════════════════════════════════════════════════════
// Ref-counted bases — inherit to get an intrusive counter + virtual destructor.
// ═══════════════════════════════════════════════════════════════════════════

/// Non-atomic ref-counted base — C++ equivalent of Rust's `Rc`.
/// Safe for single-threaded use (or under an external mutex).
struct RcBase {
  mutable uint32_t rc_strong = 0;
  virtual ~RcBase() = default;
};

/// Atomic ref-counted base — C++ equivalent of Rust's `Arc`.
/// Safe for multi-threaded use.
struct ArcBase {
  mutable std::atomic<uint32_t> arc_strong{0};
  virtual ~ArcBase() = default;
};

inline void rc_add_ref(const RcBase* p) noexcept {
  ++p->rc_strong;
}
inline void rc_release(const RcBase* p) noexcept {
  if (--p->rc_strong == 0) delete p;
}
inline void arc_add_ref(const ArcBase* p) noexcept {
  p->arc_strong.fetch_add(1, std::memory_order_relaxed);
}
inline void arc_release(const ArcBase* p) noexcept {
  if (p->arc_strong.fetch_sub(1, std::memory_order_acq_rel) == 1) delete p;
}

// ═══════════════════════════════════════════════════════════════════════════
// Smart pointers
// ═══════════════════════════════════════════════════════════════════════════

/// Non-atomic reference-counted pointer (≈ Rust `Rc<T>`).
template <typename T>
class RcPtr {
  T* ptr_ = nullptr;

  void add_ref() const noexcept {
    if (ptr_) rc_add_ref(ptr_);
  }
  void release_ptr() noexcept {
    if (ptr_) rc_release(ptr_);
  }

 public:
  RcPtr() noexcept = default;
  RcPtr(std::nullptr_t) noexcept {}

  /// Adopt a raw pointer without incrementing (ref starts at 1 from `new`).
  //
  // RcBase initializes rc_strong to 0; adopt claims the first strong reference
  // by setting it to 1. (Leaving it at 0 would make the release check
  // `--rc_strong == 0` underflow on the first release of a singly-held value,
  // and delete early once any copy bumps 0 -> 1.)
  struct adopt_t {};
  RcPtr(T* p, adopt_t) noexcept : ptr_(p) {
    if (p) p->rc_strong = 1;
  }

  /// Take a raw pointer and increment its ref count.
  explicit RcPtr(T* p) noexcept : ptr_(p) { add_ref(); }

  RcPtr(const RcPtr& o) noexcept : ptr_(o.ptr_) { add_ref(); }
  RcPtr(RcPtr&& o) noexcept : ptr_(o.ptr_) { o.ptr_ = nullptr; }

  /// Converting copy from RcPtr<U> where U* → T*.
  template <typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  RcPtr(const RcPtr<U>& o) noexcept : ptr_(o.get()) {
    add_ref();
  }
  template <typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  RcPtr(RcPtr<U>&& o) noexcept : ptr_(o.detach()) {}

  RcPtr& operator=(const RcPtr& o) noexcept {
    if (this != &o) {
      release_ptr();
      ptr_ = o.ptr_;
      add_ref();
    }
    return *this;
  }
  RcPtr& operator=(RcPtr&& o) noexcept {
    if (this != &o) {
      release_ptr();
      ptr_ = o.ptr_;
      o.ptr_ = nullptr;
    }
    return *this;
  }
  RcPtr& operator=(std::nullptr_t) noexcept {
    release_ptr();
    ptr_ = nullptr;
    return *this;
  }

  ~RcPtr() { release_ptr(); }

  T* get() const noexcept { return ptr_; }
  T& operator*() const noexcept { return *ptr_; }
  T* operator->() const noexcept { return ptr_; }
  explicit operator bool() const noexcept { return ptr_ != nullptr; }

  T* detach() noexcept {
    T* p = ptr_;
    ptr_ = nullptr;
    return p;
  }
  void reset() noexcept {
    release_ptr();
    ptr_ = nullptr;
  }
  void swap(RcPtr& o) noexcept { std::swap(ptr_, o.ptr_); }
};

/// Atomic reference-counted pointer (≈ Rust `Arc<T>`).
/// Identical interface to RcPtr but with atomic ref counting.
template <typename T>
class ArcPtr {
  T* ptr_ = nullptr;

  void add_ref() const noexcept {
    if (ptr_) arc_add_ref(ptr_);
  }
  void release_ptr() noexcept {
    if (ptr_) arc_release(ptr_);
  }

 public:
  ArcPtr() noexcept = default;
  ArcPtr(std::nullptr_t) noexcept {}

  struct adopt_t {};
  ArcPtr(T* p, adopt_t) noexcept : ptr_(p) {
    if (p) p->arc_strong.store(1, std::memory_order_relaxed);
  }
  explicit ArcPtr(T* p) noexcept : ptr_(p) { add_ref(); }

  ArcPtr(const ArcPtr& o) noexcept : ptr_(o.ptr_) { add_ref(); }
  ArcPtr(ArcPtr&& o) noexcept : ptr_(o.ptr_) { o.ptr_ = nullptr; }

  template <typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  ArcPtr(const ArcPtr<U>& o) noexcept : ptr_(o.get()) {
    add_ref();
  }
  template <typename U, typename = std::enable_if_t<std::is_convertible_v<U*, T*>>>
  ArcPtr(ArcPtr<U>&& o) noexcept : ptr_(o.detach()) {}

  ArcPtr& operator=(const ArcPtr& o) noexcept {
    if (this != &o) {
      release_ptr();
      ptr_ = o.ptr_;
      add_ref();
    }
    return *this;
  }
  ArcPtr& operator=(ArcPtr&& o) noexcept {
    if (this != &o) {
      release_ptr();
      ptr_ = o.ptr_;
      o.ptr_ = nullptr;
    }
    return *this;
  }
  ArcPtr& operator=(std::nullptr_t) noexcept {
    release_ptr();
    ptr_ = nullptr;
    return *this;
  }

  ~ArcPtr() { release_ptr(); }

  T* get() const noexcept { return ptr_; }
  T& operator*() const noexcept { return *ptr_; }
  T* operator->() const noexcept { return ptr_; }
  explicit operator bool() const noexcept { return ptr_ != nullptr; }

  T* detach() noexcept {
    T* p = ptr_;
    ptr_ = nullptr;
    return p;
  }
  void reset() noexcept {
    release_ptr();
    ptr_ = nullptr;
  }
  void swap(ArcPtr& o) noexcept { std::swap(ptr_, o.ptr_); }
};

// ═══════════════════════════════════════════════════════════════════════════
// Type-erased value boxes (replace shared_ptr<void>)
// ═══════════════════════════════════════════════════════════════════════════

/// Polymorphic base for type-erased values with non-atomic ref counting.
struct RcAny : RcBase {
  virtual const void* raw() const noexcept = 0;
};
/// Polymorphic base for type-erased values with atomic ref counting.
struct ArcAny : ArcBase {
  virtual const void* raw() const noexcept = 0;
};

template <typename T>
struct RcBox : RcAny {
  T value;
  template <typename... Args>
  explicit RcBox(Args&&... args) : value(std::forward<Args>(args)...) {}
  const void* raw() const noexcept override { return &value; }
};

template <typename T>
struct ArcBox : ArcAny {
  T value;
  template <typename... Args>
  explicit ArcBox(Args&&... args) : value(std::forward<Args>(args)...) {}
  const void* raw() const noexcept override { return &value; }
};

// ── Factories ──

template <typename T, typename... Args>
RcPtr<RcAny> make_rc(Args&&... args) {
  return RcPtr<RcAny>(new RcBox<T>(std::forward<Args>(args)...),
                      typename RcPtr<RcAny>::adopt_t{});
}

template <typename T, typename... Args>
ArcPtr<ArcAny> make_arc(Args&&... args) {
  return ArcPtr<ArcAny>(new ArcBox<T>(std::forward<Args>(args)...),
                        typename ArcPtr<ArcAny>::adopt_t{});
}

// ── Type-safe access ──

template <typename T>
const T* rc_any_cast(const RcAny* p) noexcept {
  return &static_cast<const RcBox<T>*>(p)->value;
}

template <typename T>
const T* arc_any_cast(const ArcAny* p) noexcept {
  return &static_cast<const ArcBox<T>*>(p)->value;
}

template <typename T>
T* rc_any_cast(RcAny* p) noexcept {
  return &static_cast<RcBox<T>*>(p)->value;
}

template <typename T>
T* arc_any_cast(ArcAny* p) noexcept {
  return &static_cast<ArcBox<T>*>(p)->value;
}

template <typename T>
const T* rc_any_cast(const RcPtr<RcAny>& p) noexcept {
  return p ? rc_any_cast<T>(p.get()) : nullptr;
}

template <typename T>
const T* arc_any_cast(const ArcPtr<ArcAny>& p) noexcept {
  return p ? arc_any_cast<T>(p.get()) : nullptr;
}

// ═══════════════════════════════════════════════════════════════════════════
// Traits — select Rc (non-atomic) or Arc (atomic) for Context value storage.
// ═══════════════════════════════════════════════════════════════════════════

struct RcTraits {
  using AnyValue = RcPtr<RcAny>;

  template <typename T, typename... Args>
  static AnyValue make(Args&&... args) {
    return make_rc<T>(std::forward<Args>(args)...);
  }

  template <typename T>
  static const T* cast(const AnyValue& v) noexcept {
    return rc_any_cast<T>(v);
  }

  template <typename T>
  static T* cast_mut(AnyValue& v) noexcept {
    return rc_any_cast<T>(v.get());
  }
};

struct ArcTraits {
  using AnyValue = ArcPtr<ArcAny>;

  template <typename T, typename... Args>
  static AnyValue make(Args&&... args) {
    return make_arc<T>(std::forward<Args>(args)...);
  }

  template <typename T>
  static const T* cast(const AnyValue& v) noexcept {
    return arc_any_cast<T>(v);
  }

  template <typename T>
  static T* cast_mut(AnyValue& v) noexcept {
    return arc_any_cast<T>(v.get());
  }
};

}  // namespace lazily

#endif  // LAZILY_RC_PTR_HPP
