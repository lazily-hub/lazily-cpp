#ifndef LAZILY_EFFECT_CAS_CORE_HPP
#define LAZILY_EFFECT_CAS_CORE_HPP

// Generic keyed compare-and-swap effect authority (`#lzeffectcas`).
//
// Spec: ../lazily-spec/docs/durable-sinks.md and the `effect_cas.json`
// fixture. Reference implementation: lazily-rs `src/effect_cas_core.rs`.
//
// The pure core owns effect identity, keyed single-flight, latest-wins
// coalescing, logical-clock retry timing, reconnect fencing, exact receipt
// matching, and the cursor of an ordered effect plan. Backends stay
// application-owned: they observe an opaque authority token and execute one
// atomic compare-and-apply (`EffectApply`). Nothing here performs I/O, reads a
// clock, or owns a graph handle -- time enters only as the explicit `now` of a
// claim and the `retry_at` of a retryable outcome.
//
// Not ported yet: lazily-rs's `durable_state()` / `hydrate()` crash image
// (`#lzeffectcasdurable`, lazily-rs e0e3b95). The canonical corpus does not pin
// it, so there is no replay that could prove a port.
//
// Wasm tier: core -- the closure is the C++17 standard library only.

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lazily {

/// Stable, non-empty effect identity. Ordered and comparable so it can key
/// receipt matching; an empty identity is refused at construction.
class EffectIdentity {
public:
  explicit EffectIdentity(std::string value) : value_(std::move(value)) {
    if (value_.empty()) throw std::invalid_argument("an effect identity must be non-empty");
  }
  const std::string& str() const { return value_; }
  bool operator==(const EffectIdentity& o) const { return value_ == o.value_; }
  bool operator!=(const EffectIdentity& o) const { return value_ != o.value_; }
  bool operator<(const EffectIdentity& o) const { return value_ < o.value_; }

private:
  std::string value_;
};

/// An ordered, non-empty sequence of idempotent effect payloads.
template <typename P> class EffectPlan {
public:
  explicit EffectPlan(std::vector<P> steps) : steps_(std::move(steps)) {
    if (steps_.empty())
      throw std::invalid_argument("an effect plan must contain at least one step");
  }
  static EffectPlan single(P payload) { return EffectPlan(std::vector<P>{std::move(payload)}); }
  std::size_t size() const { return steps_.size(); }
  const std::vector<P>& steps() const { return steps_; }
  bool operator==(const EffectPlan& o) const { return steps_ == o.steps_; }
  bool operator!=(const EffectPlan& o) const { return !(*this == o); }

private:
  std::vector<P> steps_;
};

/// Immutable desired effect. One epoch names exactly one identity and plan.
template <typename K, typename A, typename P> struct EffectIntent {
  EffectIdentity identity;
  std::uint64_t epoch = 0;
  K key;
  A expected;
  EffectPlan<P> plan;

  bool operator==(const EffectIntent& o) const {
    return identity == o.identity && epoch == o.epoch && key == o.key && expected == o.expected &&
           plan == o.plan;
  }
  bool operator!=(const EffectIntent& o) const { return !(*this == o); }
};

/// Exact identity of one adapter call.
struct EffectAttemptIdentity {
  EffectIdentity effect;
  std::uint64_t generation = 0;
  std::uint64_t epoch = 0;
  std::size_t step = 0;
  std::uint32_t attempt = 0;

  bool operator==(const EffectAttemptIdentity& o) const {
    return effect == o.effect && generation == o.generation && epoch == o.epoch && step == o.step &&
           attempt == o.attempt;
  }
  bool operator!=(const EffectAttemptIdentity& o) const { return !(*this == o); }
};

/// A successful step receipt, retained as lineage for later plan steps.
template <typename R, typename A> struct EffectReceipt {
  EffectAttemptIdentity attempt;
  R receipt;
  A authority;

  bool operator==(const EffectReceipt& o) const {
    return attempt == o.attempt && receipt == o.receipt && authority == o.authority;
  }
  bool operator!=(const EffectReceipt& o) const { return !(*this == o); }
};

/// The exact compare-and-apply call handed to an application adapter.
template <typename K, typename A, typename P, typename R> struct EffectAttempt {
  EffectAttemptIdentity identity;
  K key;
  A expected;
  P payload;
  std::vector<EffectReceipt<R, A>> prior_receipts;

  bool operator==(const EffectAttempt& o) const {
    return identity == o.identity && key == o.key && expected == o.expected &&
           payload == o.payload && prior_receipts == o.prior_receipts;
  }
  bool operator!=(const EffectAttempt& o) const { return !(*this == o); }
};

/// The only outcomes an application adapter may return.
enum class EffectApplyKind { Applied, Superseded, Retryable, Rejected };

template <typename R, typename A, typename E> struct EffectApply {
  EffectApplyKind kind = EffectApplyKind::Applied;
  std::optional<R> receipt;   // Applied
  std::optional<A> authority; // Applied (new authority) / Superseded (observed)
  std::optional<E> error;     // Retryable / Rejected

  static EffectApply applied(R receipt, A authority) {
    EffectApply a;
    a.kind = EffectApplyKind::Applied;
    a.receipt = std::move(receipt);
    a.authority = std::move(authority);
    return a;
  }
  static EffectApply superseded(A observed) {
    EffectApply a;
    a.kind = EffectApplyKind::Superseded;
    a.authority = std::move(observed);
    return a;
  }
  static EffectApply retryable(E error) {
    EffectApply a;
    a.kind = EffectApplyKind::Retryable;
    a.error = std::move(error);
    return a;
  }
  static EffectApply rejected(E error) {
    EffectApply a;
    a.kind = EffectApplyKind::Rejected;
    a.error = std::move(error);
    return a;
  }
};

/// Queued work for one key: the intent plus its resumable plan cursor.
template <typename K, typename A, typename P, typename R> struct EffectPending {
  EffectIntent<K, A, P> intent;
  std::size_t next_step = 0;
  A expected;
  std::vector<EffectReceipt<R, A>> receipts;
  std::uint32_t next_attempt = 1;
  std::optional<std::uint64_t> ready_at;

  bool operator==(const EffectPending& o) const {
    return intent == o.intent && next_step == o.next_step && expected == o.expected &&
           receipts == o.receipts && next_attempt == o.next_attempt && ready_at == o.ready_at;
  }
  bool operator!=(const EffectPending& o) const { return !(*this == o); }
};

template <typename E> struct EffectRetry {
  EffectAttemptIdentity attempt;
  E error;
  std::uint64_t retry_at = 0;

  bool operator==(const EffectRetry& o) const {
    return attempt == o.attempt && error == o.error && retry_at == o.retry_at;
  }
  bool operator!=(const EffectRetry& o) const { return !(*this == o); }
};

enum class EffectTerminalKind { Applied, Superseded, Rejected };

/// Exact terminal evidence for one effect identity.
template <typename R, typename A, typename E> struct EffectTerminal {
  EffectTerminalKind kind = EffectTerminalKind::Applied;
  EffectIdentity identity;
  std::uint64_t epoch = 0;
  std::optional<A> observed; // Superseded: the authority observed, when known
  std::optional<E> error;    // Rejected
  std::vector<EffectReceipt<R, A>> receipts;

  bool operator==(const EffectTerminal& o) const {
    return kind == o.kind && identity == o.identity && epoch == o.epoch && observed == o.observed &&
           error == o.error && receipts == o.receipts;
  }
  bool operator!=(const EffectTerminal& o) const { return !(*this == o); }
};

/// Reader projection of one key.
template <typename K, typename A, typename P, typename R, typename E> struct EffectCasKeyState {
  std::optional<EffectPending<K, A, P, R>> pending;
  std::optional<EffectAttempt<K, A, P, R>> inflight;
  std::optional<EffectRetry<E>> retry;
  std::optional<std::uint64_t> applied_through;
  std::optional<EffectTerminal<R, A, E>> terminal;

  bool operator==(const EffectCasKeyState& o) const {
    return pending == o.pending && inflight == o.inflight && retry == o.retry &&
           applied_through == o.applied_through && terminal == o.terminal;
  }
  bool operator!=(const EffectCasKeyState& o) const { return !(*this == o); }
};

template <typename K, typename A, typename P, typename R, typename E> struct EffectCasSnapshot {
  std::uint64_t generation = 0;
  std::map<K, EffectCasKeyState<K, A, P, R, E>> keys;
};

// -- Outcomes ---------------------------------------------------------------

enum class EffectCasUpsertKind { Accepted, Unchanged, AlreadyApplied, StaleEpoch, EpochConflict };

struct EffectCasUpsert {
  EffectCasUpsertKind kind = EffectCasUpsertKind::Accepted;
  std::uint64_t applied_through = 0; // AlreadyApplied
  std::uint64_t current = 0;         // StaleEpoch

  static EffectCasUpsert make(EffectCasUpsertKind kind, std::uint64_t applied = 0,
                              std::uint64_t current = 0) {
    EffectCasUpsert u;
    u.kind = kind;
    u.applied_through = applied;
    u.current = current;
    return u;
  }
  bool operator==(const EffectCasUpsert& o) const {
    return kind == o.kind && applied_through == o.applied_through && current == o.current;
  }
  bool operator!=(const EffectCasUpsert& o) const { return !(*this == o); }
};

enum class EffectCasClaimKind { Claimed, Empty, Busy, Waiting, StaleGeneration };

template <typename K, typename A, typename P, typename R> struct EffectCasClaim {
  EffectCasClaimKind kind = EffectCasClaimKind::Empty;
  std::optional<EffectAttempt<K, A, P, R>> attempt;
  std::uint64_t retry_at = 0; // Waiting
  std::uint64_t current = 0;  // StaleGeneration

  static EffectCasClaim make(EffectCasClaimKind kind) {
    EffectCasClaim c;
    c.kind = kind;
    return c;
  }
  bool operator==(const EffectCasClaim& o) const {
    return kind == o.kind && attempt == o.attempt && retry_at == o.retry_at && current == o.current;
  }
  bool operator!=(const EffectCasClaim& o) const { return !(*this == o); }
};

enum class EffectCasAppliedKind {
  StepApplied,
  Completed,
  Superseded,
  Duplicate,
  UnknownAttempt,
  StaleGeneration
};

struct EffectCasApplied {
  EffectCasAppliedKind kind = EffectCasAppliedKind::UnknownAttempt;
  std::size_t next_step = 0;         // StepApplied
  std::uint64_t applied_through = 0; // Completed
  std::uint64_t current = 0;         // StaleGeneration

  static EffectCasApplied make(EffectCasAppliedKind kind) {
    EffectCasApplied a;
    a.kind = kind;
    return a;
  }
  bool operator==(const EffectCasApplied& o) const {
    return kind == o.kind && next_step == o.next_step && applied_through == o.applied_through &&
           current == o.current;
  }
  bool operator!=(const EffectCasApplied& o) const { return !(*this == o); }
};

enum class EffectCasFailureKind { Superseded, Retrying, Rejected, UnknownAttempt, StaleGeneration };

struct EffectCasFailure {
  EffectCasFailureKind kind = EffectCasFailureKind::UnknownAttempt;
  std::uint64_t retry_at = 0; // Retrying
  std::uint64_t current = 0;  // StaleGeneration

  static EffectCasFailure make(EffectCasFailureKind kind) {
    EffectCasFailure f;
    f.kind = kind;
    return f;
  }
  bool operator==(const EffectCasFailure& o) const {
    return kind == o.kind && retry_at == o.retry_at && current == o.current;
  }
  bool operator!=(const EffectCasFailure& o) const { return !(*this == o); }
};

enum class EffectCasReconnectKind { Advanced, Unchanged, StaleGeneration };

struct EffectCasReconnect {
  EffectCasReconnectKind kind = EffectCasReconnectKind::Unchanged;
  std::uint64_t generation = 0; // Advanced / Unchanged
  std::size_t requeued = 0;     // Advanced
  std::size_t superseded = 0;   // Advanced
  std::uint64_t current = 0;    // StaleGeneration

  bool operator==(const EffectCasReconnect& o) const {
    return kind == o.kind && generation == o.generation && requeued == o.requeued &&
           superseded == o.superseded && current == o.current;
  }
  bool operator!=(const EffectCasReconnect& o) const { return !(*this == o); }
};

/// Either half of `record_outcome`.
struct EffectCasOutcome {
  bool applied = false;
  EffectCasApplied applied_outcome;
  EffectCasFailure failure_outcome;
};

/// Whether a transition changed any reader-visible state.
struct EffectCasChange {
  bool state = false;
  bool operator==(const EffectCasChange& o) const { return state == o.state; }
  bool operator!=(const EffectCasChange& o) const { return !(*this == o); }
};

/// Pure keyed effect-CAS orchestration authority.
template <typename K, typename A, typename P, typename R, typename E> class EffectCasCore {
public:
  using Intent = EffectIntent<K, A, P>;
  using Pending = EffectPending<K, A, P, R>;
  using Attempt = EffectAttempt<K, A, P, R>;
  using Receipt = EffectReceipt<R, A>;
  using Retry = EffectRetry<E>;
  using Terminal = EffectTerminal<R, A, E>;
  using KeyState = EffectCasKeyState<K, A, P, R, E>;
  using Snapshot = EffectCasSnapshot<K, A, P, R, E>;
  using Claim = EffectCasClaim<K, A, P, R>;
  using Apply = EffectApply<R, A, E>;

  explicit EffectCasCore(std::uint64_t generation) : generation_(generation) {}

  std::uint64_t generation() const { return generation_; }

  Snapshot snapshot() const {
    Snapshot out;
    out.generation = generation_;
    for (const auto& entry : keys_)
      out.keys.emplace(entry.first, project(entry.second));
    return out;
  }

  std::optional<KeyState> state(const K& key) const {
    const auto it = keys_.find(key);
    if (it == keys_.end()) return std::nullopt;
    return project(it->second);
  }

  /// Keys whose pending work is claimable at logical time `now`.
  std::vector<K> pending_keys(std::uint64_t now) const {
    std::vector<K> out;
    for (const auto& entry : keys_) {
      const Entry& e = entry.second;
      if (!e.inflight && e.pending && (!e.pending->ready_at || *e.pending->ready_at <= now))
        out.push_back(entry.first);
    }
    return out;
  }

  /// Latest-wins desired state for the intent's key.
  std::pair<EffectCasChange, EffectCasUpsert> upsert_desired(Intent intent) {
    Entry& entry = keys_[intent.key];
    if (entry.applied_through && intent.epoch <= *entry.applied_through)
      return {{},
              EffectCasUpsert::make(EffectCasUpsertKind::AlreadyApplied, *entry.applied_through)};

    const Intent* pending_intent = entry.pending ? &entry.pending->intent : nullptr;
    const Intent* inflight_intent = entry.inflight ? &entry.inflight->retained.intent : nullptr;
    std::optional<std::uint64_t> current;
    const auto widen = [&current](std::uint64_t epoch) {
      if (!current || epoch > *current) current = epoch;
    };
    if (pending_intent) widen(pending_intent->epoch);
    if (inflight_intent) widen(inflight_intent->epoch);
    if (entry.terminal_intent) widen(entry.terminal_intent->epoch);
    if (current) {
      if (intent.epoch < *current)
        return {{}, EffectCasUpsert::make(EffectCasUpsertKind::StaleEpoch, 0, *current)};
      if (intent.epoch == *current) {
        const bool same = (pending_intent && *pending_intent == intent) ||
                          (inflight_intent && *inflight_intent == intent) ||
                          (entry.terminal_intent && *entry.terminal_intent == intent);
        return {{},
                EffectCasUpsert::make(same ? EffectCasUpsertKind::Unchanged
                                           : EffectCasUpsertKind::EpochConflict)};
      }
    }

    if (entry.pending) {
      Pending superseded = std::move(*entry.pending);
      entry.pending.reset();
      entry.terminal_intent = superseded.intent;
      entry.terminal = Terminal{EffectTerminalKind::Superseded,
                                superseded.intent.identity,
                                superseded.intent.epoch,
                                std::nullopt,
                                std::nullopt,
                                std::move(superseded.receipts)};
    }
    A expected = intent.expected;
    entry.pending = Pending{std::move(intent), 0, std::move(expected), {}, 1, std::nullopt};
    entry.retry.reset();
    return {{true}, EffectCasUpsert::make(EffectCasUpsertKind::Accepted)};
  }

  /// Keyed single-flight claim at logical time `now`.
  std::pair<EffectCasChange, Claim> claim(const K& key, std::uint64_t generation,
                                          std::uint64_t now) {
    if (generation != generation_) {
      Claim c = Claim::make(EffectCasClaimKind::StaleGeneration);
      c.current = generation_;
      return {{}, c};
    }
    const auto it = keys_.find(key);
    if (it == keys_.end()) return {{}, Claim::make(EffectCasClaimKind::Empty)};
    Entry& entry = it->second;
    if (entry.inflight) return {{}, Claim::make(EffectCasClaimKind::Busy)};
    if (!entry.pending) return {{}, Claim::make(EffectCasClaimKind::Empty)};
    if (entry.pending->ready_at && now < *entry.pending->ready_at) {
      Claim c = Claim::make(EffectCasClaimKind::Waiting);
      c.retry_at = *entry.pending->ready_at;
      return {{}, c};
    }

    Pending pending = std::move(*entry.pending);
    entry.pending.reset();
    Attempt attempt{EffectAttemptIdentity{pending.intent.identity, generation, pending.intent.epoch,
                                          pending.next_step, pending.next_attempt},
                    pending.intent.key, pending.expected,
                    pending.intent.plan.steps()[pending.next_step], pending.receipts};
    entry.inflight = Inflight{std::move(pending), attempt};
    Claim c = Claim::make(EffectCasClaimKind::Claimed);
    c.attempt = std::move(attempt);
    return {{true}, c};
  }

  std::pair<EffectCasChange, EffectCasApplied>
  record_applied(const K& key, const EffectAttemptIdentity& attempt, R receipt, A authority) {
    if (attempt.generation != generation_) {
      EffectCasApplied a = EffectCasApplied::make(EffectCasAppliedKind::StaleGeneration);
      a.current = generation_;
      return {{}, a};
    }
    const auto it = keys_.find(key);
    if (it == keys_.end())
      return {{}, EffectCasApplied::make(EffectCasAppliedKind::UnknownAttempt)};
    Entry& entry = it->second;
    if (entry.terminal) {
      for (const auto& known : entry.terminal->receipts)
        if (known.attempt == attempt && known.receipt == receipt)
          return {{}, EffectCasApplied::make(EffectCasAppliedKind::Duplicate)};
    }
    if (!entry.inflight || entry.inflight->attempt.identity != attempt)
      return {{}, EffectCasApplied::make(EffectCasAppliedKind::UnknownAttempt)};

    Inflight inflight = std::move(*entry.inflight);
    entry.inflight.reset();
    inflight.retained.receipts.push_back(Receipt{attempt, std::move(receipt), authority});
    entry.retry.reset();

    if (entry.pending && entry.pending->intent.epoch > inflight.retained.intent.epoch) {
      entry.terminal_intent = inflight.retained.intent;
      entry.terminal = Terminal{EffectTerminalKind::Superseded,
                                inflight.retained.intent.identity,
                                inflight.retained.intent.epoch,
                                std::move(authority),
                                std::nullopt,
                                std::move(inflight.retained.receipts)};
      return {{true}, EffectCasApplied::make(EffectCasAppliedKind::Superseded)};
    }

    const std::size_t next_step = inflight.retained.next_step + 1;
    if (next_step < inflight.retained.intent.plan.size()) {
      inflight.retained.next_step = next_step;
      inflight.retained.expected = std::move(authority);
      inflight.retained.ready_at.reset();
      inflight.retained.next_attempt = 1;
      entry.pending = std::move(inflight.retained);
      EffectCasApplied a = EffectCasApplied::make(EffectCasAppliedKind::StepApplied);
      a.next_step = next_step;
      return {{true}, a};
    }

    const std::uint64_t epoch = inflight.retained.intent.epoch;
    entry.applied_through = entry.applied_through ? std::max(*entry.applied_through, epoch) : epoch;
    entry.terminal_intent = inflight.retained.intent;
    entry.terminal = Terminal{EffectTerminalKind::Applied,
                              inflight.retained.intent.identity,
                              epoch,
                              std::nullopt,
                              std::nullopt,
                              std::move(inflight.retained.receipts)};
    EffectCasApplied a = EffectCasApplied::make(EffectCasAppliedKind::Completed);
    a.applied_through = *entry.applied_through;
    return {{true}, a};
  }

  /// The backend observed a different authority: zero mutation happened.
  std::pair<EffectCasChange, EffectCasFailure>
  record_superseded(const K& key, const EffectAttemptIdentity& attempt, A observed) {
    return finish_failed(key, attempt, FailureKind::Superseded, std::move(observed), std::nullopt,
                         0);
  }

  std::pair<EffectCasChange, EffectCasFailure>
  record_retryable(const K& key, const EffectAttemptIdentity& attempt, E error,
                   std::uint64_t retry_at) {
    return finish_failed(key, attempt, FailureKind::Retryable, std::nullopt, std::move(error),
                         retry_at);
  }

  std::pair<EffectCasChange, EffectCasFailure>
  record_rejected(const K& key, const EffectAttemptIdentity& attempt, E error) {
    return finish_failed(key, attempt, FailureKind::Rejected, std::nullopt, std::move(error), 0);
  }

  /// Route one adapter outcome. `retry_at` is used only for `Retryable`.
  std::pair<EffectCasChange, EffectCasOutcome> record_outcome(const K& key,
                                                              const EffectAttemptIdentity& attempt,
                                                              Apply outcome,
                                                              std::uint64_t retry_at) {
    EffectCasOutcome out;
    EffectCasChange change;
    switch (outcome.kind) {
    case EffectApplyKind::Applied: {
      auto result =
          record_applied(key, attempt, std::move(*outcome.receipt), std::move(*outcome.authority));
      change = result.first;
      out.applied = true;
      out.applied_outcome = result.second;
      break;
    }
    case EffectApplyKind::Superseded: {
      auto result = record_superseded(key, attempt, std::move(*outcome.authority));
      change = result.first;
      out.failure_outcome = result.second;
      break;
    }
    case EffectApplyKind::Retryable: {
      auto result = record_retryable(key, attempt, std::move(*outcome.error), retry_at);
      change = result.first;
      out.failure_outcome = result.second;
      break;
    }
    case EffectApplyKind::Rejected: {
      auto result = record_rejected(key, attempt, std::move(*outcome.error));
      change = result.first;
      out.failure_outcome = result.second;
      break;
    }
    }
    return {change, out};
  }

  /// Fence the previous actor generation. In-flight work is requeued under the
  /// SAME effect identity with the next attempt number, unless a newer desired
  /// epoch already superseded it.
  std::pair<EffectCasChange, EffectCasReconnect> reconnect(std::uint64_t new_generation) {
    EffectCasReconnect r;
    if (new_generation < generation_) {
      r.kind = EffectCasReconnectKind::StaleGeneration;
      r.current = generation_;
      return {{}, r};
    }
    if (new_generation == generation_) {
      r.kind = EffectCasReconnectKind::Unchanged;
      r.generation = generation_;
      return {{}, r};
    }
    generation_ = new_generation;
    std::size_t requeued = 0;
    std::size_t superseded = 0;
    for (auto& kv : keys_) {
      Entry& entry = kv.second;
      if (!entry.inflight) continue;
      Inflight inflight = std::move(*entry.inflight);
      entry.inflight.reset();
      if (entry.pending && entry.pending->intent.epoch > inflight.retained.intent.epoch) {
        entry.terminal_intent = inflight.retained.intent;
        entry.terminal = Terminal{EffectTerminalKind::Superseded,
                                  inflight.retained.intent.identity,
                                  inflight.retained.intent.epoch,
                                  std::nullopt,
                                  std::nullopt,
                                  std::move(inflight.retained.receipts)};
        ++superseded;
      } else {
        if (inflight.retained.next_attempt != UINT32_MAX) ++inflight.retained.next_attempt;
        inflight.retained.ready_at.reset();
        entry.pending = std::move(inflight.retained);
        ++requeued;
      }
    }
    r.kind = EffectCasReconnectKind::Advanced;
    r.generation = new_generation;
    r.requeued = requeued;
    r.superseded = superseded;
    return {{true}, r};
  }

private:
  struct Inflight {
    Pending retained;
    Attempt attempt;
  };

  struct Entry {
    std::optional<Pending> pending;
    std::optional<Inflight> inflight;
    std::optional<Retry> retry;
    std::optional<std::uint64_t> applied_through;
    std::optional<Terminal> terminal;
    std::optional<Intent> terminal_intent;
  };

  enum class FailureKind { Superseded, Retryable, Rejected };

  static KeyState project(const Entry& entry) {
    KeyState s;
    s.pending = entry.pending;
    if (entry.inflight) s.inflight = entry.inflight->attempt;
    s.retry = entry.retry;
    s.applied_through = entry.applied_through;
    s.terminal = entry.terminal;
    return s;
  }

  std::pair<EffectCasChange, EffectCasFailure>
  finish_failed(const K& key, const EffectAttemptIdentity& attempt, FailureKind kind,
                std::optional<A> observed, std::optional<E> error, std::uint64_t retry_at) {
    if (attempt.generation != generation_) {
      EffectCasFailure f = EffectCasFailure::make(EffectCasFailureKind::StaleGeneration);
      f.current = generation_;
      return {{}, f};
    }
    const auto it = keys_.find(key);
    if (it == keys_.end())
      return {{}, EffectCasFailure::make(EffectCasFailureKind::UnknownAttempt)};
    Entry& entry = it->second;
    if (!entry.inflight || entry.inflight->attempt.identity != attempt)
      return {{}, EffectCasFailure::make(EffectCasFailureKind::UnknownAttempt)};

    Inflight inflight = std::move(*entry.inflight);
    entry.inflight.reset();
    const bool newer_pending =
        entry.pending && entry.pending->intent.epoch > inflight.retained.intent.epoch;
    const EffectIdentity identity = inflight.retained.intent.identity;
    const std::uint64_t epoch = inflight.retained.intent.epoch;

    if (kind == FailureKind::Retryable && !newer_pending) {
      entry.retry = Retry{attempt, std::move(*error), retry_at};
      if (inflight.retained.next_attempt != UINT32_MAX) ++inflight.retained.next_attempt;
      inflight.retained.ready_at = retry_at;
      entry.pending = std::move(inflight.retained);
      EffectCasFailure f = EffectCasFailure::make(EffectCasFailureKind::Retrying);
      f.retry_at = retry_at;
      return {{true}, f};
    }

    entry.retry.reset();
    entry.terminal_intent = inflight.retained.intent;
    if (kind == FailureKind::Rejected) {
      entry.terminal = Terminal{
          EffectTerminalKind::Rejected,         identity, epoch, std::nullopt, std::move(error),
          std::move(inflight.retained.receipts)};
      return {{true}, EffectCasFailure::make(EffectCasFailureKind::Rejected)};
    }
    // Superseded by the backend's authority, or a retryable failure that a newer
    // desired epoch already replaced.
    entry.terminal = Terminal{
        EffectTerminalKind::Superseded,       identity, epoch, std::move(observed), std::nullopt,
        std::move(inflight.retained.receipts)};
    return {{true}, EffectCasFailure::make(EffectCasFailureKind::Superseded)};
  }

  std::uint64_t generation_;
  std::map<K, Entry> keys_;
};

} // namespace lazily

#endif // LAZILY_EFFECT_CAS_CORE_HPP
