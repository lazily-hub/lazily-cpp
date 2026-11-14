#ifndef LAZILY_EGRESS_CORE_HPP
#define LAZILY_EGRESS_CORE_HPP

// Transport-agnostic reactive egress core (`#lzegress`).
//
// Spec: ../lazily-spec/docs/durable-sinks.md and the four `egress_*.json`
// fixtures. Reference implementation: lazily-rs `src/egress_core.rs`.
//
// The core owns delivery authority: sequence assignment, the unacknowledged
// window, a monotone acknowledgement watermark, the retry budget, and the
// producer-generation fence. It owns no graph handles and performs no I/O, so
// one core serves the single-threaded, thread-safe and async shells in
// `egress.hpp` unchanged. `DurableOutbox`/`SpillStore` remain the persistence
// layer; `RelayCell` remains the conflation/backpressure layer.
//
// Every mutator returns an `EgressChange` -- WHICH reader kinds the transition
// dirtied -- and each shell clears exactly that set on its own graph. That
// return value is the whole contract between the core and a shell.
//
// Wasm tier: core. The closure is the C++17 standard library only -- no
// threads, no files, no shared memory -- so this header is includable from a
// single-threaded wasm32 build exactly like `core.hpp`.

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace lazily {

/// Egress policy. `retry_budget` counts retries AFTER the first attempt.
struct EgressPolicy {
  std::size_t inflight_limit = 64;
  std::uint32_t retry_budget = 3;
  std::uint64_t retry_base = 10;
  std::uint64_t retry_ceiling = 1000;

  bool operator==(const EgressPolicy& o) const {
    return inflight_limit == o.inflight_limit && retry_budget == o.retry_budget &&
           retry_base == o.retry_base && retry_ceiling == o.retry_ceiling;
  }
  bool operator!=(const EgressPolicy& o) const { return !(*this == o); }
};

/// Why a policy was refused at construction time.
enum class EgressConfigError {
  ZeroInflightLimit,
  ZeroRetryBase,
  RetryCeilingBelowBase,
};

inline const char* egress_config_error_name(EgressConfigError error) {
  switch (error) {
  case EgressConfigError::ZeroInflightLimit:
    return "ZeroInflightLimit";
  case EgressConfigError::ZeroRetryBase:
    return "ZeroRetryBase";
  case EgressConfigError::RetryCeilingBelowBase:
    return "RetryCeilingBelowBase";
  }
  return "EgressConfigError";
}

/// Construction-time policy refusal, mirroring `IngressConfigException`.
struct EgressConfigException : std::invalid_argument {
  EgressConfigError error;
  explicit EgressConfigException(EgressConfigError e)
      : std::invalid_argument(egress_config_error_name(e)), error(e) {}
};

/// One send attempt. `(generation, sequence)` is the transport idempotency
/// identity.
template <typename T> struct EgressEnvelope {
  std::uint64_t generation = 0;
  std::uint64_t sequence = 0;
  std::uint32_t attempt = 0;
  T payload{};

  bool operator==(const EgressEnvelope& o) const {
    return generation == o.generation && sequence == o.sequence && attempt == o.attempt &&
           payload == o.payload;
  }
  bool operator!=(const EgressEnvelope& o) const { return !(*this == o); }
};

/// The parked-failure projection: the most recent failed send and its derived
/// backoff.
struct EgressRetry {
  std::uint64_t sequence = 0;
  std::uint32_t attempt = 0;
  std::uint64_t backoff = 0;
  bool exhausted = false;

  bool operator==(const EgressRetry& o) const {
    return sequence == o.sequence && attempt == o.attempt && backoff == o.backoff &&
           exhausted == o.exhausted;
  }
  bool operator!=(const EgressRetry& o) const { return !(*this == o); }
};

// -- Outcomes. Tagged records rather than std::variant, so a replay compares
//    them field-for-field; fields a case does not carry stay at zero. --

enum class EgressClaimKind { Claimed, Empty, WindowFull, StaleGeneration };

template <typename T> struct EgressClaim {
  EgressClaimKind kind = EgressClaimKind::Empty;
  std::optional<EgressEnvelope<T>> envelope;
  std::uint64_t current = 0;

  static EgressClaim claimed(EgressEnvelope<T> e) {
    EgressClaim c;
    c.kind = EgressClaimKind::Claimed;
    c.envelope = std::move(e);
    return c;
  }
  static EgressClaim empty() { return EgressClaim{}; }
  static EgressClaim window_full() {
    EgressClaim c;
    c.kind = EgressClaimKind::WindowFull;
    return c;
  }
  static EgressClaim stale_generation(std::uint64_t current) {
    EgressClaim c;
    c.kind = EgressClaimKind::StaleGeneration;
    c.current = current;
    return c;
  }
  bool operator==(const EgressClaim& o) const {
    return kind == o.kind && envelope == o.envelope && current == o.current;
  }
  bool operator!=(const EgressClaim& o) const { return !(*this == o); }
};

enum class EgressAckKind { Advanced, Unchanged, StaleGeneration, BeyondSent };

struct EgressAck {
  EgressAckKind kind = EgressAckKind::Unchanged;
  /// `Advanced`: the new watermark. `Unchanged`: the retained watermark.
  std::optional<std::uint64_t> through;
  std::uint64_t current = 0;
  /// `BeyondSent`: the first sequence never assigned. An ACK at or past it
  /// names a record that was never sent and is refused.
  std::uint64_t next_sequence = 0;

  static EgressAck advanced(std::uint64_t through) {
    EgressAck a;
    a.kind = EgressAckKind::Advanced;
    a.through = through;
    return a;
  }
  static EgressAck unchanged(std::optional<std::uint64_t> through) {
    EgressAck a;
    a.kind = EgressAckKind::Unchanged;
    a.through = through;
    return a;
  }
  static EgressAck stale_generation(std::uint64_t current) {
    EgressAck a;
    a.kind = EgressAckKind::StaleGeneration;
    a.current = current;
    return a;
  }
  static EgressAck beyond_sent(std::uint64_t next_sequence) {
    EgressAck a;
    a.kind = EgressAckKind::BeyondSent;
    a.next_sequence = next_sequence;
    return a;
  }
  bool operator==(const EgressAck& o) const {
    return kind == o.kind && through == o.through && current == o.current &&
           next_sequence == o.next_sequence;
  }
  bool operator!=(const EgressAck& o) const { return !(*this == o); }
};

enum class EgressFailureKind { Retrying, Exhausted, UnknownSequence, StaleGeneration };

struct EgressFailure {
  EgressFailureKind kind = EgressFailureKind::UnknownSequence;
  std::optional<EgressRetry> retry;
  std::uint64_t current = 0;

  static EgressFailure retrying(EgressRetry r) {
    EgressFailure f;
    f.kind = EgressFailureKind::Retrying;
    f.retry = r;
    return f;
  }
  static EgressFailure exhausted(EgressRetry r) {
    EgressFailure f;
    f.kind = EgressFailureKind::Exhausted;
    f.retry = r;
    return f;
  }
  static EgressFailure unknown_sequence() { return EgressFailure{}; }
  static EgressFailure stale_generation(std::uint64_t current) {
    EgressFailure f;
    f.kind = EgressFailureKind::StaleGeneration;
    f.current = current;
    return f;
  }
  bool operator==(const EgressFailure& o) const {
    return kind == o.kind && retry == o.retry && current == o.current;
  }
  bool operator!=(const EgressFailure& o) const { return !(*this == o); }
};

enum class EgressRetryActionKind { Scheduled, UnknownSequence, StaleGeneration };

struct EgressRetryAction {
  EgressRetryActionKind kind = EgressRetryActionKind::UnknownSequence;
  std::uint64_t sequence = 0;
  std::uint64_t current = 0;

  static EgressRetryAction scheduled(std::uint64_t sequence) {
    EgressRetryAction a;
    a.kind = EgressRetryActionKind::Scheduled;
    a.sequence = sequence;
    return a;
  }
  static EgressRetryAction unknown_sequence() { return EgressRetryAction{}; }
  static EgressRetryAction stale_generation(std::uint64_t current) {
    EgressRetryAction a;
    a.kind = EgressRetryActionKind::StaleGeneration;
    a.current = current;
    return a;
  }
  bool operator==(const EgressRetryAction& o) const {
    return kind == o.kind && sequence == o.sequence && current == o.current;
  }
  bool operator!=(const EgressRetryAction& o) const { return !(*this == o); }
};

enum class EgressReconnectKind { Advanced, Unchanged, StaleGeneration };

struct EgressReconnect {
  EgressReconnectKind kind = EgressReconnectKind::Unchanged;
  /// `Advanced` / `Unchanged`: the generation now in force.
  std::uint64_t generation = 0;
  /// `Advanced`: how many unacknowledged records were replayed.
  std::size_t replayed = 0;
  std::uint64_t current = 0;

  static EgressReconnect advanced(std::uint64_t generation, std::size_t replayed) {
    EgressReconnect r;
    r.kind = EgressReconnectKind::Advanced;
    r.generation = generation;
    r.replayed = replayed;
    return r;
  }
  static EgressReconnect unchanged(std::uint64_t generation) {
    EgressReconnect r;
    r.kind = EgressReconnectKind::Unchanged;
    r.generation = generation;
    return r;
  }
  static EgressReconnect stale_generation(std::uint64_t current) {
    EgressReconnect r;
    r.kind = EgressReconnectKind::StaleGeneration;
    r.current = current;
    return r;
  }
  bool operator==(const EgressReconnect& o) const {
    return kind == o.kind && generation == o.generation && replayed == o.replayed &&
           current == o.current;
  }
  bool operator!=(const EgressReconnect& o) const { return !(*this == o); }
};

/// Reader-kind invalidations emitted by one transition.
struct EgressChange {
  bool pending = false;
  bool inflight = false;
  bool acked_through = false;
  bool retry = false;

  bool empty() const { return !pending && !inflight && !acked_through && !retry; }
  bool operator==(const EgressChange& o) const {
    return pending == o.pending && inflight == o.inflight && acked_through == o.acked_through &&
           retry == o.retry;
  }
  bool operator!=(const EgressChange& o) const { return !(*this == o); }
};

/// Pure egress delivery authority.
template <typename T> class EgressCore {
public:
  using value_type = T;

  /// Throws `EgressConfigException` for a policy that bounds nothing.
  EgressCore(std::uint64_t generation, EgressPolicy policy)
      : policy_(validate(policy)), generation_(generation) {}

  const EgressPolicy& policy() const { return policy_; }
  std::uint64_t generation() const { return generation_; }
  std::uint64_t next_sequence() const { return next_sequence_; }
  std::optional<std::uint64_t> acked_through() const { return acked_through_; }
  std::optional<EgressRetry> retry() const { return retry_; }

  std::vector<EgressEnvelope<T>> pending() const {
    std::vector<EgressEnvelope<T>> out;
    out.reserve(pending_.size());
    for (const auto& record : pending_)
      out.push_back(record);
    return out;
  }

  std::vector<EgressEnvelope<T>> inflight() const {
    std::vector<EgressEnvelope<T>> out;
    out.reserve(inflight_.size());
    for (const auto& entry : inflight_)
      out.push_back(entry.second);
    return out;
  }

  /// Assign the next sequence and place the value in the pending projection.
  std::pair<EgressChange, std::uint64_t> enqueue(T payload) {
    const std::uint64_t sequence = next_sequence_;
    if (next_sequence_ != UINT64_MAX) ++next_sequence_;
    pending_.push_back(EgressEnvelope<T>{generation_, sequence, 0, std::move(payload)});
    EgressChange change;
    change.pending = true;
    return {change, sequence};
  }

  /// Move one pending value into the unacknowledged window. The caller supplies
  /// the generation captured by its transport attachment; an attachment from a
  /// superseded incarnation cannot claim or replay.
  std::pair<EgressChange, EgressClaim<T>> claim(std::uint64_t generation) {
    if (generation != generation_) return {{}, EgressClaim<T>::stale_generation(generation_)};
    if (inflight_.size() >= policy_.inflight_limit) return {{}, EgressClaim<T>::window_full()};
    if (pending_.empty()) return {{}, EgressClaim<T>::empty()};
    EgressEnvelope<T> record = std::move(pending_.front());
    pending_.pop_front();
    record.generation = generation_;
    if (record.attempt != UINT32_MAX) ++record.attempt;
    const EgressEnvelope<T> envelope = record;
    inflight_.emplace(record.sequence, std::move(record));
    const bool retry_changed = retry_.has_value() && retry_->sequence == envelope.sequence;
    if (retry_changed) retry_.reset();
    EgressChange change;
    change.pending = true;
    change.inflight = true;
    change.retry = retry_changed;
    return {change, EgressClaim<T>::claimed(envelope)};
  }

  /// Advance the cumulative acknowledgement watermark and prune every record at
  /// or below it. A `through` at or past `next_sequence` is refused as
  /// `BeyondSent` and changes nothing.
  std::pair<EgressChange, EgressAck> ack(std::uint64_t generation, std::uint64_t through) {
    if (generation != generation_) return {{}, EgressAck::stale_generation(generation_)};
    // An ACK may only cover sequences this core has assigned. One at or past
    // `next_sequence` acknowledges records that were never sent: refuse it
    // before the watermark moves, so it can neither advance the frontier nor
    // prune records enqueued later under those sequence numbers.
    if (through >= next_sequence_) return {{}, EgressAck::beyond_sent(next_sequence_)};
    if (acked_through_.has_value() && through <= *acked_through_)
      return {{}, EgressAck::unchanged(acked_through_)};
    const std::size_t inflight_before = inflight_.size();
    const std::size_t pending_before = pending_.size();
    acked_through_ = through;
    inflight_.erase(inflight_.begin(), inflight_.upper_bound(through));
    failed_.erase(failed_.begin(), failed_.upper_bound(through));
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [through](const EgressEnvelope<T>& record) {
                                    return record.sequence <= through;
                                  }),
                   pending_.end());
    const bool retry_changed = retry_.has_value() && retry_->sequence <= through;
    if (retry_changed) retry_.reset();
    EgressChange change;
    change.pending = pending_.size() != pending_before;
    change.inflight = inflight_.size() != inflight_before;
    change.acked_through = true;
    change.retry = retry_changed;
    return {change, EgressAck::advanced(through)};
  }

  /// Park a failed in-flight record while budget remains; past the budget the
  /// failure is terminal and the record is dropped.
  std::pair<EgressChange, EgressFailure> fail(std::uint64_t generation, std::uint64_t sequence) {
    if (generation != generation_) return {{}, EgressFailure::stale_generation(generation_)};
    const auto it = inflight_.find(sequence);
    if (it == inflight_.end()) return {{}, EgressFailure::unknown_sequence()};
    EgressEnvelope<T> record = std::move(it->second);
    inflight_.erase(it);
    const bool exhausted = record.attempt > policy_.retry_budget;
    EgressRetry retry;
    retry.sequence = sequence;
    retry.attempt = record.attempt;
    retry.backoff = backoff(record.attempt);
    retry.exhausted = exhausted;
    retry_ = retry;
    if (!exhausted) failed_.emplace(sequence, std::move(record));
    EgressChange change;
    change.inflight = true;
    change.retry = true;
    return {change, exhausted ? EgressFailure::exhausted(retry) : EgressFailure::retrying(retry)};
  }

  /// Make a parked failed record eligible again once its derived backoff has
  /// elapsed (the caller owns the clock). Order is preserved: the record
  /// re-enters pending at its sequence position.
  std::pair<EgressChange, EgressRetryAction> retry_now(std::uint64_t generation,
                                                       std::uint64_t sequence) {
    if (generation != generation_) return {{}, EgressRetryAction::stale_generation(generation_)};
    const auto it = failed_.find(sequence);
    if (it == failed_.end()) return {{}, EgressRetryAction::unknown_sequence()};
    EgressEnvelope<T> record = std::move(it->second);
    failed_.erase(it);
    insert_pending_ordered(std::move(record));
    const bool retry_changed = retry_.has_value() && retry_->sequence == sequence;
    if (retry_changed) retry_.reset();
    EgressChange change;
    change.pending = true;
    change.retry = retry_changed;
    return {change, EgressRetryAction::scheduled(sequence)};
  }

  /// Fence the previous attachment and replay every unacknowledged in-flight or
  /// parked record through the new generation. The ack watermark and sequence
  /// space survive; only the producer incarnation changes.
  std::pair<EgressChange, EgressReconnect> reconnect(std::uint64_t generation) {
    if (generation < generation_) return {{}, EgressReconnect::stale_generation(generation_)};
    if (generation == generation_) return {{}, EgressReconnect::unchanged(generation_)};
    generation_ = generation;
    const std::size_t replayed = inflight_.size() + failed_.size();
    for (auto& entry : inflight_) {
      entry.second.generation = generation;
      insert_pending_ordered(std::move(entry.second));
    }
    inflight_.clear();
    for (auto& entry : failed_) {
      entry.second.generation = generation;
      insert_pending_ordered(std::move(entry.second));
    }
    failed_.clear();
    for (auto& record : pending_)
      record.generation = generation;
    const bool retry_changed = retry_.has_value();
    retry_.reset();
    EgressChange change;
    // Pending changes when records were replayed into it AND when records
    // already pending were re-stamped with the new generation: a reader holding
    // the old projection would otherwise report envelopes from a fenced
    // incarnation.
    change.pending = replayed > 0 || !pending_.empty();
    change.inflight = replayed > 0;
    change.retry = retry_changed;
    return {change, EgressReconnect::advanced(generation, replayed)};
  }

private:
  static EgressPolicy validate(EgressPolicy policy) {
    if (policy.inflight_limit == 0)
      throw EgressConfigException(EgressConfigError::ZeroInflightLimit);
    if (policy.retry_base == 0) throw EgressConfigException(EgressConfigError::ZeroRetryBase);
    if (policy.retry_ceiling < policy.retry_base)
      throw EgressConfigException(EgressConfigError::RetryCeilingBelowBase);
    return policy;
  }

  void insert_pending_ordered(EgressEnvelope<T> record) {
    const auto position = std::find_if(pending_.begin(), pending_.end(),
                                       [&record](const EgressEnvelope<T>& existing) {
                                         return existing.sequence > record.sequence;
                                       });
    pending_.insert(position, std::move(record));
  }

  /// `retry_base * 2^(attempt-1)`, saturating, clamped to `retry_ceiling`.
  std::uint64_t backoff(std::uint32_t attempt) const {
    const std::uint32_t shift = std::min<std::uint32_t>(attempt == 0 ? 0 : attempt - 1, 63);
    const std::uint64_t factor = std::uint64_t{1} << shift;
    const std::uint64_t base = policy_.retry_base;
    const std::uint64_t scaled = base > UINT64_MAX / factor ? UINT64_MAX : base * factor;
    return std::min(scaled, policy_.retry_ceiling);
  }

  EgressPolicy policy_;
  std::uint64_t generation_;
  std::uint64_t next_sequence_ = 0;
  std::optional<std::uint64_t> acked_through_;
  std::deque<EgressEnvelope<T>> pending_;
  std::map<std::uint64_t, EgressEnvelope<T>> inflight_;
  std::map<std::uint64_t, EgressEnvelope<T>> failed_;
  std::optional<EgressRetry> retry_;
};

} // namespace lazily

#endif // LAZILY_EGRESS_CORE_HPP
