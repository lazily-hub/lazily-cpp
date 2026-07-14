#ifndef LAZILY_WINDOWING_HPP
#define LAZILY_WINDOWING_HPP

// Stream windowing (`#lzwindow`).
//
// A C++17 port of `lazily-rs/src/windowing.rs` (Phase 5 of the realtime +
// distributed primitives plan). See `lazily-spec/docs/windowing.md`. Window
// aggregation *is* a merge, so the `MergePolicy` algebra in `merge.hpp`
// (`Sum`/`Max`/`SetUnion`/custom) composes: the aggregate of a window equals the
// associative fold of its elements. Each primitive is a pure compute **core**
// (window bookkeeping + a merge-policy fold) split from a reactive **cell**
// projecting the last emitted aggregate onto a `CellHandle<std::optional<T>>`.
//
//   - TumblingCountCore / TumblingCountWindow — count-based, non-overlapping.
//   - TumblingTimeCore  / TumblingTimeWindow  — time-based, non-overlapping.
//   - SlidingCore       / SlidingWindow       — overlapping, fold-recompute.
//   - SessionCore       / SessionWindow       — gap-based sessionization.
//
// Windows are driven by pushed elements plus a monotone logical `now` (for the
// time and session variants). Emit-boundary semantics mirror the Rust source
// exactly: a window emits its aggregate only when it closes.

#include <lazily/context.hpp>
#include <lazily/merge.hpp>

#include <cstdint>
#include <deque>
#include <optional>
#include <utility>

namespace lazily {

// -- Merge helpers (mirrors the Rust free fns) ------------------------------

/// Fold `v` into an optional accumulator under `M` (identity when empty).
template <typename T, typename M>
inline void window_merge_into(std::optional<T>& acc, T v) {
  if (!acc.has_value()) {
    acc = std::move(v);
  } else {
    acc = M::template merge<T>(*acc, std::move(v));
  }
}

/// Fold a window of elements under `M` (`std::nullopt` for an empty window).
template <typename T, typename M>
inline std::optional<T> window_fold(const std::deque<T>& items) {
  std::optional<T> acc;
  for (const auto& v : items) window_merge_into<T, M>(acc, v);
  return acc;
}

// Move the accumulator out, leaving it empty (mirrors Rust `Option::take`).
template <typename T>
inline std::optional<T> take_opt(std::optional<T>& acc) {
  std::optional<T> out = std::move(acc);
  acc.reset();
  return out;
}

// ===========================================================================
// Tumbling (count)
// ===========================================================================

/// Count-based tumbling window compute core.
template <typename T, typename M>
class TumblingCountCore {
 public:
  explicit TumblingCountCore(uint64_t n) : n_(n < 1 ? 1 : n) {}

  /// Push an element; emit the window aggregate on the `n`-th and reset.
  std::optional<T> push(T v) {
    window_merge_into<T, M>(acc_, std::move(v));
    ++count_;
    if (count_ >= n_) {
      count_ = 0;
      return take_opt(acc_);
    }
    return std::nullopt;
  }

 private:
  uint64_t n_;
  std::optional<T> acc_;
  uint64_t count_ = 0;
};

// ===========================================================================
// Tumbling (time)
// ===========================================================================

/// Time-based tumbling window compute core.
template <typename T, typename M>
class TumblingTimeCore {
 public:
  explicit TumblingTimeCore(uint64_t period)
      : period_(period < 1 ? 1 : period), next_(period < 1 ? 1 : period) {}

  /// Accumulate an element into the current window (`now` is unused).
  void push(uint64_t /*now*/, T v) { window_merge_into<T, M>(acc_, std::move(v)); }

  /// At a period boundary emit the window aggregate (empty window -> nullopt).
  std::optional<T> tick(uint64_t now) {
    if (now < next_) return std::nullopt;
    while (next_ <= now) next_ += period_;
    return take_opt(acc_);
  }

 private:
  uint64_t period_;
  uint64_t next_;
  std::optional<T> acc_;
};

// ===========================================================================
// Sliding (count)
// ===========================================================================

/// Count-based sliding window compute core (fold-recompute, correct for any
/// associative merge).
template <typename T, typename M>
class SlidingCore {
 public:
  SlidingCore(uint64_t size, uint64_t slide)
      : size_(size < 1 ? 1 : size), slide_(slide < 1 ? 1 : slide) {}

  /// Push an element; every `slide` pushes emit the fold over the last `size`.
  std::optional<T> push(T v) {
    buffer_.push_back(std::move(v));
    while (buffer_.size() > size_) buffer_.pop_front();
    ++since_;
    if (since_ >= slide_) {
      since_ = 0;
      return window_fold<T, M>(buffer_);
    }
    return std::nullopt;
  }

 private:
  uint64_t size_;
  uint64_t slide_;
  std::deque<T> buffer_;
  uint64_t since_ = 0;
};

// ===========================================================================
// Session (gap-based)
// ===========================================================================

/// Gap-based sessionization compute core.
template <typename T, typename M>
class SessionCore {
 public:
  explicit SessionCore(uint64_t gap) : gap_(gap) {}

  /// Push an element; a gap larger than `gap` closes the session (emitting its
  /// aggregate) and opens a new one.
  std::optional<T> push(uint64_t now, T v) {
    if (idle(now) && acc_.has_value()) {
      std::optional<T> emit = take_opt(acc_);
      acc_ = std::move(v);
      last_ = now;
      return emit;
    }
    window_merge_into<T, M>(acc_, std::move(v));
    last_ = now;
    return std::nullopt;
  }

  /// Close the open session if it has been idle longer than `gap`.
  std::optional<T> flush(uint64_t now) {
    if (idle(now) && acc_.has_value()) return take_opt(acc_);
    return std::nullopt;
  }

 private:
  // Whether more than `gap` has elapsed since the last element (saturating).
  bool idle(uint64_t now) const {
    if (!last_.has_value()) return false;
    uint64_t delta = now >= *last_ ? now - *last_ : 0;
    return delta > gap_;
  }

  uint64_t gap_;
  std::optional<T> acc_;
  std::optional<uint64_t> last_;
};

// ===========================================================================
// Reactive cells
// ===========================================================================

// Project the last emitted aggregate onto the output cell (only on a real emit).
template <typename T>
inline void window_set_output(Context& ctx,
                              const CellHandle<std::optional<T>>& cell,
                              const std::optional<T>& emitted) {
  if (emitted.has_value()) ctx.set_cell(cell, std::optional<T>(*emitted));
}

/// Reactive count-tumbling window; projects the last emitted aggregate.
template <typename T, typename M>
class TumblingCountWindow {
 public:
  TumblingCountWindow(Context& ctx, uint64_t n)
      : core_(n), output_(ctx.cell(std::optional<T>())) {}

  std::optional<T> push(Context& ctx, T v) {
    std::optional<T> e = core_.push(std::move(v));
    window_set_output(ctx, output_, e);
    return e;
  }
  std::optional<T> output(Context& ctx) const { return ctx.get_cell(output_); }
  CellHandle<std::optional<T>> output_cell() const { return output_; }

 private:
  TumblingCountCore<T, M> core_;
  CellHandle<std::optional<T>> output_;
};

/// Reactive time-tumbling window (`push(now, v)` + `tick(now)`).
template <typename T, typename M>
class TumblingTimeWindow {
 public:
  TumblingTimeWindow(Context& ctx, uint64_t period)
      : core_(period), output_(ctx.cell(std::optional<T>())) {}

  void push(Context& /*ctx*/, uint64_t now, T v) { core_.push(now, std::move(v)); }
  std::optional<T> tick(Context& ctx, uint64_t now) {
    std::optional<T> e = core_.tick(now);
    window_set_output(ctx, output_, e);
    return e;
  }
  std::optional<T> output(Context& ctx) const { return ctx.get_cell(output_); }
  CellHandle<std::optional<T>> output_cell() const { return output_; }

 private:
  TumblingTimeCore<T, M> core_;
  CellHandle<std::optional<T>> output_;
};

/// Reactive count-sliding window; projects the last emitted aggregate.
template <typename T, typename M>
class SlidingWindow {
 public:
  SlidingWindow(Context& ctx, uint64_t size, uint64_t slide)
      : core_(size, slide), output_(ctx.cell(std::optional<T>())) {}

  std::optional<T> push(Context& ctx, T v) {
    std::optional<T> e = core_.push(std::move(v));
    window_set_output(ctx, output_, e);
    return e;
  }
  std::optional<T> output(Context& ctx) const { return ctx.get_cell(output_); }
  CellHandle<std::optional<T>> output_cell() const { return output_; }

 private:
  SlidingCore<T, M> core_;
  CellHandle<std::optional<T>> output_;
};

/// Reactive session window (`push(now, v)` + `flush(now)`).
template <typename T, typename M>
class SessionWindow {
 public:
  SessionWindow(Context& ctx, uint64_t gap)
      : core_(gap), output_(ctx.cell(std::optional<T>())) {}

  std::optional<T> push(Context& ctx, uint64_t now, T v) {
    std::optional<T> e = core_.push(now, std::move(v));
    window_set_output(ctx, output_, e);
    return e;
  }
  std::optional<T> flush(Context& ctx, uint64_t now) {
    std::optional<T> e = core_.flush(now);
    window_set_output(ctx, output_, e);
    return e;
  }
  std::optional<T> output(Context& ctx) const { return ctx.get_cell(output_); }
  CellHandle<std::optional<T>> output_cell() const { return output_; }

 private:
  SessionCore<T, M> core_;
  CellHandle<std::optional<T>> output_;
};

}  // namespace lazily

#endif  // LAZILY_WINDOWING_HPP
