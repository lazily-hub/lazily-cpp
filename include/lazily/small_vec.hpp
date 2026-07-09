#ifndef LAZILY_SMALL_VEC_HPP
#define LAZILY_SMALL_VEC_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>
#include <utility>

namespace lazily {

/// Small-buffer-optimised vector: stores up to \p InlineCapacity elements inline
/// (no heap allocation), falling back to a heap buffer for larger sizes.
///
/// Uses a union layout so that heap metadata (pointer + capacity) overlaps with
/// the inline buffer. For InlineCapacity=2 with 8-byte T, the total size equals
/// std::vector (24 bytes), but with zero heap allocation for 0-2 elements.
template <typename T, size_t InlineCapacity>
class SmallVec {
  static constexpr size_t kInlineBytes = InlineCapacity * sizeof(T);
  static constexpr size_t kHeapMetaBytes = sizeof(T*) + sizeof(size_t);
  static constexpr size_t kBufBytes =
      kInlineBytes >= kHeapMetaBytes ? kInlineBytes : kHeapMetaBytes;
  static constexpr size_t kHeapBit = size_t(1) << (sizeof(size_t) * 8 - 1);
  static constexpr bool kTrivial = std::is_trivially_copyable_v<T>;

 public:
  static constexpr size_t kInlineCapacity = InlineCapacity;

  SmallVec() noexcept = default;

  SmallVec(const SmallVec& other) {
    if constexpr (kTrivial) {
      copy_trivial(other);
    } else {
      copy_from(other);
    }
  }

  SmallVec(SmallVec&& other) noexcept {
    if constexpr (kTrivial) {
      copy_trivial(other);
      other.raw_size_ = 0;
    } else {
      move_from(other);
    }
  }

  SmallVec& operator=(const SmallVec& other) {
    if (this != &other) {
      if constexpr (kTrivial) {
        destroy_release_reset();
        copy_trivial(other);
      } else {
        destroy_release_reset();
        copy_from(other);
      }
    }
    return *this;
  }

  SmallVec& operator=(SmallVec&& other) noexcept {
    if (this != &other) {
      if constexpr (kTrivial) {
        destroy_release_reset();
        copy_trivial(other);
        other.raw_size_ = 0;
      } else {
        destroy_release_reset();
        move_from(other);
      }
    }
    return *this;
  }

  ~SmallVec() {
    if constexpr (kTrivial) {
      if (is_heap()) ::operator delete(heap_ptr());
    } else {
      destroy_release();
    }
  }

  size_t size() const noexcept { return raw_size_ & ~kHeapBit; }
  bool empty() const noexcept { return size() == 0; }
  size_t capacity() const noexcept {
    return is_heap() ? heap_cap() : InlineCapacity;
  }

  T* data() noexcept {
    return is_heap() ? heap_ptr() : reinterpret_cast<T*>(buf_);
  }
  const T* data() const noexcept {
    return is_heap() ? heap_ptr() : reinterpret_cast<const T*>(buf_);
  }

  T& operator[](size_t i) { return data()[i]; }
  const T& operator[](size_t i) const { return data()[i]; }
  T& front() { return data()[0]; }
  const T& front() const { return data()[0]; }
  T& back() { return data()[size() - 1]; }
  const T& back() const { return data()[size() - 1]; }

  T* begin() noexcept { return data(); }
  T* end() noexcept { return data() + size(); }
  const T* begin() const noexcept { return data(); }
  const T* end() const noexcept { return data() + size(); }

  void push_back(T value) {
    if (is_heap()) {
      if (size() == heap_cap()) grow_heap();
      if constexpr (kTrivial) {
        heap_ptr()[size()] = value;
      } else {
        new (heap_ptr() + size()) T(std::move(value));
      }
    } else if (size() < InlineCapacity) {
      if constexpr (kTrivial) {
        reinterpret_cast<T*>(buf_)[size()] = value;
      } else {
        new (reinterpret_cast<T*>(buf_) + size()) T(std::move(value));
      }
    } else {
      migrate_to_heap(std::move(value));
      return;
    }
    set_size(size() + 1);
  }

  void pop_back() {
    size_t s = size();
    if constexpr (!kTrivial) data()[s - 1].~T();
    set_size(s - 1);
  }

  void clear() noexcept {
    if constexpr (kTrivial) {
      if (is_heap()) ::operator delete(heap_ptr());
      raw_size_ = 0;
    } else {
      destroy_release_reset();
    }
  }

  void erase_swap(size_t i) {
    size_t s = size();
    if (i >= s) return;
    if (i != s - 1) data()[i] = std::move(data()[s - 1]);
    pop_back();
  }

 private:
  alignas(T) unsigned char buf_[kBufBytes];
  size_t raw_size_ = 0;

  bool is_heap() const noexcept { return (raw_size_ & kHeapBit) != 0; }
  void set_inline_size(size_t s) noexcept { raw_size_ = s; }
  void set_heap_size(size_t s) noexcept { raw_size_ = s | kHeapBit; }
  void set_size(size_t s) noexcept {
    if (is_heap()) set_heap_size(s);
    else set_inline_size(s);
  }

  T*& heap_ptr_ref() noexcept { return *reinterpret_cast<T**>(buf_); }
  T* heap_ptr() noexcept { return heap_ptr_ref(); }
  const T* heap_ptr() const noexcept {
    return *reinterpret_cast<T* const*>(buf_);
  }
  size_t& heap_cap_ref() noexcept {
    return *reinterpret_cast<size_t*>(buf_ + sizeof(T*));
  }
  size_t heap_cap() const noexcept {
    return *reinterpret_cast<const size_t*>(buf_ + sizeof(T*));
  }
  void set_heap(T* ptr, size_t cap, size_t sz) noexcept {
    heap_ptr_ref() = ptr;
    heap_cap_ref() = cap;
    raw_size_ = sz | kHeapBit;
  }

  void copy_trivial(const SmallVec& other) noexcept {
    raw_size_ = other.raw_size_;
    if (other.is_heap()) {
      size_t s = other.size();
      size_t cap = other.heap_cap();
      T* ptr = static_cast<T*>(::operator new(cap * sizeof(T)));
      std::memcpy(ptr, other.heap_ptr(), s * sizeof(T));
      heap_ptr_ref() = ptr;
      heap_cap_ref() = cap;
    } else {
      std::memcpy(buf_, other.buf_, other.size() * sizeof(T));
    }
  }

  void copy_from(const SmallVec& other) {
    size_t s = other.size();
    if (other.is_heap()) {
      size_t cap = other.heap_cap();
      T* ptr = static_cast<T*>(::operator new(cap * sizeof(T)));
      for (size_t i = 0; i < s; ++i) new (ptr + i) T(other.heap_ptr()[i]);
      set_heap(ptr, cap, s);
    } else {
      const T* src = reinterpret_cast<const T*>(other.buf_);
      T* dst = reinterpret_cast<T*>(buf_);
      for (size_t i = 0; i < s; ++i) new (dst + i) T(src[i]);
      set_inline_size(s);
    }
  }

  void move_from(SmallVec& other) noexcept {
    if (other.is_heap()) {
      T* ptr = other.heap_ptr();
      size_t cap = other.heap_cap();
      size_t s = other.size();
      other.raw_size_ = 0;
      set_heap(ptr, cap, s);
    } else {
      T* src = reinterpret_cast<T*>(other.buf_);
      T* dst = reinterpret_cast<T*>(buf_);
      for (size_t i = 0; i < other.size(); ++i)
        new (dst + i) T(std::move(src[i]));
      set_inline_size(other.size());
      other.raw_size_ = 0;
    }
  }

  void migrate_to_heap(T&& extra) {
    size_t s = size();
    size_t new_cap = InlineCapacity * 2;
    if (new_cap < 4) new_cap = 4;
    T* buf = static_cast<T*>(::operator new(new_cap * sizeof(T)));
    if constexpr (kTrivial) {
      std::memcpy(buf, data(), s * sizeof(T));
      buf[s] = std::move(extra);
    } else {
      T* src = data();
      for (size_t i = 0; i < s; ++i) new (buf + i) T(std::move(src[i]));
      new (buf + s) T(std::move(extra));
      for (size_t i = 0; i < s; ++i) src[i].~T();
    }
    set_heap(buf, new_cap, s + 1);
  }

  void grow_heap() {
    size_t s = size();
    size_t new_cap = heap_cap() * 2;
    T* buf = static_cast<T*>(::operator new(new_cap * sizeof(T)));
    T* old = heap_ptr();
    if constexpr (kTrivial) {
      std::memcpy(buf, old, s * sizeof(T));
    } else {
      for (size_t i = 0; i < s; ++i) {
        new (buf + i) T(std::move(old[i]));
        old[i].~T();
      }
    }
    ::operator delete(old);
    heap_ptr_ref() = buf;
    heap_cap_ref() = new_cap;
    set_heap_size(s);
  }

  void destroy_release() noexcept {
    T* p = data();
    for (size_t i = 0; i < size(); ++i) p[i].~T();
    if (is_heap()) ::operator delete(heap_ptr());
  }

  void destroy_release_reset() noexcept {
    destroy_release();
    raw_size_ = 0;
  }
};

}  // namespace lazily

#endif  // LAZILY_SMALL_VEC_HPP
