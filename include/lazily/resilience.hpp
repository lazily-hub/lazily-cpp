#ifndef LAZILY_RESILIENCE_HPP
#define LAZILY_RESILIENCE_HPP

// Fault-tolerance primitives (`#lzresilience`).
//
// Port of `lazily-rs/src/resilience.rs` — see `lazily-spec/docs/resilience.md`
// and the formal model `lazily-formal/LazilyFormal/Resilience.lean`. Circuit
// breaker / retry / bulkhead / timeout, each a pure compute **core** (a state
// machine / counter over a monotone logical clock) split from a reactive
// **cell** projecting the salient reader onto a `CellHandle`. Cross-language
// conformance fixtures live in
// `lazily-spec/conformance/resilience/{circuit_breaker,retry,bulkhead,timeout}.json`.

#include <cstdint>
#include <deque>

#include <lazily/context.hpp>

namespace lazily {

// ===========================================================================
// Circuit breaker
// ===========================================================================

// Circuit-breaker state. Scoped enum — the built-in `==`/`!=` drive `set_cell`
// dedup on the projected `state` reader.
enum class BreakerState {
  // Calls pass; failures accumulate in the window.
  Closed,
  // Fast-fail until the reset deadline.
  Open,
  // Allow a single probe.
  HalfOpen,
};

// Circuit-breaker compute core: a sliding window of outcomes trips
// `Closed -> Open` at `failure_threshold`; `Open -> HalfOpen` at the deadline; a
// HalfOpen success closes, a failure re-opens.
class CircuitBreakerCore {
 public:
  CircuitBreakerCore(std::size_t window, std::size_t failure_threshold,
                     uint64_t reset_timeout)
      : window_(window < 1 ? 1 : window),
        failure_threshold_(failure_threshold < 1 ? 1 : failure_threshold),
        reset_timeout_(reset_timeout),
        state_(BreakerState::Closed),
        open_until_(0) {}

  BreakerState state() const { return state_; }

  // Whether a call is permitted; performs the `Open -> HalfOpen` transition at
  // the deadline.
  bool allow(uint64_t now) {
    switch (state_) {
      case BreakerState::Closed:
        return true;
      case BreakerState::Open:
        if (now >= open_until_) {
          state_ = BreakerState::HalfOpen;
          return true;
        }
        return false;
      case BreakerState::HalfOpen:
        return true;
    }
    return true;
  }

  // Feed a call outcome and drive the state machine.
  void record(bool success, uint64_t now) {
    switch (state_) {
      case BreakerState::HalfOpen:
        if (success) {
          state_ = BreakerState::Closed;
          outcomes_.clear();
        } else {
          state_ = BreakerState::Open;
          open_until_ = now + reset_timeout_;
        }
        break;
      case BreakerState::Closed:
        outcomes_.push_back(success);
        while (outcomes_.size() > window_) outcomes_.pop_front();
        if (failures() >= failure_threshold_) {
          state_ = BreakerState::Open;
          open_until_ = now + reset_timeout_;
        }
        break;
      case BreakerState::Open:
        break;
    }
  }

 private:
  std::size_t failures() const {
    std::size_t n = 0;
    for (bool s : outcomes_)
      if (!s) ++n;
    return n;
  }

  std::size_t window_;
  std::size_t failure_threshold_;
  uint64_t reset_timeout_;
  BreakerState state_;
  std::deque<bool> outcomes_;  // true = success
  uint64_t open_until_;
};

// Reactive circuit breaker: projects the `state` onto a `Cell`.
class CircuitBreakerCell {
 public:
  CircuitBreakerCell(Context& ctx, std::size_t window,
                     std::size_t failure_threshold, uint64_t reset_timeout)
      : core_(window, failure_threshold, reset_timeout),
        state_(ctx.cell(BreakerState::Closed)) {}

  bool allow(Context& ctx, uint64_t now) {
    bool r = core_.allow(now);
    refresh(ctx);
    return r;
  }

  void record(Context& ctx, bool success, uint64_t now) {
    core_.record(success, now);
    refresh(ctx);
  }

  BreakerState state() const { return core_.state(); }
  CellHandle<BreakerState> state_cell() const { return state_; }

 private:
  void refresh(Context& ctx) { ctx.set_cell(state_, core_.state()); }

  CircuitBreakerCore core_;
  CellHandle<BreakerState> state_;
};

// ===========================================================================
// Retry backoff
// ===========================================================================

// Exponential-backoff compute core: `delay(attempt) = min(cap, base * 2^attempt)`,
// saturating to `cap` on shift overflow.
class RetryPolicyCore {
 public:
  RetryPolicyCore(uint64_t base, uint64_t cap)
      : base_(base), cap_(cap), attempt_(0) {}

  // The delay for `attempt` (saturating at `cap`).
  uint64_t delay(uint32_t attempt) const {
    // Saturate to `cap` on shift overflow (>= 64-bit shift or value > cap).
    if (attempt >= 64) return cap_;
    uint64_t shifted = base_ << attempt;
    // Overflow guard: if the shift dropped bits, saturate.
    if ((shifted >> attempt) != base_) return cap_;
    return shifted < cap_ ? shifted : cap_;
  }

  // The current attempt's delay, then advance (saturating the attempt counter).
  uint64_t next_delay() {
    uint64_t d = delay(attempt_);
    if (attempt_ != UINT32_MAX) ++attempt_;
    return d;
  }

  void reset() { attempt_ = 0; }

 private:
  uint64_t base_;
  uint64_t cap_;
  uint32_t attempt_;
};

// Reactive retry policy: projects the current delay onto a `Cell`.
class RetryPolicyCell {
 public:
  RetryPolicyCell(Context& ctx, uint64_t base, uint64_t cap)
      : core_(base, cap), delay_(ctx.cell(uint64_t(0))) {}

  uint64_t next_delay(Context& ctx) {
    uint64_t d = core_.next_delay();
    ctx.set_cell(delay_, d);
    return d;
  }

  void reset(Context& ctx) {
    core_.reset();
    ctx.set_cell(delay_, uint64_t(0));
  }

  uint64_t delay(Context& ctx) { return ctx.get_cell(delay_); }
  CellHandle<uint64_t> delay_cell() const { return delay_; }

 private:
  RetryPolicyCore core_;
  CellHandle<uint64_t> delay_;
};

// ===========================================================================
// Bulkhead
// ===========================================================================

// Bounded isolation-pool compute core.
class BulkheadCore {
 public:
  explicit BulkheadCore(uint64_t capacity) : capacity_(capacity), in_use_(0) {}

  uint64_t in_use() const { return in_use_; }

  bool acquire() {
    if (in_use_ < capacity_) {
      ++in_use_;
      return true;
    }
    return false;
  }

  void release() {
    if (in_use_ > 0) --in_use_;
  }

 private:
  uint64_t capacity_;
  uint64_t in_use_;
};

// Reactive bulkhead: projects `permits_in_use` onto a `Cell`.
class BulkheadCell {
 public:
  BulkheadCell(Context& ctx, uint64_t capacity)
      : core_(capacity), in_use_(ctx.cell(uint64_t(0))) {}

  bool acquire(Context& ctx) {
    bool r = core_.acquire();
    refresh(ctx);
    return r;
  }

  void release(Context& ctx) {
    core_.release();
    refresh(ctx);
  }

  uint64_t permits_in_use(Context& ctx) { return ctx.get_cell(in_use_); }
  CellHandle<uint64_t> permits_in_use_cell() const { return in_use_; }

 private:
  void refresh(Context& ctx) { ctx.set_cell(in_use_, core_.in_use()); }

  BulkheadCore core_;
  CellHandle<uint64_t> in_use_;
};

// ===========================================================================
// Timeout
// ===========================================================================

// Deadline-bounded call compute core.
class TimeoutCore {
 public:
  TimeoutCore() : deadline_(0), armed_(false), timed_out_(false) {}

  // Arm the timeout with `deadline = now + timeout`.
  void arm(uint64_t now, uint64_t timeout) {
    deadline_ = now + timeout;
    armed_ = true;
    timed_out_ = false;
  }

  // Fast-fail when `now >= deadline`; returns the timeout edge (once).
  bool tick(uint64_t now) {
    if (armed_ && !timed_out_ && now >= deadline_) {
      timed_out_ = true;
      return true;
    }
    return false;
  }

  bool is_timed_out() const { return timed_out_; }

 private:
  uint64_t deadline_;
  bool armed_;
  bool timed_out_;
};

// Reactive timeout: projects `is_timed_out` onto a `Cell`.
class TimeoutCell {
 public:
  explicit TimeoutCell(Context& ctx) : timed_out_(ctx.cell(false)) {}

  void arm(Context& ctx, uint64_t now, uint64_t timeout) {
    core_.arm(now, timeout);
    refresh(ctx);
  }

  bool tick(Context& ctx, uint64_t now) {
    bool r = core_.tick(now);
    refresh(ctx);
    return r;
  }

  bool is_timed_out(Context& ctx) { return ctx.get_cell(timed_out_); }
  CellHandle<bool> is_timed_out_cell() const { return timed_out_; }

 private:
  void refresh(Context& ctx) { ctx.set_cell(timed_out_, core_.is_timed_out()); }

  TimeoutCore core_;
  CellHandle<bool> timed_out_;
};

}  // namespace lazily

#endif  // LAZILY_RESILIENCE_HPP
