#ifndef LAZILY_STDLIB_HPP
#define LAZILY_STDLIB_HPP

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace lazily {

enum class TimerError { deadline_overflow, clock_regression };

struct CheckedDeadline {
  bool ok = false;
  std::uint64_t value = 0;
  TimerError error = TimerError::deadline_overflow;
};

inline CheckedDeadline checked_deadline(std::uint64_t now, std::uint64_t duration) {
  if (duration > std::numeric_limits<std::uint64_t>::max() - now)
    return {false, 0, TimerError::deadline_overflow};
  return {true, now + duration, TimerError::deadline_overflow};
}

struct TimerObservation {
  enum class Outcome { pending, fired, unavailable } outcome = Outcome::pending;
  std::uint64_t deadline = 0;
  std::uint64_t fired_at = 0;
  std::optional<TimerError> error;
};

class Timer {
public:
  static std::pair<std::unique_ptr<Timer>, std::optional<TimerError>>
  start(std::uint64_t now, std::uint64_t duration) {
    const auto deadline = checked_deadline(now, duration);
    if (!deadline.ok) return {nullptr, deadline.error};
    return {std::unique_ptr<Timer>(new Timer(now, deadline.value)), std::nullopt};
  }

  TimerObservation observe(std::uint64_t now) {
    std::lock_guard<std::mutex> lock(mu_);
    if (fired_) return {TimerObservation::Outcome::fired, 0, fired_at_, std::nullopt};
    if (now < last_now_)
      return {TimerObservation::Outcome::unavailable, deadline_, 0, TimerError::clock_regression};
    last_now_ = now;
    if (now >= deadline_) {
      fired_ = true;
      fired_at_ = now;
      return {TimerObservation::Outcome::fired, 0, now, std::nullopt};
    }
    return {TimerObservation::Outcome::pending, deadline_, 0, std::nullopt};
  }

private:
  Timer(std::uint64_t now, std::uint64_t deadline) : deadline_(deadline), last_now_(now) {}

  std::mutex mu_;
  std::uint64_t deadline_;
  std::uint64_t last_now_;
  std::uint64_t fired_at_ = 0;
  bool fired_ = false;
};

enum class TimeoutCancellation { pending, cancelled, unavailable };
enum class TimeoutOperationState { pending, completed, unavailable };

template <typename T> struct TimeoutOperation {
  TimeoutOperationState state = TimeoutOperationState::pending;
  T value{};

  static TimeoutOperation pending() { return {}; }
  static TimeoutOperation completed(T value) {
    return {TimeoutOperationState::completed, std::move(value)};
  }
  static TimeoutOperation unavailable() { return {TimeoutOperationState::unavailable, {}}; }
};

template <typename T> struct TimeoutObservation {
  enum class Outcome { pending, completed, timed_out, cancelled, unavailable };
  Outcome outcome = Outcome::pending;
  std::uint64_t deadline = 0;
  T value{};
  std::string reason;
};

template <typename T> class Timeout {
public:
  using Observation = TimeoutObservation<T>;

  static std::pair<std::unique_ptr<Timeout>, std::optional<TimerError>>
  start(std::uint64_t now, std::uint64_t duration) {
    const auto deadline = checked_deadline(now, duration);
    if (!deadline.ok) return {nullptr, deadline.error};
    return {std::unique_ptr<Timeout>(new Timeout(now, deadline.value)), std::nullopt};
  }

  template <typename Operation, typename Cancellation>
  Observation poll(std::uint64_t now, Operation operation, Cancellation cancellation) {
    std::lock_guard<std::mutex> lock(mu_);
    if (terminal_) return *terminal_;
    if (now < last_now_)
      return latch({Observation::Outcome::unavailable, 0, {}, "clock_regression"});
    last_now_ = now;
    if (now >= deadline_) return latch({Observation::Outcome::timed_out, 0, {}, {}});

    auto op = operation();
    const auto cancel = cancellation();
    if (op.state == TimeoutOperationState::completed)
      return latch({Observation::Outcome::completed, 0, std::move(op.value), {}});
    if (op.state == TimeoutOperationState::unavailable)
      return latch({Observation::Outcome::unavailable, 0, {}, "operation_unavailable"});
    if (cancel == TimeoutCancellation::cancelled)
      return latch({Observation::Outcome::cancelled, 0, {}, {}});
    if (cancel == TimeoutCancellation::unavailable)
      return latch({Observation::Outcome::unavailable, 0, {}, "cancellation_unavailable"});
    return {Observation::Outcome::pending, deadline_, {}, {}};
  }

private:
  Timeout(std::uint64_t now, std::uint64_t deadline) : deadline_(deadline), last_now_(now) {}

  Observation latch(Observation observation) {
    terminal_ = observation;
    return observation;
  }

  std::mutex mu_;
  std::uint64_t deadline_;
  std::uint64_t last_now_;
  std::optional<Observation> terminal_;
};

struct RevisionBarrierObservation {
  enum class Outcome {
    pending,
    satisfied,
    timed_out,
    cancelled,
    disposed,
    unavailable
  } outcome = Outcome::pending;
  std::string reason;
  std::uint64_t revision = 0;
  std::uint64_t generation = 0;
};

class RevisionBarrier {
public:
  RevisionBarrier(std::uint64_t revision, std::uint64_t required_revision,
                  std::optional<std::uint64_t> deadline)
      : revision_(revision), required_revision_(required_revision), deadline_(deadline) {}

  template <typename Cancellation>
  RevisionBarrierObservation observe(std::uint64_t now, bool predicate, Cancellation cancellation) {
    std::unique_lock<std::mutex> lock(mu_);
    if (terminal_) return snapshot();
    if (!accept_now(now))
      return latch(RevisionBarrierObservation::Outcome::unavailable, "clock_regression");
    if (deadline_ && now >= *deadline_)
      return latch(RevisionBarrierObservation::Outcome::timed_out);
    if (predicate && revision_ >= required_revision_)
      return latch(RevisionBarrierObservation::Outcome::satisfied);
    lock.unlock();
    const auto cancellation_state = cancellation();
    lock.lock();
    if (terminal_) return snapshot();
    switch (cancellation_state) {
    case TimeoutCancellation::cancelled:
      return latch(RevisionBarrierObservation::Outcome::cancelled);
    case TimeoutCancellation::unavailable:
      return latch(RevisionBarrierObservation::Outcome::unavailable, "cancellation_unavailable");
    case TimeoutCancellation::pending:
      return snapshot();
    }
    return snapshot();
  }

  RevisionBarrierObservation register_recheck(std::uint64_t now, std::uint64_t observed_revision,
                                              bool predicate) {
    std::lock_guard<std::mutex> lock(mu_);
    if (terminal_) return snapshot();
    if (!accept_now(now))
      return latch(RevisionBarrierObservation::Outcome::unavailable, "clock_regression");
    if (deadline_ && now >= *deadline_)
      return latch(RevisionBarrierObservation::Outcome::timed_out);
    accept_revision(observed_revision);
    if (predicate && revision_ >= required_revision_)
      return latch(RevisionBarrierObservation::Outcome::satisfied);
    return snapshot();
  }

  RevisionBarrierObservation advance(std::uint64_t revision, bool predicate) {
    std::lock_guard<std::mutex> lock(mu_);
    if (terminal_) return snapshot();
    accept_revision(revision);
    if (predicate && revision_ >= required_revision_)
      return latch(RevisionBarrierObservation::Outcome::satisfied);
    return snapshot();
  }

  RevisionBarrierObservation dispose() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!terminal_) return latch(RevisionBarrierObservation::Outcome::disposed);
    return snapshot();
  }

  RevisionBarrierObservation receipt(const std::string&) {
    std::lock_guard<std::mutex> lock(mu_);
    return snapshot();
  }

private:
  bool accept_now(std::uint64_t now) {
    if (last_now_ && now < *last_now_) return false;
    last_now_ = now;
    return true;
  }

  void accept_revision(std::uint64_t revision) {
    if (revision > revision_) {
      revision_ = revision;
      ++generation_;
    }
  }

  RevisionBarrierObservation latch(RevisionBarrierObservation::Outcome outcome,
                                   std::string reason = {}) {
    terminal_ = outcome;
    terminal_reason_ = std::move(reason);
    return snapshot();
  }

  RevisionBarrierObservation snapshot() const {
    return {terminal_.value_or(RevisionBarrierObservation::Outcome::pending), terminal_reason_,
            revision_, generation_};
  }

  std::mutex mu_;
  std::uint64_t revision_;
  std::uint64_t required_revision_;
  std::uint64_t generation_ = 0;
  std::optional<std::uint64_t> deadline_;
  std::optional<std::uint64_t> last_now_;
  std::optional<RevisionBarrierObservation::Outcome> terminal_;
  std::string terminal_reason_;
};

} // namespace lazily

#endif // LAZILY_STDLIB_HPP
