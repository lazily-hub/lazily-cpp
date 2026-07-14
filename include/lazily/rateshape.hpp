#ifndef LAZILY_RATESHAPE_HPP
#define LAZILY_RATESHAPE_HPP

// Rate-shaping source operators (`#lzrateshape`).
//
// Port of `lazily-rs/src/rateshape.rs`. Debounce / throttle / time-sampling
// already exist algorithmically inside the relay plane (WindowPolicy /
// ExpiryPolicy / RatePolicy in relay/relay_policy); this header is the
// standalone home of the four **source operators** so any reactive source can be
// rate-shaped, not just a relay egress. Window/Expiry/Rate policies are NOT
// re-added here — only the new operators the conformance fixtures cover.
//
// Each operator is a pure compute **core** — the emit/drop decision over plain
// state — split from a thin reactive **cell** that projects the emitted value
// onto a `CellHandle<std::optional<T>>` so a dropped/held input never
// invalidates dependents. Time is a monotone logical clock (as in `#lztime`).
//
// See `lazily-spec/conformance/rateshape/*.json`.

#include <cstdint>
#include <optional>

#include <lazily/context.hpp>

namespace lazily {

// The reactive-cell projection shared by every operator: publish the last
// emitted value onto a `CellHandle<std::optional<T>>`. A dropped/held input
// (empty `emitted`) never touches the cell, so dependents survive. `set_cell`
// dedups, so an emit that repeats the current value is a no-op invalidation.
template <typename T>
inline void rateshape_set_output(Context& ctx,
                                 const CellHandle<std::optional<T>>& cell,
                                 const std::optional<T>& emitted) {
  if (emitted) ctx.set_cell(cell, std::optional<T>(*emitted));
}

// ── Debounce ────────────────────────────────────────────────────────────────

// Debounce compute core: coalesce inputs (KeepLatest) and emit the latest value
// only after `quiet` ticks with no new input — every input resets the deadline.
template <typename T>
class DebounceCore {
 public:
  explicit DebounceCore(uint64_t quiet) : quiet_(quiet) {}

  // Record an input; resets the quiet deadline to `now + quiet`.
  void input(uint64_t now, T v) {
    pending_ = std::move(v);
    fire_at_ = now + quiet_;
    armed_ = true;
  }

  // Advance; emits the latest value once the quiet period has elapsed.
  std::optional<T> tick(uint64_t now) {
    if (armed_ && pending_ && fire_at_ <= now) {
      armed_ = false;
      std::optional<T> out = std::move(pending_);
      pending_.reset();
      return out;
    }
    return std::nullopt;
  }

 private:
  uint64_t quiet_;
  std::optional<T> pending_{};
  uint64_t fire_at_ = 0;
  bool armed_ = false;
};

// Reactive debounce over any reactive source.
template <typename T>
class DebounceCell {
 public:
  DebounceCell(Context& ctx, uint64_t quiet)
      : core_(quiet), output_(ctx.cell(std::optional<T>{})) {}

  void input(Context&, uint64_t now, T v) { core_.input(now, std::move(v)); }

  std::optional<T> tick(Context& ctx, uint64_t now) {
    auto emitted = core_.tick(now);
    rateshape_set_output(ctx, output_, emitted);
    return emitted;
  }

  std::optional<T> output(Context& ctx) const { return output_.get(ctx); }
  CellHandle<std::optional<T>> output_cell() const { return output_; }

 private:
  DebounceCore<T> core_;
  CellHandle<std::optional<T>> output_;
};

// ── Throttle ──────────────────────────────────────────────────────────────

// Which edge of the window a `ThrottleCore` emits on.
enum class ThrottleEdge {
  // First input of a window passes immediately; the rest are dropped.
  Leading,
  // First input opens the window; the latest is emitted at the window boundary.
  Trailing,
};

// Throttle compute core: at most one emit per `window`.
template <typename T>
class ThrottleCore {
 public:
  ThrottleCore(ThrottleEdge edge, uint64_t window)
      : edge_(edge), window_(window) {}

  // Record an input. Leading emits (or drops); Trailing coalesces and holds.
  std::optional<T> input(uint64_t now, T v) {
    if (edge_ == ThrottleEdge::Leading) {
      if (window_end_ && now < *window_end_) return std::nullopt;
      window_end_ = now + window_;
      return std::optional<T>(std::move(v));
    }
    // Trailing.
    if (!window_start_) window_start_ = now;
    pending_ = std::move(v);
    return std::nullopt;
  }

  // Advance. Trailing emits the coalesced latest at the window boundary.
  std::optional<T> tick(uint64_t now) {
    if (edge_ == ThrottleEdge::Leading) return std::nullopt;
    if (!window_start_) return std::nullopt;
    if (now >= *window_start_ + window_ && pending_) {
      window_start_.reset();
      std::optional<T> out = std::move(pending_);
      pending_.reset();
      return out;
    }
    return std::nullopt;
  }

 private:
  ThrottleEdge edge_;
  uint64_t window_;
  std::optional<uint64_t> window_end_{};    // Leading.
  std::optional<uint64_t> window_start_{};  // Trailing.
  std::optional<T> pending_{};
};

// Reactive throttle over any reactive source.
template <typename T>
class ThrottleCell {
 public:
  ThrottleCell(Context& ctx, ThrottleEdge edge, uint64_t window)
      : core_(edge, window), output_(ctx.cell(std::optional<T>{})) {}

  std::optional<T> input(Context& ctx, uint64_t now, T v) {
    auto emitted = core_.input(now, std::move(v));
    rateshape_set_output(ctx, output_, emitted);
    return emitted;
  }

  std::optional<T> tick(Context& ctx, uint64_t now) {
    auto emitted = core_.tick(now);
    rateshape_set_output(ctx, output_, emitted);
    return emitted;
  }

  std::optional<T> output(Context& ctx) const { return output_.get(ctx); }
  CellHandle<std::optional<T>> output_cell() const { return output_; }

 private:
  ThrottleCore<T> core_;
  CellHandle<std::optional<T>> output_;
};

// ── Sample ──────────────────────────────────────────────────────────────────

// Sampling mode kind for `SampleMode`.
enum class SampleKind {
  // Emit every `n`-th input (count-based).
  Count,
  // Emit the held latest at each `period` boundary (time-based).
  Time,
};

// Deterministic sampling mode — Count(n) or Time(period).
struct SampleMode {
  SampleKind kind;
  uint64_t value;
  static SampleMode Count(uint64_t n) { return {SampleKind::Count, n}; }
  static SampleMode Time(uint64_t period) { return {SampleKind::Time, period}; }
};

// Deterministic sampling compute core.
template <typename T>
class SampleCore {
 public:
  explicit SampleCore(SampleMode mode) : mode_(mode) {
    next_ = (mode.kind == SampleKind::Time) ? (mode.value < 1 ? 1 : mode.value)
                                            : 0;
  }

  // Record an input. Count mode emits on every `n`-th; Time mode holds the
  // latest for the next boundary.
  std::optional<T> input(T v) {
    if (mode_.kind == SampleKind::Count) {
      uint64_t n = mode_.value < 1 ? 1 : mode_.value;
      counter_ += 1;
      if (counter_ % n == 0) return std::optional<T>(std::move(v));
      return std::nullopt;
    }
    // Time: hold the latest.
    held_ = std::move(v);
    return std::nullopt;
  }

  // Advance. Time mode emits the held latest once per period boundary crossed.
  std::optional<T> tick(uint64_t now) {
    if (mode_.kind == SampleKind::Count) return std::nullopt;
    uint64_t period = mode_.value < 1 ? 1 : mode_.value;
    if (now < next_) return std::nullopt;
    uint64_t fires = (now - next_) / period + 1;
    next_ += fires * period;
    // Emit the held latest; it persists (sampling the current value).
    return held_;
  }

 private:
  SampleMode mode_;
  uint64_t counter_ = 0;
  uint64_t next_ = 0;
  std::optional<T> held_{};
};

// Reactive sampler over any reactive source.
template <typename T>
class SampleCell {
 public:
  SampleCell(Context& ctx, SampleMode mode)
      : core_(mode), output_(ctx.cell(std::optional<T>{})) {}

  std::optional<T> input(Context& ctx, T v) {
    auto emitted = core_.input(std::move(v));
    rateshape_set_output(ctx, output_, emitted);
    return emitted;
  }

  std::optional<T> tick(Context& ctx, uint64_t now) {
    auto emitted = core_.tick(now);
    rateshape_set_output(ctx, output_, emitted);
    return emitted;
  }

  std::optional<T> output(Context& ctx) const { return output_.get(ctx); }
  CellHandle<std::optional<T>> output_cell() const { return output_; }

 private:
  SampleCore<T> core_;
  CellHandle<std::optional<T>> output_;
};

// ── Probabilistic sample ────────────────────────────────────────────────────

// An injectable RNG so probabilistic sampling is deterministic under a fixed
// seed. `next_f64` yields a draw in `[0, 1)`.
class SampleRng {
 public:
  virtual ~SampleRng() = default;
  virtual double next_f64() = 0;
};

// A small deterministic SplitMix64 LCG — no external dependency, reproducible
// for the distribution property test. Bit-identical to `Lcg` in rateshape.rs.
class Lcg : public SampleRng {
 public:
  explicit Lcg(uint64_t seed) : state_(seed) {}

  double next_f64() override {
    // SplitMix64.
    state_ += 0x9E3779B97F4A7C15ULL;
    uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= z >> 31;
    // 53-bit mantissa → [0, 1).
    return static_cast<double>(z >> 11) /
           static_cast<double>(uint64_t(1) << 53);
  }

 private:
  uint64_t state_;
};

// Probabilistic (tail) sampling compute core — the plan's only new algorithm.
// A draw in `[0, 1)` passes iff `draw < rate`.
class ProbabilisticSampleCore {
 public:
  explicit ProbabilisticSampleCore(double rate)
      : rate_(rate < 0.0 ? 0.0 : (rate > 1.0 ? 1.0 : rate)) {}

  double rate() const { return rate_; }

  // Whether an input with this random `draw` is sampled.
  bool decide(double draw) const { return draw < rate_; }

 private:
  double rate_;
};

// Reactive probabilistic sampler; owns an injectable `SampleRng` (`R`).
template <typename T, typename R>
class ProbabilisticSampleCell {
 public:
  ProbabilisticSampleCell(Context& ctx, double rate, R rng)
      : core_(rate), rng_(std::move(rng)),
        output_(ctx.cell(std::optional<T>{})) {}

  // Sample an input using the owned RNG.
  std::optional<T> input(Context& ctx, T v) {
    double draw = rng_.next_f64();
    return input_with_draw(ctx, std::move(v), draw);
  }

  // Sample an input against an explicit `draw` (deterministic / conformance).
  std::optional<T> input_with_draw(Context& ctx, T v, double draw) {
    if (core_.decide(draw)) {
      ctx.set_cell(output_, std::optional<T>(v));
      return std::optional<T>(std::move(v));
    }
    return std::nullopt;
  }

  std::optional<T> output(Context& ctx) const { return output_.get(ctx); }
  CellHandle<std::optional<T>> output_cell() const { return output_; }

 private:
  ProbabilisticSampleCore core_;
  R rng_;
  CellHandle<std::optional<T>> output_;
};

}  // namespace lazily

#endif  // LAZILY_RATESHAPE_HPP
