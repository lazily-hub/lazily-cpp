#ifndef LAZILY_TEMPORAL_HPP
#define LAZILY_TEMPORAL_HPP

// Temporal source primitives (`#lztime`).
//
// A C++17 port of `lazily-rs/src/time.rs`. See `lazily-spec/docs/temporal-sources.md`
// and the formal model `lazily-formal/LazilyFormal/Temporal.lean`. Time is modeled
// by a **logical clock** — a monotone `now` (uint64_t) tick that a binding drives
// from its own runtime timer (a game loop, a test). Each source is a pure
// [`TimelineSource`] **compute core** (a side-effect-free state machine over plain
// integers — the C++-eligible, bytes-payload part) split from a thin reactive
// **cell** that projects the core's fire edge onto a `CellHandle` so dependents
// invalidate *only on an actual fire* (the backend-portability rule).
// `DeadlineCell<T>` carries an opaque user value alongside a bytes-eligible
// deadline core.

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <lazily/context.hpp>

namespace lazily {

// ── TimelineSource concept ──
//
// A pure temporal compute core driven by a monotone logical clock. A runtime
// advances any source uniformly via `tick(now)`; `next_fire()` lets a scheduler
// compute the delay to the next wake-up. In Rust this is a trait; in C++ it is a
// duck-typed concept the cores below satisfy:
//   - `bool tick(uint64_t now)` — advance to logical time `now` (callers must not
//     go backwards). Returns `true` on a **fire edge**.
//   - `std::optional<uint64_t> next_fire() const` — logical time of the next fire,
//     or `std::nullopt` when the source is exhausted.

/// A monotone logical clock a manual runtime (game loop, test) can own to drive
/// sources. `advance` clamps backwards moves so `now` is always non-decreasing.
class ManualClock {
 public:
  ManualClock() : now_(0) {}
  uint64_t now() const { return now_; }
  /// Advance to `now` (monotone: a smaller value is clamped to the current
  /// time). Returns the effective `now` a source should be ticked with.
  uint64_t advance(uint64_t now) {
    now_ = std::max(now_, now);
    return now_;
  }

 private:
  uint64_t now_;
};

// ---------------------------------------------------------------------------
// Single-shot timer
// ---------------------------------------------------------------------------

/// Single-shot compute core: `false → true` (`None → Some(())`) at the first
/// tick with `now >= fire_at`; fires exactly once (idempotent thereafter).
class TimerCore {
 public:
  explicit TimerCore(uint64_t fire_at) : fire_at_(fire_at), fired_(false) {}

  bool fired() const { return fired_; }

  bool tick(uint64_t now) {
    if (fired_ || now < fire_at_) return false;
    fired_ = true;
    return true;
  }

  std::optional<uint64_t> next_fire() const {
    if (fired_) return std::nullopt;
    return fire_at_;
  }

  bool operator==(const TimerCore& o) const {
    return fire_at_ == o.fire_at_ && fired_ == o.fired_;
  }
  bool operator!=(const TimerCore& o) const { return !(*this == o); }

 private:
  uint64_t fire_at_;
  bool fired_;
};

/// Reactive single-shot timer: projects `TimerCore`'s fire edge onto a cell so
/// `has_fired`/`value` dependents invalidate only on the fire (idempotent). The
/// `set_cell` store-guard makes a repeat tick a no-op, so dependents invalidate
/// exactly once.
class TimerCell {
 public:
  TimerCell(Context& ctx, uint64_t fire_at)
      : core_(fire_at), fired_(ctx.cell(false)) {}

  /// Advance to logical time `now`; returns the fire edge. On a fire the backing
  /// cell flips to `true`.
  bool tick(Context& ctx, uint64_t now) {
    bool edge = core_.tick(now);
    if (edge) ctx.set_cell(fired_, true);
    return edge;
  }

  /// Whether the timer has fired (reactive read).
  bool has_fired(Context& ctx) const { return fired_.get(ctx); }

  /// `std::nullopt` before the fire, `std::optional<>` set to a unit after
  /// (reactive read). Models `Option<()>` — the presence of a value is what
  /// matters.
  std::optional<bool> value(Context& ctx) const {
    if (fired_.get(ctx)) return std::optional<bool>(true);
    return std::nullopt;
  }

  /// The backing cell, for dependents that want to subscribe directly.
  CellHandle<bool> fired_cell() const { return fired_; }

  std::optional<uint64_t> next_fire() const { return core_.next_fire(); }

 private:
  TimerCore core_;
  CellHandle<bool> fired_;
};

// ---------------------------------------------------------------------------
// Periodic interval
// ---------------------------------------------------------------------------

/// Periodic compute core: fire boundaries at `period, 2*period, …`. A tick counts
/// every boundary in `(frontier, now]`, so a jump past several boundaries counts
/// them all.
class IntervalCore {
 public:
  explicit IntervalCore(uint64_t period)
      : period_(std::max<uint64_t>(period, 1)),
        next_(std::max<uint64_t>(period, 1)),
        count_(0) {}

  uint64_t count() const { return count_; }

  bool tick(uint64_t now) {
    uint64_t fires = fires_this_tick(now);
    if (fires == 0) return false;
    count_ += fires;
    next_ += fires * period_;
    return true;
  }

  std::optional<uint64_t> next_fire() const { return next_; }

  bool operator==(const IntervalCore& o) const {
    return period_ == o.period_ && next_ == o.next_ && count_ == o.count_;
  }
  bool operator!=(const IntervalCore& o) const { return !(*this == o); }

 private:
  /// Boundaries crossed on a single tick (0 when `now` is below the frontier).
  uint64_t fires_this_tick(uint64_t now) const {
    if (now < next_) return 0;
    return (now - next_) / period_ + 1;
  }

  uint64_t period_;
  uint64_t next_;
  uint64_t count_;
};

/// Reactive periodic interval: projects `IntervalCore`'s fire count onto a cell
/// (invalidates only when `count` changes).
class IntervalCell {
 public:
  IntervalCell(Context& ctx, uint64_t period)
      : core_(period), count_(ctx.cell<uint64_t>(0)) {}

  /// Advance to logical time `now`; returns whether a boundary fired. The count
  /// cell mirrors the core's total fire count.
  bool tick(Context& ctx, uint64_t now) {
    bool edge = core_.tick(now);
    if (edge) ctx.set_cell(count_, core_.count());
    return edge;
  }

  /// Total fires so far (reactive read).
  uint64_t count(Context& ctx) const { return count_.get(ctx); }

  CellHandle<uint64_t> count_cell() const { return count_; }

  std::optional<uint64_t> next_fire() const { return core_.next_fire(); }

 private:
  IntervalCore core_;
  CellHandle<uint64_t> count_;
};

// ---------------------------------------------------------------------------
// Cron pattern
// ---------------------------------------------------------------------------

/// Count of `m ∈ 1..=n` with `m mod cycle == o` (`0 <= o < cycle`).
inline uint64_t cron_count_upto(uint64_t n, uint64_t o, uint64_t cycle) {
  if (o == 0)
    return n / cycle;
  if (o <= n)
    return (n - o) / cycle + 1;
  return 0;
}

/// Pattern-periodic compute core: a tick `m >= 1` fires iff `m mod cycle ∈
/// offsets`. Structurally an interval with a match set — a cron expression's
/// shape. The match count in `(cursor, now]` is computed arithmetically, so a
/// large `now` jump is `O(offsets)`.
class CronCore {
 public:
  CronCore(uint64_t cycle, std::vector<uint64_t> offsets)
      : cycle_(std::max<uint64_t>(cycle, 1)), cursor_(0), count_(0) {
    // Reduce mod cycle, sort, dedup.
    offsets_.reserve(offsets.size());
    for (uint64_t o : offsets) offsets_.push_back(o % cycle_);
    std::sort(offsets_.begin(), offsets_.end());
    offsets_.erase(std::unique(offsets_.begin(), offsets_.end()), offsets_.end());
  }

  uint64_t count() const { return count_; }

  bool tick(uint64_t now) {
    if (now <= cursor_) {
      cursor_ = std::max(cursor_, now);
      return false;
    }
    uint64_t fires = matches_in(cursor_, now);
    cursor_ = now;
    if (fires == 0) return false;
    count_ += fires;
    return true;
  }

  std::optional<uint64_t> next_fire() const {
    if (offsets_.empty()) return std::nullopt;
    // Smallest m > cursor with m mod cycle ∈ offsets.
    uint64_t start = cursor_ + 1;
    uint64_t base = start / cycle_ * cycle_;
    for (uint64_t cyc = 0; cyc < 2; ++cyc) {
      uint64_t block = base + cyc * cycle_;
      for (uint64_t o : offsets_) {
        uint64_t cand = block + o;
        if (cand >= start) return cand;
      }
    }
    return std::nullopt;
  }

  bool operator==(const CronCore& o) const {
    return cycle_ == o.cycle_ && offsets_ == o.offsets_ &&
           cursor_ == o.cursor_ && count_ == o.count_;
  }
  bool operator!=(const CronCore& o) const { return !(*this == o); }

 private:
  uint64_t matches_in(uint64_t lo, uint64_t hi) const {
    uint64_t total = 0;
    for (uint64_t o : offsets_)
      total += cron_count_upto(hi, o, cycle_) - cron_count_upto(lo, o, cycle_);
    return total;
  }

  uint64_t cycle_;
  std::vector<uint64_t> offsets_;
  uint64_t cursor_;
  uint64_t count_;
};

/// Reactive cron source: same reactive contract as `IntervalCell`.
class CronCell {
 public:
  CronCell(Context& ctx, uint64_t cycle, std::vector<uint64_t> offsets)
      : core_(cycle, std::move(offsets)), count_(ctx.cell<uint64_t>(0)) {}

  bool tick(Context& ctx, uint64_t now) {
    bool edge = core_.tick(now);
    if (edge) ctx.set_cell(count_, core_.count());
    return edge;
  }

  uint64_t count(Context& ctx) const { return count_.get(ctx); }

  CellHandle<uint64_t> count_cell() const { return count_; }

  std::optional<uint64_t> next_fire() const { return core_.next_fire(); }

 private:
  CronCore core_;
  CellHandle<uint64_t> count_;
};

// ---------------------------------------------------------------------------
// Value + deadline
// ---------------------------------------------------------------------------

/// A value paired with a liveness state: `Live` until its deadline, then
/// `Expired` — the value is preserved across the flip. Models Rust's
/// `Deadlined<T>` enum.
template <typename T>
class Deadlined {
 public:
  static Deadlined<T> live(T value) { return Deadlined<T>(std::move(value), false); }
  static Deadlined<T> expired(T value) {
    return Deadlined<T>(std::move(value), true);
  }

  bool is_expired() const { return expired_; }
  const T& value() const { return value_; }
  T into_value() && { return std::move(value_); }

  bool operator==(const Deadlined<T>& o) const {
    return expired_ == o.expired_ && value_ == o.value_;
  }
  bool operator!=(const Deadlined<T>& o) const { return !(*this == o); }

 private:
  Deadlined(T value, bool expired)
      : value_(std::move(value)), expired_(expired) {}
  T value_;
  bool expired_;
};

/// Deadline compute core (bytes-eligible): a `TimerCore` over the deadline. The
/// value lives in the reactive cell.
class DeadlineCore {
 public:
  explicit DeadlineCore(uint64_t deadline) : timer_(deadline) {}

  bool is_expired() const { return timer_.fired(); }

  bool tick(uint64_t now) { return timer_.tick(now); }

  std::optional<uint64_t> next_fire() const { return timer_.next_fire(); }

  bool operator==(const DeadlineCore& o) const { return timer_ == o.timer_; }
  bool operator!=(const DeadlineCore& o) const { return !(*this == o); }

 private:
  TimerCore timer_;
};

/// Reactive value + deadline: flips `Live(v) → Expired(v)` at the deadline,
/// preserving the value; the `state` reader invalidates only on the expiry edge.
template <typename T>
class DeadlineCell {
 public:
  DeadlineCell(Context& ctx, T value, uint64_t deadline)
      : core_(deadline), value_(std::move(value)), expired_(ctx.cell(false)) {}

  /// Advance to logical time `now`; returns the expiry edge.
  bool tick(Context& ctx, uint64_t now) {
    bool edge = core_.tick(now);
    if (edge) ctx.set_cell(expired_, true);
    return edge;
  }

  /// The current state, cloning the preserved value (reactive read).
  Deadlined<T> state(Context& ctx) const {
    if (expired_.get(ctx)) return Deadlined<T>::expired(value_);
    return Deadlined<T>::live(value_);
  }

  bool is_expired(Context& ctx) const { return expired_.get(ctx); }

  CellHandle<bool> expired_cell() const { return expired_; }

  std::optional<uint64_t> next_fire() const { return core_.next_fire(); }

 private:
  DeadlineCore core_;
  T value_;
  CellHandle<bool> expired_;
};

}  // namespace lazily

#endif  // LAZILY_TEMPORAL_HPP
