// Transport-agnostic reactive ingress (`#designimplementtransport`).
//
// Spec: ../lazily-spec/docs/transport-ingress.md. Reference implementation:
// lazily-rs `src/ingress_core.rs` + `src/{ingress,thread_safe_ingress,
// async_ingress}.rs`.
//
// ## The core / shell split
//
//   IngressCore<K, T, M>       the admission algebra. No context, no handles,
//                              nothing awaited, no interior mutability.
//     BasicIngressCell<Cx,...> the flavor-neutral reactive shell
//       IngressCell            single-threaded (Context)
//       ThreadSafeIngressCell  Send + Sync (ThreadSafeContext)
//       AsyncIngressCell       async (AsyncContext)
//
// This is the same split `topic_core` makes for the broadcast family and
// `KeyedOrder` makes for the map family, and for the same reason: **invalidation
// is a graph write**, so the core must not perform it. Every core mutator returns
// an `IngressChange` -- *which* reader kinds the transition dirtied -- and each
// shell clears exactly that set on its own graph. That return value is the whole
// contract between the core and a shell, and it is a pure function of the
// transition, which is what keeps the plane portable across flavors without
// re-deriving values per flavor.
//
// ## Transport-agnostic by construction
//
// The core never touches a transport. An envelope is a value
// (`IngressEnvelope`) carrying its own provenance -- `generation`, `sequence`,
// `stamped_at` -- so a WebSocket frame, an RPC response, and a polled page are
// the *same* input once decoded. `IngressTransportKind` therefore influences
// exactly one derived value, the schedule: event delivery needs no polling, and a
// bounded poll interval is offered only where an event channel is unavailable
// (and never zero).
//
// ## Admission is not async-coloured
//
// Whether an envelope is admissible is a function of the fence, the watermark,
// the reorder buffer, and the observed clock -- state the graph does not own and
// nothing has to await. The async flavor therefore mints ordinary synchronous
// readers on the `AsyncContext`'s graph and returns plain values, exactly like
// the other two (the same choice `AsyncQueueCell` / `AsyncTopicCell` make).
// Awaiting belongs to the transport, and the transport is outside the primitive
// by construction.
//
// ## NO observers -- only derives
//
// Readiness, authority, and retry are `Computed`s over scope state, not
// imperative refresh calls, and there is no observer registry, listener list, or
// subscription set anywhere below: anything that survived an invalidation would
// not be a graph edge. Freshness is time-dependent, so it enters through an
// explicit `tick(now)` rather than a hidden clock read -- the same discipline
// `TimerCell::tick` uses, and the reason staleness transitions are deterministic
// and fixture-replayable.

#ifndef LAZILY_INGRESS_HPP
#define LAZILY_INGRESS_HPP

#include <lazily/async_context.hpp>
#include <lazily/cell.hpp>
#include <lazily/context.hpp>
#include <lazily/merge.hpp>
#include <lazily/relay.hpp>  // Overflow -- the backpressure algebra is shared
#include <lazily/thread_safe.hpp>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lazily {

// -- Transport provenance and the derived schedule ---------------------------

/// How envelopes reach a scope. Event delivery is the default and needs no
/// schedule; the other two exist so a deployment without an event channel still
/// has a *bounded* fallback rather than an unbounded refresh loop.
enum class IngressTransportKind {
  /// Server-initiated delivery (WebSocket, SSE, in-proc channel). Preferred.
  EventChannel,
  /// Client-initiated, but triggered by an out-of-band event rather than a
  /// timer -- an RPC issued *because* something happened.
  RpcTriggered,
  /// Client-initiated on a bounded interval. The fallback of last resort.
  BoundedPolling,
};

/// When, if ever, a scope should ask the transport for more data.
///
/// `poll_interval` is engaged only for `BoundedPolling`, which makes "we polled a
/// transport that pushes" and "we polled in a tight loop" both unrepresentable
/// rather than merely discouraged.
struct IngressSchedule {
  IngressTransportKind kind = IngressTransportKind::EventChannel;
  std::optional<std::uint64_t> poll_interval;

  /// Derive the schedule for `kind`. A poll interval is offered only where event
  /// delivery is unavailable, and never zero.
  static IngressSchedule for_kind(IngressTransportKind kind,
                                  std::uint64_t poll_interval) {
    IngressSchedule out;
    out.kind = kind;
    if (kind == IngressTransportKind::BoundedPolling)
      out.poll_interval = poll_interval < 1 ? 1 : poll_interval;
    return out;
  }

  bool operator==(const IngressSchedule &o) const {
    return kind == o.kind && poll_interval == o.poll_interval;
  }
  bool operator!=(const IngressSchedule &o) const { return !(*this == o); }
};

/// One decoded inbound message, with the provenance admission needs.
///
/// `generation` fences a producer incarnation (a reconnect, a redeploy, a build
/// skew); `sequence` orders within a generation; `stamped_at` is the producer's
/// logical time, which is what freshness is measured against.
template <typename K, typename T> struct IngressEnvelope {
  K key{};
  std::uint64_t generation = 0;
  std::uint64_t sequence = 0;
  std::uint64_t stamped_at = 0;
  T payload{};

  IngressEnvelope() = default;
  IngressEnvelope(K k, std::uint64_t generation, std::uint64_t sequence,
                  std::uint64_t stamped_at, T payload)
      : key(std::move(k)), generation(generation), sequence(sequence),
        stamped_at(stamped_at), payload(std::move(payload)) {}

  bool operator==(const IngressEnvelope &o) const {
    return key == o.key && generation == o.generation &&
           sequence == o.sequence && stamped_at == o.stamped_at &&
           payload == o.payload;
  }
  bool operator!=(const IngressEnvelope &o) const { return !(*this == o); }
};

// -- Decisions --------------------------------------------------------------

/// Why an envelope was refused. Every variant is a *decision*, not a failure --
/// dropping a superseded envelope is correct behaviour and is receipted as such.
enum class IngressDropReason {
  /// `generation` is below the scope's fence: a zombie producer.
  StaleGeneration,
  /// `sequence` was already delivered in this generation.
  DuplicateSequence,
  /// `sequence` is already sitting in the reorder buffer.
  DuplicateBuffered,
  /// The reorder buffer is at `reorder_window` and this envelope does not fill
  /// the gap.
  ReorderWindowOverflow,
  /// `now - stamped_at` exceeds the freshness horizon.
  Expired,
  /// The hot window is at `high_water` under a bounding overflow policy.
  Backpressure,
  /// The scope is closed; it admits nothing until reopened.
  ScopeClosed,
};

/// A transport- or decode-level failure attributed to a scope. Distinct from a
/// drop: an error means we could not *decide*, so it drives retry.
enum class IngressError {
  TransportClosed,
  DecodeFailed,
  AuthorityLost,
};

/// Which case an `IngressAdmission` carries.
enum class IngressAdmissionKind {
  Accepted,
  Conflated,
  Buffered,
  GenerationHandoff,
  Dropped,
  Blocked,
};

/// The outcome of admitting one envelope. A tagged record rather than a
/// `std::variant` so the corpus runner can compare it field-for-field; the
/// factories below leave every field the case does not carry at its zero value,
/// so `==` is exact.
struct IngressAdmission {
  IngressAdmissionKind kind = IngressAdmissionKind::Blocked;
  /// Highest in-order sequence now delivered (`Accepted` / `Conflated`).
  std::uint64_t delivered_through = 0;
  /// The first sequence still missing (`Buffered`).
  std::uint64_t gap_from = 0;
  /// The fence we held / now hold (`GenerationHandoff`).
  std::uint64_t from = 0;
  std::uint64_t to = 0;
  /// Why we refused (`Dropped`).
  IngressDropReason reason = IngressDropReason::StaleGeneration;

  static IngressAdmission accepted(std::uint64_t delivered_through) {
    IngressAdmission a;
    a.kind = IngressAdmissionKind::Accepted;
    a.delivered_through = delivered_through;
    return a;
  }
  static IngressAdmission conflated(std::uint64_t delivered_through) {
    IngressAdmission a;
    a.kind = IngressAdmissionKind::Conflated;
    a.delivered_through = delivered_through;
    return a;
  }
  static IngressAdmission buffered(std::uint64_t gap_from) {
    IngressAdmission a;
    a.kind = IngressAdmissionKind::Buffered;
    a.gap_from = gap_from;
    return a;
  }
  static IngressAdmission generation_handoff(std::uint64_t from,
                                             std::uint64_t to) {
    IngressAdmission a;
    a.kind = IngressAdmissionKind::GenerationHandoff;
    a.from = from;
    a.to = to;
    return a;
  }
  static IngressAdmission dropped(IngressDropReason reason) {
    IngressAdmission a;
    a.kind = IngressAdmissionKind::Dropped;
    a.reason = reason;
    return a;
  }
  static IngressAdmission blocked() {
    IngressAdmission a;
    a.kind = IngressAdmissionKind::Blocked;
    return a;
  }

  /// Whether the envelope became visible to readers.
  bool is_delivered() const {
    return kind == IngressAdmissionKind::Accepted ||
           kind == IngressAdmissionKind::Conflated ||
           kind == IngressAdmissionKind::GenerationHandoff;
  }

  bool operator==(const IngressAdmission &o) const {
    return kind == o.kind && delivered_through == o.delivered_through &&
           gap_from == o.gap_from && from == o.from && to == o.to &&
           reason == o.reason;
  }
  bool operator!=(const IngressAdmission &o) const { return !(*this == o); }
};

// -- Lifecycle and derives --------------------------------------------------

/// Where a scope is in its lifecycle. Scopes are keyed and independent: closing
/// one never touches another.
enum class IngressLifecycle {
  /// Opened, nothing delivered yet.
  Opening,
  /// Delivering.
  Live,
  /// Disconnected but retained: state and cursors survive for replay.
  Suspended,
  /// Terminal until reopened. Admits nothing.
  Closed,
};

/// The derived answer to "can a consumer trust this scope right now?".
enum class IngressReadiness {
  Unknown,
  Warming,
  Ready,
  Stale,
  Suspended,
  Closed,
};

/// What the scope currently claims authority over -- the fence plus the in-order
/// watermark a replay request must resume from.
struct IngressAuthority {
  std::uint64_t generation = 0;
  std::optional<std::uint64_t> delivered_through;
  std::uint64_t stamped_at = 0;

  bool operator==(const IngressAuthority &o) const {
    return generation == o.generation &&
           delivered_through == o.delivered_through &&
           stamped_at == o.stamped_at;
  }
  bool operator!=(const IngressAuthority &o) const { return !(*this == o); }
};

/// The derived retry decision for a scope that has errored.
struct IngressRetry {
  std::uint32_t attempt = 0;
  std::uint64_t backoff = 0;
  std::uint64_t resume_from = 0;

  bool operator==(const IngressRetry &o) const {
    return attempt == o.attempt && backoff == o.backoff &&
           resume_from == o.resume_from;
  }
  bool operator!=(const IngressRetry &o) const { return !(*this == o); }
};

/// What a reconnect needs from the transport to close its gap.
struct ReplayRequest {
  std::uint64_t generation = 0;
  std::uint64_t from_sequence = 0;

  bool operator==(const ReplayRequest &o) const {
    return generation == o.generation && from_sequence == o.from_sequence;
  }
  bool operator!=(const ReplayRequest &o) const { return !(*this == o); }
};

// -- Policy -----------------------------------------------------------------

/// Bounds and taxes, all flavor-neutral.
struct IngressPolicy {
  /// How many out-of-order envelopes may be held per scope. `0` disables
  /// reordering: a gap drops immediately.
  std::size_t reorder_window = 8;
  /// `now - stamped_at` above this marks a scope `Stale`; an *arriving* envelope
  /// that old is dropped as `Expired`.
  std::uint64_t freshness_horizon = 1000;
  /// Merged-op count at which `overflow` engages.
  std::uint64_t high_water = 64;
  /// What to do at `high_water`. The relay algebra's overflow, reused verbatim.
  Overflow overflow = Overflow::Conflate;
  /// Retained receipts, oldest evicted first.
  std::size_t receipt_capacity = 256;
  /// First retry backoff; doubles per consecutive error.
  std::uint64_t retry_base = 10;
  /// Backoff clamp.
  std::uint64_t retry_ceiling = 10000;

  bool operator==(const IngressPolicy &o) const {
    return reorder_window == o.reorder_window &&
           freshness_horizon == o.freshness_horizon &&
           high_water == o.high_water && overflow == o.overflow &&
           receipt_capacity == o.receipt_capacity &&
           retry_base == o.retry_base && retry_ceiling == o.retry_ceiling;
  }
  bool operator!=(const IngressPolicy &o) const { return !(*this == o); }
};

/// Why a policy was refused at construction time.
enum class IngressConfigError {
  /// `Overflow::Conflate` chosen for a non-conflating merge policy.
  ConflateNotBounding,
  /// A zero receipt capacity would discard every receipt it just minted.
  ZeroReceiptCapacity,
};

/// Construction-time policy refusal. Mirrors `RelayConfigException`: the merge
/// algebra decides which overflow choices bound anything, so an unsound pairing
/// is refused where it is chosen rather than tolerated at admission time.
struct IngressConfigException : std::invalid_argument {
  IngressConfigError error;
  explicit IngressConfigException(IngressConfigError e)
      : std::invalid_argument(e == IngressConfigError::ConflateNotBounding
                                  ? "ConflateNotBounding"
                                  : "ZeroReceiptCapacity"),
        error(e) {}
};

// -- Receipts: three channels, not one log ----------------------------------

/// Which receipt channel a receipt belongs to. The three are separate reader
/// kinds because they have separate consumers: a projection wants accepts, a
/// dashboard wants drops, a supervisor wants errors.
enum class IngressReceiptChannel { Accepted, Dropped, Error };

/// Which case an `IngressReceiptOutcome` carries.
enum class IngressReceiptOutcomeKind { Accepted, Dropped, Error };

/// The decision a receipt records.
struct IngressReceiptOutcome {
  IngressReceiptOutcomeKind kind = IngressReceiptOutcomeKind::Accepted;
  std::uint64_t delivered_through = 0;
  bool conflated = false;
  IngressDropReason reason = IngressDropReason::StaleGeneration;
  IngressError error = IngressError::TransportClosed;

  static IngressReceiptOutcome accepted(std::uint64_t delivered_through,
                                        bool conflated) {
    IngressReceiptOutcome o;
    o.kind = IngressReceiptOutcomeKind::Accepted;
    o.delivered_through = delivered_through;
    o.conflated = conflated;
    return o;
  }
  static IngressReceiptOutcome dropped(IngressDropReason reason) {
    IngressReceiptOutcome o;
    o.kind = IngressReceiptOutcomeKind::Dropped;
    o.reason = reason;
    return o;
  }
  static IngressReceiptOutcome failed(IngressError error) {
    IngressReceiptOutcome o;
    o.kind = IngressReceiptOutcomeKind::Error;
    o.error = error;
    return o;
  }

  bool operator==(const IngressReceiptOutcome &o) const {
    return kind == o.kind && delivered_through == o.delivered_through &&
           conflated == o.conflated && reason == o.reason && error == o.error;
  }
  bool operator!=(const IngressReceiptOutcome &o) const {
    return !(*this == o);
  }
};

/// One durable record of an admission decision. `offset` is monotone and stable
/// across eviction, so a consumer can tell "I have seen everything" from "the log
/// wrapped".
template <typename K> struct IngressReceipt {
  std::uint64_t offset = 0;
  K key{};
  std::uint64_t generation = 0;
  std::optional<std::uint64_t> sequence;
  IngressReceiptOutcome outcome;

  IngressReceiptChannel channel() const {
    switch (outcome.kind) {
    case IngressReceiptOutcomeKind::Accepted:
      return IngressReceiptChannel::Accepted;
    case IngressReceiptOutcomeKind::Dropped:
      return IngressReceiptChannel::Dropped;
    case IngressReceiptOutcomeKind::Error:
      break;
    }
    return IngressReceiptChannel::Error;
  }

  bool operator==(const IngressReceipt &o) const {
    return offset == o.offset && key == o.key && generation == o.generation &&
           sequence == o.sequence && outcome == o.outcome;
  }
  bool operator!=(const IngressReceipt &o) const { return !(*this == o); }
};

// -- The invalidation set: the core/shell contract ---------------------------

/// Which of a scope's reader kinds a transition dirtied.
///
/// Four kinds exist because they have four different invalidation boundaries: a
/// buffered envelope moves nothing but its own gap, a `tick` across the horizon
/// moves only readiness, and an error moves only retry.
struct IngressScopeChange {
  bool value = false;
  bool readiness = false;
  bool authority = false;
  bool retry = false;

  /// Nothing changed -- the shell must not clear a reader.
  bool empty() const { return !(value || readiness || authority || retry); }

  static IngressScopeChange all() { return {true, true, true, true}; }
  static IngressScopeChange readiness_only() {
    return {false, true, false, false};
  }
  static IngressScopeChange value_only() { return {true, false, false, false}; }
  static IngressScopeChange retry_only() { return {false, false, false, true}; }
  /// What materializing a previously-unknown scope changes: an unknown scope
  /// reads `Unknown`/none, so its first appearance moves readiness and authority
  /// -- and nothing else. A reader that observed a key before it opened must
  /// learn that it did.
  static IngressScopeChange creation() { return {false, true, true, false}; }

  IngressScopeChange unite(const IngressScopeChange &o) const {
    return {value || o.value, readiness || o.readiness,
            authority || o.authority, retry || o.retry};
  }

  bool operator==(const IngressScopeChange &o) const {
    return value == o.value && readiness == o.readiness &&
           authority == o.authority && retry == o.retry;
  }
  bool operator!=(const IngressScopeChange &o) const { return !(*this == o); }
};

/// The pure invalidation set of one transition: the whole contract between the
/// core and a flavor shell.
template <typename K> struct IngressChange {
  /// Per-scope dirty reader kinds, in transition order.
  std::vector<std::pair<K, IngressScopeChange>> scopes;
  bool accepted_receipts = false;
  bool dropped_receipts = false;
  bool error_receipts = false;

  bool empty() const {
    return scopes.empty() && !accepted_receipts && !dropped_receipts &&
           !error_receipts;
  }

  void mark(K key, const IngressScopeChange &change) {
    if (!change.empty())
      scopes.emplace_back(std::move(key), change);
  }

  void mark_channel(IngressReceiptChannel channel) {
    switch (channel) {
    case IngressReceiptChannel::Accepted:
      accepted_receipts = true;
      break;
    case IngressReceiptChannel::Dropped:
      dropped_receipts = true;
      break;
    case IngressReceiptChannel::Error:
      error_receipts = true;
      break;
    }
  }
};

// -- The read-only scope projection every derive is computed from ------------

/// Read-only projection of one scope. A shell's `Computed` closures call these
/// and nothing else, which is why the three flavors cannot disagree about
/// readiness, authority, or retry.
struct IngressScopeView {
  IngressLifecycle lifecycle = IngressLifecycle::Opening;
  std::uint64_t generation = 0;
  std::optional<std::uint64_t> delivered_through;
  std::uint64_t stamped_at = 0;
  std::size_t buffered = 0;
  std::uint64_t window_depth = 0;
  std::uint32_t consecutive_errors = 0;
  std::uint64_t observed_now = 0;
  IngressPolicy policy;

  /// Whether the newest delivered stamp is inside the freshness horizon.
  bool is_fresh() const {
    const std::uint64_t age =
        observed_now > stamped_at ? observed_now - stamped_at : 0;
    return age <= policy.freshness_horizon;
  }

  /// Derived readiness. A scope that has never delivered is `Warming`, not
  /// `Stale`, because there is no stamp to be old.
  IngressReadiness readiness() const {
    switch (lifecycle) {
    case IngressLifecycle::Closed:
      return IngressReadiness::Closed;
    case IngressLifecycle::Suspended:
      return IngressReadiness::Suspended;
    case IngressLifecycle::Opening:
      return IngressReadiness::Warming;
    case IngressLifecycle::Live:
      break;
    }
    if (!delivered_through.has_value())
      return IngressReadiness::Warming;
    return is_fresh() ? IngressReadiness::Ready : IngressReadiness::Stale;
  }

  /// Derived authority. A closed scope claims none.
  std::optional<IngressAuthority> authority() const {
    if (lifecycle == IngressLifecycle::Closed)
      return std::nullopt;
    IngressAuthority a;
    a.generation = generation;
    a.delivered_through = delivered_through;
    a.stamped_at = stamped_at;
    return a;
  }

  /// The first sequence not yet delivered in order.
  std::uint64_t resume_from() const {
    return delivered_through.has_value() ? *delivered_through + 1 : 0;
  }

  /// Whether the scope is holding a gap open -- an out-of-order buffer that a
  /// replay, not a retry, is the fix for.
  bool has_gap() const { return buffered > 0; }

  /// Derived retry. Absent while no error is outstanding -- a healthy scope has
  /// no backoff, rather than a zero one.
  std::optional<IngressRetry> retry() const {
    if (consecutive_errors == 0)
      return std::nullopt;
    const std::uint32_t shift =
        std::min<std::uint32_t>(consecutive_errors - 1, 31);
    const std::uint64_t factor = std::uint64_t{1} << shift;
    std::uint64_t backoff;
    if (policy.retry_base != 0 &&
        factor > std::numeric_limits<std::uint64_t>::max() / policy.retry_base)
      backoff = std::numeric_limits<std::uint64_t>::max();
    else
      backoff = policy.retry_base * factor;
    if (backoff > policy.retry_ceiling)
      backoff = policy.retry_ceiling;
    IngressRetry r;
    r.attempt = consecutive_errors;
    r.backoff = backoff;
    r.resume_from = resume_from();
    return r;
  }

  bool operator==(const IngressScopeView &o) const {
    return lifecycle == o.lifecycle && generation == o.generation &&
           delivered_through == o.delivered_through &&
           stamped_at == o.stamped_at && buffered == o.buffered &&
           window_depth == o.window_depth &&
           consecutive_errors == o.consecutive_errors &&
           observed_now == o.observed_now && policy == o.policy;
  }
  bool operator!=(const IngressScopeView &o) const { return !(*this == o); }
};

// -- The transport seam -----------------------------------------------------

/// A decoded source of envelopes.
///
/// The core never calls this -- a shell's `pump` does -- which is exactly what
/// keeps admission independent of delivery. Implementations decode; they do not
/// decide.
template <typename K, typename T> class IngressTransportSeam {
public:
  virtual ~IngressTransportSeam() = default;

  /// How this transport delivers. Drives `IngressSchedule` and nothing else.
  virtual IngressTransportKind kind() const = 0;

  /// Take everything decoded since the last call. Never blocks.
  virtual std::vector<IngressEnvelope<K, T>> drain_inbound() = 0;

  /// Ask the producer to resend from `request.from_sequence`. Returns whether
  /// the transport could carry the request -- a polling transport that cannot
  /// address history answers `false`, which is what makes "this gap will never
  /// close" observable rather than silent.
  virtual bool request_replay(const K &key, ReplayRequest request) = 0;
};

/// An in-process event channel: the reference transport. `kind` is configurable
/// so one implementation exercises all three delivery modes -- including the
/// `BoundedPolling` case that cannot serve a replay.
template <typename K, typename T>
class InProcIngress : public IngressTransportSeam<K, T> {
public:
  explicit InProcIngress(IngressTransportKind kind) : kind_(kind) {}

  IngressTransportKind kind() const override { return kind_; }

  /// Queue one envelope for the next drain.
  void push(IngressEnvelope<K, T> envelope) {
    inbound_.push_back(std::move(envelope));
  }

  std::vector<IngressEnvelope<K, T>> drain_inbound() override {
    std::vector<IngressEnvelope<K, T>> out;
    out.reserve(inbound_.size());
    while (!inbound_.empty()) {
      out.push_back(std::move(inbound_.front()));
      inbound_.pop_front();
    }
    return out;
  }

  bool request_replay(const K &key, ReplayRequest request) override {
    // A bounded poll has no addressable history: it can only wait for the next
    // page, so it cannot honour a replay.
    if (kind_ == IngressTransportKind::BoundedPolling)
      return false;
    replays_.emplace_back(key, request);
    return true;
  }

  /// Replay requests observed so far, oldest first.
  const std::vector<std::pair<K, ReplayRequest>> &replays() const {
    return replays_;
  }

private:
  IngressTransportKind kind_;
  std::deque<IngressEnvelope<K, T>> inbound_;
  std::vector<std::pair<K, ReplayRequest>> replays_;
};

// -- The admission algebra --------------------------------------------------

namespace ingress_detail {

template <typename T> struct Scope {
  IngressLifecycle lifecycle = IngressLifecycle::Opening;
  std::uint64_t generation = 0;
  std::optional<std::uint64_t> delivered_through;
  std::uint64_t stamped_at = 0;
  /// Ordered so the reorder buffer replays in SEQUENCE order -- which is what
  /// buys `reorder_needs_no_commutativity`: a merely associative merge converges
  /// to the in-order fold under reordering.
  std::map<std::uint64_t, std::pair<T, std::uint64_t>> pending;
  std::optional<T> window;
  std::uint64_t window_depth = 0;
  std::uint32_t consecutive_errors = 0;

  Scope() = default;
  explicit Scope(std::uint64_t generation) : generation(generation) {}

  IngressScopeView view(std::uint64_t observed_now,
                        const IngressPolicy &policy) const {
    IngressScopeView v;
    v.lifecycle = lifecycle;
    v.generation = generation;
    v.delivered_through = delivered_through;
    v.stamped_at = stamped_at;
    v.buffered = pending.size();
    v.window_depth = window_depth;
    v.consecutive_errors = consecutive_errors;
    v.observed_now = observed_now;
    v.policy = policy;
    return v;
  }

  std::uint64_t next_expected() const {
    return delivered_through.has_value() ? *delivered_through + 1 : 0;
  }

  /// Everything a reader can observe *about shape rather than payload*. The
  /// buffered path diffs these to derive its invalidation set, so "a buffered
  /// envelope invalidates nothing" is a computed fact rather than a claim -- and
  /// the handoff-that-buffers (which clears the window) cannot slip through.
  struct Stamp {
    IngressLifecycle lifecycle;
    std::uint64_t generation;
    std::optional<std::uint64_t> delivered_through;
    bool has_window;
  };

  Stamp stamp() const {
    return Stamp{lifecycle, generation, delivered_through, window.has_value()};
  }

  IngressLifecycle live_or_opening() const {
    return delivered_through.has_value() ? IngressLifecycle::Live
                                         : IngressLifecycle::Opening;
  }
};

/// What the admission algebra decided, before any receipt is minted. Splitting
/// the decision from its bookkeeping keeps the scope mutation from overlapping
/// the receipt log.
enum class DecisionKind { Refuse, Block, Buffered, Delivered };

struct Decision {
  DecisionKind kind = DecisionKind::Refuse;
  IngressDropReason reason = IngressDropReason::StaleGeneration;
  std::uint64_t gap_from = 0;
  std::uint64_t delivered_through = 0;
  bool conflated = false;
  bool handoff = false;
  std::uint64_t handoff_from = 0;
  std::uint64_t handoff_to = 0;
};

} // namespace ingress_detail

/// Keyed lifecycle scopes, an admission algebra, and a bounded receipt log. No
/// context, no handles, no interior mutability -- each flavor wraps this in its
/// own mutex and owns its own reactivity.
template <typename K, typename T, typename M> class IngressCore {
public:
  /// Build a core over `policy`, validating the overflow choice against the merge
  /// algebra the way `RelayCell` does: `Conflate` bounds nothing for a
  /// non-conflating merge. Throws `IngressConfigException` otherwise.
  explicit IngressCore(IngressPolicy policy) : policy_(policy) {
    if (policy.overflow == Overflow::Conflate && !M::conflates)
      throw IngressConfigException(IngressConfigError::ConflateNotBounding);
    if (policy.receipt_capacity == 0)
      throw IngressConfigException(IngressConfigError::ZeroReceiptCapacity);
  }

  const IngressPolicy &policy() const { return policy_; }

  /// Every known scope key, for a shell rebuilding its reader table.
  std::vector<K> scope_keys() const {
    std::vector<K> keys;
    keys.reserve(scopes_.size());
    for (const auto &entry : scopes_)
      keys.push_back(entry.first);
    return keys;
  }

  /// Read-only projection of one scope, or none when unknown.
  std::optional<IngressScopeView> view(const K &key) const {
    auto it = scopes_.find(key);
    if (it == scopes_.end())
      return std::nullopt;
    return it->second.view(observed_now_, policy_);
  }

  /// Readiness of a scope. An unknown scope is `Unknown` rather than an error: a
  /// reader may legitimately observe a key before it opens.
  IngressReadiness readiness(const K &key) const {
    auto v = view(key);
    return v ? v->readiness() : IngressReadiness::Unknown;
  }

  std::optional<IngressAuthority> authority(const K &key) const {
    auto v = view(key);
    return v ? v->authority() : std::nullopt;
  }

  std::optional<IngressRetry> retry(const K &key) const {
    auto v = view(key);
    return v ? v->retry() : std::nullopt;
  }

  /// The coalesced window awaiting drain.
  std::optional<T> peek(const K &key) const {
    auto it = scopes_.find(key);
    if (it == scopes_.end())
      return std::nullopt;
    return it->second.window;
  }

  /// Receipts on one channel, oldest first.
  std::vector<IngressReceipt<K>> receipts(IngressReceiptChannel channel) const {
    std::vector<IngressReceipt<K>> out;
    for (const auto &receipt : receipts_)
      if (receipt.channel() == channel)
        out.push_back(receipt);
    return out;
  }

  std::uint64_t observed_now() const { return observed_now_; }

  /// Open (or reopen) a scope at `generation`.
  ///
  /// Reopening a suspended scope preserves its watermark so a replay can resume
  /// from the gap; reopening a *closed* scope resets it, because a closed scope's
  /// producer is gone and its sequence space is not resumable.
  IngressChange<K> open(K key, std::uint64_t generation) {
    IngressChange<K> change;
    auto it = scopes_.find(key);
    if (it == scopes_.end()) {
      scopes_.emplace(key, ingress_detail::Scope<T>(generation));
      change.mark(std::move(key), IngressScopeChange::creation());
      return change;
    }
    auto &scope = it->second;
    const auto before_lifecycle = scope.lifecycle;
    const auto before_generation = scope.generation;
    const auto before_watermark = scope.delivered_through;
    if (scope.lifecycle == IngressLifecycle::Closed) {
      scope = ingress_detail::Scope<T>(generation);
    } else {
      scope.lifecycle = scope.live_or_opening();
      if (generation > scope.generation) {
        scope.generation = generation;
        scope.delivered_through = std::nullopt;
        scope.pending.clear();
      }
    }
    const bool moved = before_lifecycle != scope.lifecycle ||
                       before_generation != scope.generation ||
                       before_watermark != scope.delivered_through;
    if (moved) {
      IngressScopeChange sc;
      sc.readiness = before_lifecycle != scope.lifecycle;
      sc.authority = true;
      change.mark(std::move(key), sc);
    }
    return change;
  }

  /// Suspend a scope: retain state and cursors, stop delivering. Returns the
  /// replay request a reconnect will need, or none when there was nothing to
  /// suspend.
  std::pair<IngressChange<K>, std::optional<ReplayRequest>>
  suspend(const K &key) {
    IngressChange<K> change;
    auto it = scopes_.find(key);
    if (it == scopes_.end())
      return {change, std::nullopt};
    auto &scope = it->second;
    if (scope.lifecycle == IngressLifecycle::Suspended ||
        scope.lifecycle == IngressLifecycle::Closed)
      return {change, std::nullopt};
    scope.lifecycle = IngressLifecycle::Suspended;
    const ReplayRequest request{scope.generation, scope.next_expected()};
    change.mark(key, IngressScopeChange::readiness_only());
    return {change, request};
  }

  /// Reconnect a scope at `generation`, clearing the error streak.
  ///
  /// A higher generation is a producer handoff: the sequence space restarts, so
  /// the buffered reorder window and the coalesced value are discarded rather
  /// than replayed against a fence they no longer belong to.
  std::pair<IngressChange<K>, ReplayRequest>
  reconnect(const K &key, std::uint64_t generation) {
    IngressChange<K> change;
    const bool created = scopes_.find(key) == scopes_.end();
    if (created)
      scopes_.emplace(key, ingress_detail::Scope<T>(generation));
    auto &scope = scopes_.find(key)->second;
    const bool handoff = generation > scope.generation;
    const bool had_window = scope.window.has_value();
    if (handoff) {
      scope.generation = generation;
      scope.delivered_through = std::nullopt;
      scope.pending.clear();
      scope.window.reset();
      scope.window_depth = 0;
    }
    const auto before_lifecycle = scope.lifecycle;
    scope.lifecycle = scope.live_or_opening();
    const bool had_errors = scope.consecutive_errors > 0;
    scope.consecutive_errors = 0;
    const ReplayRequest request{scope.generation, scope.next_expected()};
    IngressScopeChange base;
    base.value = handoff && had_window;
    base.readiness = before_lifecycle != scope.lifecycle;
    base.authority = handoff;
    base.retry = had_errors;
    change.mark(key,
                created ? base.unite(IngressScopeChange::creation()) : base);
    return {change, request};
  }

  /// Close a scope. It admits nothing and claims no authority until reopened.
  IngressChange<K> close(const K &key) {
    IngressChange<K> change;
    auto it = scopes_.find(key);
    if (it == scopes_.end())
      return change;
    auto &scope = it->second;
    if (scope.lifecycle == IngressLifecycle::Closed)
      return change;
    const bool had_window = scope.window.has_value();
    const bool had_errors = scope.consecutive_errors > 0;
    scope.lifecycle = IngressLifecycle::Closed;
    scope.pending.clear();
    scope.window.reset();
    scope.window_depth = 0;
    scope.consecutive_errors = 0;
    IngressScopeChange sc;
    sc.value = had_window;
    sc.readiness = true;
    sc.authority = true;
    sc.retry = had_errors;
    change.mark(key, sc);
    return change;
  }

  /// Advance logical time. Only scopes that *crossed* the freshness horizon are
  /// dirtied -- a tick inside the horizon invalidates nothing, which is what keeps
  /// a polling shell from re-rendering on every tick.
  IngressChange<K> tick(std::uint64_t now) {
    IngressChange<K> change;
    if (now == observed_now_)
      return change;
    const std::uint64_t before = observed_now_;
    observed_now_ = now;
    for (const auto &entry : scopes_) {
      if (entry.second.view(before, policy_).readiness() !=
          entry.second.view(now, policy_).readiness())
        change.mark(entry.first, IngressScopeChange::readiness_only());
    }
    return change;
  }

  /// Record a transport/decode failure against a scope, deepening its backoff.
  IngressChange<K> fail(const K &key, IngressError error) {
    IngressChange<K> change;
    const bool created = scopes_.find(key) == scopes_.end();
    if (created)
      scopes_.emplace(key, ingress_detail::Scope<T>(0));
    auto &scope = scopes_.find(key)->second;
    if (scope.consecutive_errors < std::numeric_limits<std::uint32_t>::max())
      ++scope.consecutive_errors;
    const std::uint64_t generation = scope.generation;
    const IngressScopeChange base = IngressScopeChange::retry_only();
    change.mark(key,
                created ? base.unite(IngressScopeChange::creation()) : base);
    IngressReceipt<K> receipt;
    receipt.key = key;
    receipt.generation = generation;
    receipt.outcome = IngressReceiptOutcome::failed(error);
    change.mark_channel(push_receipt(std::move(receipt)));
    return change;
  }

  /// Drain a scope's coalesced window, resetting its depth. Returns nothing for
  /// an empty window and dirties nothing.
  ///
  /// A drain is an *egress*, not an ack: it never moves the watermark, so a
  /// replay after a drain still resumes from the same sequence.
  std::pair<IngressChange<K>, std::optional<T>> drain(const K &key) {
    IngressChange<K> change;
    auto it = scopes_.find(key);
    if (it == scopes_.end())
      return {change, std::nullopt};
    auto &scope = it->second;
    if (!scope.window.has_value())
      return {change, std::nullopt};
    std::optional<T> value = std::move(scope.window);
    scope.window.reset();
    scope.window_depth = 0;
    change.mark(key, IngressScopeChange::value_only());
    return {change, std::move(value)};
  }

  /// Admit one envelope, applying -- in this order -- scope lifecycle, the
  /// generation fence, freshness, generation handoff, dedupe, ordering,
  /// backpressure, and merge.
  ///
  /// The order is the contract: a zombie generation is rejected before its stale
  /// sequence is consulted (else it reads as a duplicate and the zombie hides),
  /// and an expired envelope is rejected before it can occupy a reorder slot
  /// (else a slow zombie exhausts the buffer and starves live data).
  std::pair<IngressChange<K>, IngressAdmission>
  admit(IngressEnvelope<K, T> envelope) {
    const K key = envelope.key;
    const std::uint64_t generation = envelope.generation;
    const std::uint64_t sequence = envelope.sequence;
    const std::uint64_t stamped_at = envelope.stamped_at;

    const bool created = scopes_.find(key) == scopes_.end();
    std::optional<typename ingress_detail::Scope<T>::Stamp> before;
    if (created)
      scopes_.emplace(key, ingress_detail::Scope<T>(generation));
    else
      before = scopes_.find(key)->second.stamp();

    const ingress_detail::Decision decision =
        decide(scopes_.find(key)->second, policy_, observed_now_, generation,
               sequence, stamped_at, std::move(envelope.payload));

    // A refused envelope must not leave a scope behind: an expired or blocked
    // message for a key we do not track is not an admission plane, and
    // materializing one would report a readiness change that never happened.
    const bool admitted =
        decision.kind == ingress_detail::DecisionKind::Buffered ||
        decision.kind == ingress_detail::DecisionKind::Delivered;
    if (created && !admitted)
      scopes_.erase(key);

    IngressChange<K> change;
    auto scope_it = scopes_.find(key);
    const std::uint64_t fence =
        scope_it == scopes_.end() ? generation : scope_it->second.generation;

    switch (decision.kind) {
    case ingress_detail::DecisionKind::Refuse: {
      IngressReceipt<K> receipt;
      receipt.key = key;
      receipt.generation = fence;
      receipt.sequence = sequence;
      receipt.outcome = IngressReceiptOutcome::dropped(decision.reason);
      change.mark_channel(push_receipt(std::move(receipt)));
      return {std::move(change), IngressAdmission::dropped(decision.reason)};
    }
    case ingress_detail::DecisionKind::Block: {
      IngressReceipt<K> receipt;
      receipt.key = key;
      receipt.generation = fence;
      receipt.sequence = sequence;
      receipt.outcome =
          IngressReceiptOutcome::dropped(IngressDropReason::Backpressure);
      change.mark_channel(push_receipt(std::move(receipt)));
      return {std::move(change), IngressAdmission::blocked()};
    }
    case ingress_detail::DecisionKind::Buffered: {
      // A buffered envelope mints no receipt, and for an already-current scope it
      // dirties no reader, because nothing a reader can observe moved. Two cases
      // are NOT invisible and are DERIVED rather than assumed: the scope's own
      // first appearance (it moves off `Unknown`), and a generation handoff that
      // buffers -- which resets the fence, the watermark, and the window before
      // parking the envelope.
      IngressScopeChange scope_change =
          created ? IngressScopeChange::creation() : IngressScopeChange{};
      if (before.has_value() && scope_it != scopes_.end()) {
        const auto after = scope_it->second.stamp();
        IngressScopeChange diff;
        diff.value = before->has_window != after.has_window;
        diff.readiness = before->lifecycle != after.lifecycle ||
                         before->delivered_through.has_value() !=
                             after.delivered_through.has_value();
        diff.authority = before->generation != after.generation ||
                         before->delivered_through != after.delivered_through;
        scope_change = scope_change.unite(diff);
      }
      change.mark(key, scope_change);
      return {std::move(change), IngressAdmission::buffered(decision.gap_from)};
    }
    case ingress_detail::DecisionKind::Delivered:
      break;
    }

    change.mark(key, IngressScopeChange::all());
    IngressReceipt<K> receipt;
    receipt.key = key;
    receipt.generation = fence;
    receipt.sequence = sequence;
    receipt.outcome = IngressReceiptOutcome::accepted(
        decision.delivered_through, decision.conflated);
    change.mark_channel(push_receipt(std::move(receipt)));

    const IngressAdmission admission =
        decision.handoff
            ? IngressAdmission::generation_handoff(decision.handoff_from,
                                                   decision.handoff_to)
            : (decision.conflated
                   ? IngressAdmission::conflated(decision.delivered_through)
                   : IngressAdmission::accepted(decision.delivered_through));
    return {std::move(change), admission};
  }

private:
  /// The admission algebra proper: pure over one scope, mutating only that scope,
  /// minting nothing.
  static ingress_detail::Decision
  decide(ingress_detail::Scope<T> &scope, const IngressPolicy &policy,
         std::uint64_t observed_now, std::uint64_t generation,
         std::uint64_t sequence, std::uint64_t stamped_at, T payload) {
    ingress_detail::Decision out;

    // 1. lifecycle.
    if (scope.lifecycle == IngressLifecycle::Closed) {
      out.kind = ingress_detail::DecisionKind::Refuse;
      out.reason = IngressDropReason::ScopeClosed;
      return out;
    }
    // 2. generation fence -- BEFORE dedupe, so a zombie producer replaying old
    //    sequences under an old generation stays distinguishable from a retry.
    if (generation < scope.generation) {
      out.kind = ingress_detail::DecisionKind::Refuse;
      out.reason = IngressDropReason::StaleGeneration;
      return out;
    }
    // 3. freshness -- BEFORE ordering, so an expired envelope never occupies a
    //    reorder slot.
    const std::uint64_t age =
        observed_now > stamped_at ? observed_now - stamped_at : 0;
    if (age > policy.freshness_horizon) {
      out.kind = ingress_detail::DecisionKind::Refuse;
      out.reason = IngressDropReason::Expired;
      return out;
    }

    // 4. generation handoff -- a baseline RESET, not a continuation. The new
    //    incarnation's first envelope is authoritative, so the old incarnation's
    //    undrained window and buffered successors are discarded rather than
    //    folded into it: merging a superseded delta into a fresh baseline is
    //    exactly the build-skew corruption the fence exists to prevent, and it is
    //    the same rule `reconnect` at a higher generation applies.
    if (generation > scope.generation) {
      out.handoff = true;
      out.handoff_from = scope.generation;
      out.handoff_to = generation;
      scope.generation = generation;
      scope.delivered_through = std::nullopt;
      scope.pending.clear();
      scope.window.reset();
      scope.window_depth = 0;
    }

    // 5. dedupe.
    const std::uint64_t expected = scope.next_expected();
    if (sequence < expected) {
      out.kind = ingress_detail::DecisionKind::Refuse;
      out.reason = IngressDropReason::DuplicateSequence;
      return out;
    }
    // 6. ordering.
    if (sequence > expected) {
      if (scope.pending.find(sequence) != scope.pending.end()) {
        out.kind = ingress_detail::DecisionKind::Refuse;
        out.reason = IngressDropReason::DuplicateBuffered;
        return out;
      }
      if (scope.pending.size() >= policy.reorder_window) {
        out.kind = ingress_detail::DecisionKind::Refuse;
        out.reason = IngressDropReason::ReorderWindowOverflow;
        return out;
      }
      scope.pending.emplace(sequence,
                            std::make_pair(std::move(payload), stamped_at));
      out.kind = ingress_detail::DecisionKind::Buffered;
      out.gap_from = expected;
      return out;
    }

    // 7. backpressure. Checked here and not earlier: refusing an in-order
    //    envelope leaves a gap the reorder buffer cannot close, so `Block` must
    //    be observable by the producer as its own outcome.
    if (scope.window_depth >= policy.high_water) {
      switch (policy.overflow) {
      case Overflow::Block:
        // Refuses WITHOUT advancing the watermark, which is what makes the
        // producer's retry in-order rather than a duplicate.
        out.kind = ingress_detail::DecisionKind::Block;
        return out;
      case Overflow::DropNewest:
        out.kind = ingress_detail::DecisionKind::Refuse;
        out.reason = IngressDropReason::Backpressure;
        return out;
      case Overflow::DropOldest:
        scope.window.reset();
        scope.window_depth = 0;
        break;
      case Overflow::Conflate:
      case Overflow::Spill:
        // `Conflate` *is* the bound; `Spill` degrades to it until a durable tail
        // is wired, exactly as `RelayCell` does.
        break;
      }
    }

    // 8. merge.
    bool conflated = merge_into(scope, std::move(payload), stamped_at);
    scope.delivered_through = sequence;
    scope.lifecycle = IngressLifecycle::Live;
    scope.consecutive_errors = 0;
    std::uint64_t delivered_through = sequence;

    // Flush every buffered successor this delivery unblocked. ONE invalidation
    // covers the whole flush: readers observe the coalesced window, never a
    // partial replay. Replay is in SEQUENCE order, which is why a merely
    // associative merge suffices (`reorder_needs_no_commutativity`).
    for (;;) {
      const std::uint64_t next = scope.next_expected();
      auto it = scope.pending.find(next);
      if (it == scope.pending.end())
        break;
      T buffered = std::move(it->second.first);
      const std::uint64_t buffered_stamp = it->second.second;
      scope.pending.erase(it);
      const bool coalesced = merge_into(scope, std::move(buffered), buffered_stamp);
      conflated = conflated || coalesced;
      scope.delivered_through = next;
      delivered_through = next;
    }

    out.kind = ingress_detail::DecisionKind::Delivered;
    out.delivered_through = delivered_through;
    out.conflated = conflated;
    return out;
  }

  /// Merge one payload into a scope's hot head. Returns whether it coalesced with
  /// an existing window.
  static bool merge_into(ingress_detail::Scope<T> &scope, T payload,
                         std::uint64_t stamped_at) {
    bool conflated;
    if (!scope.window.has_value()) {
      scope.window = std::move(payload);
      conflated = false;
    } else {
      const T current = std::move(*scope.window);
      scope.window = M::template merge<T>(current, std::move(payload));
      conflated = true;
    }
    ++scope.window_depth;
    if (stamped_at > scope.stamped_at)
      scope.stamped_at = stamped_at;
    return conflated;
  }

  IngressReceiptChannel push_receipt(IngressReceipt<K> receipt) {
    receipt.offset = next_receipt_offset_++;
    const IngressReceiptChannel channel = receipt.channel();
    receipts_.push_back(std::move(receipt));
    while (receipts_.size() > policy_.receipt_capacity)
      receipts_.pop_front();
    return channel;
  }

  IngressPolicy policy_;
  std::unordered_map<K, ingress_detail::Scope<T>> scopes_;
  std::deque<IngressReceipt<K>> receipts_;
  std::uint64_t next_receipt_offset_ = 0;
  std::uint64_t observed_now_ = 0;
};

// -- The flavor-neutral reactive shell --------------------------------------

namespace ingress_detail {

inline Context &graph(Context &ctx) { return ctx; }
inline Context &graph(ThreadSafeContext &ctx) { return ctx.context(); }
inline Context &graph(AsyncContext &ctx) { return ctx.context(); }

// Multi-root invalidation always goes through `batch()`. One admission can dirty
// a scope's value, readiness, authority, and retry PLUS a receipt channel;
// clearing them one at a time is one effect flush each, and a reader can
// interleave and observe "new value, old authority" -- precisely the partial
// fan-out a generation handoff must never expose. `Context::clear_slot`
// accumulates into `batched_slots_` while batching, and `finish_batch` flushes
// effects exactly once for the whole set.
template <typename F> void batch(Context &ctx, F &&fn) {
  ctx.batch([&](Context &g) { fn(g); });
}
template <typename F> void batch(ThreadSafeContext &ctx, F &&fn) {
  ctx.batch([&](Context &g) { fn(g); });
}
template <typename F> void batch(AsyncContext &ctx, F &&fn) {
  ctx.context().batch([&](Context &g) { fn(g); });
}

template <typename Cx, typename T> T read(Cx &ctx, const Computed<T> &handle) {
  return ctx.get(handle);
}
template <typename T> T read(AsyncContext &ctx, const Computed<T> &handle) {
  return ctx.context().get(handle);
}

/// `false` when the reader's cache is invalid -- the probe the conformance corpus
/// asserts `invalidates` through, in both directions.
template <typename Cx, typename T>
bool is_valid(Cx &ctx, const Computed<T> &handle) {
  return ctx.is_set(handle);
}
template <typename T>
bool is_valid(AsyncContext &ctx, const Computed<T> &handle) {
  return ctx.context().is_set(handle);
}

/// The four reader kinds one keyed scope exposes. Four and not one: collapsing
/// them would make an error deepen a backoff *and* re-render a value that did not
/// change.
template <typename T> struct ScopeReaders {
  Computed<std::optional<T>> value;
  Computed<IngressReadiness> readiness;
  Computed<std::optional<IngressAuthority>> authority;
  Computed<std::optional<IngressRetry>> retry;
};

template <typename K, typename T, typename M> struct IngressCellInner {
  /// Guards the core. NEVER held across a graph write -- a reader's compute takes
  /// the graph first and then this mutex, so an op that invalidated while holding
  /// this would invert the order.
  mutable std::mutex core_mutex;
  /// Guards the per-key reader table only.
  mutable std::mutex reader_mutex;
  IngressCore<K, T, M> core;
  std::unordered_map<K, ScopeReaders<T>> scopes;
  Computed<std::vector<IngressReceipt<K>>> accepted;
  Computed<std::vector<IngressReceipt<K>>> dropped;
  Computed<std::vector<IngressReceipt<K>>> errors;
  Source<IngressTransportKind> transport_kind;
  Source<std::uint64_t> poll_interval;
  Computed<IngressSchedule> schedule;

  explicit IngressCellInner(IngressPolicy policy) : core(policy) {}
};

} // namespace ingress_detail

/// A keyed, lifecycle-scoped reactive ingress: one admission plane per key, with
/// readiness, authority, and retry as **derives rather than calls**.
///
/// The admission algebra lives in the flavor-neutral `IngressCore`; this shell
/// adds only the reactivity -- four memoized `Computed`s per keyed scope, three
/// receipt readers, and a derived schedule, minted on *this* context's graph.
template <typename OwnerContext, typename K, typename T,
          typename M = KeepLatest>
class BasicIngressCell {
public:
  using key_type = K;
  using value_type = T;
  using policy_type = M;

  /// Build an ingress over `policy`, delivering as `kind`.
  ///
  /// `poll_interval` is retained even for an event channel so a later
  /// `set_transport` to `BoundedPolling` has a bound to fall back to rather than
  /// inventing one.
  BasicIngressCell(OwnerContext &ctx, IngressPolicy policy,
                   IngressTransportKind kind, std::uint64_t poll_interval)
      : inner_(
            std::make_shared<ingress_detail::IngressCellInner<K, T, M>>(policy)) {
    Context &g = ingress_detail::graph(ctx);
    inner_->accepted = receipt_reader(g, IngressReceiptChannel::Accepted);
    inner_->dropped = receipt_reader(g, IngressReceiptChannel::Dropped);
    inner_->errors = receipt_reader(g, IngressReceiptChannel::Error);
    inner_->transport_kind = g.template source<IngressTransportKind>(kind);
    inner_->poll_interval = g.template source<std::uint64_t>(poll_interval);
    const auto transport_handle = inner_->transport_kind;
    const auto interval_handle = inner_->poll_interval;
    inner_->schedule = g.template computed<IngressSchedule>(
        [transport_handle, interval_handle](Compute &c) {
          return IngressSchedule::for_kind(c.get(transport_handle),
                                          c.get(interval_handle));
        });
  }

  // -- Mutating ops. Each takes the core mutex for the transition, RELEASES it,
  //    and only then applies the invalidation set the core reported. --

  /// Open (or reopen) a keyed scope at `generation`.
  void open(OwnerContext &ctx, K key, std::uint64_t generation) {
    IngressChange<K> change;
    {
      std::lock_guard<std::mutex> lock(inner_->core_mutex);
      change = inner_->core.open(std::move(key), generation);
    }
    apply(ctx, change);
  }

  /// Admit one decoded envelope.
  IngressAdmission admit(OwnerContext &ctx, IngressEnvelope<K, T> envelope) {
    IngressChange<K> change;
    IngressAdmission admission;
    {
      std::lock_guard<std::mutex> lock(inner_->core_mutex);
      auto result = inner_->core.admit(std::move(envelope));
      change = std::move(result.first);
      admission = result.second;
    }
    apply(ctx, change);
    return admission;
  }

  /// Suspend a scope, retaining its watermark. Returns the replay request a
  /// reconnect will need.
  std::optional<ReplayRequest> suspend(OwnerContext &ctx, const K &key) {
    IngressChange<K> change;
    std::optional<ReplayRequest> request;
    {
      std::lock_guard<std::mutex> lock(inner_->core_mutex);
      auto result = inner_->core.suspend(key);
      change = std::move(result.first);
      request = result.second;
    }
    apply(ctx, change);
    return request;
  }

  /// Reconnect a scope at `generation`, clearing its error streak.
  ReplayRequest reconnect(OwnerContext &ctx, const K &key,
                          std::uint64_t generation) {
    IngressChange<K> change;
    ReplayRequest request;
    {
      std::lock_guard<std::mutex> lock(inner_->core_mutex);
      auto result = inner_->core.reconnect(key, generation);
      change = std::move(result.first);
      request = result.second;
    }
    apply(ctx, change);
    return request;
  }

  /// Close a scope. It admits nothing and claims no authority until reopened.
  void close(OwnerContext &ctx, const K &key) {
    IngressChange<K> change;
    {
      std::lock_guard<std::mutex> lock(inner_->core_mutex);
      change = inner_->core.close(key);
    }
    apply(ctx, change);
  }

  /// Record a transport/decode failure, deepening the scope's backoff.
  void fail(OwnerContext &ctx, const K &key, IngressError error) {
    IngressChange<K> change;
    {
      std::lock_guard<std::mutex> lock(inner_->core_mutex);
      change = inner_->core.fail(key, error);
    }
    apply(ctx, change);
  }

  /// Advance logical time. Only scopes that crossed the freshness horizon are
  /// invalidated.
  void tick(OwnerContext &ctx, std::uint64_t now) {
    IngressChange<K> change;
    {
      std::lock_guard<std::mutex> lock(inner_->core_mutex);
      change = inner_->core.tick(now);
    }
    apply(ctx, change);
  }

  /// Drain a scope's coalesced window.
  std::optional<T> drain(OwnerContext &ctx, const K &key) {
    IngressChange<K> change;
    std::optional<T> value;
    {
      std::lock_guard<std::mutex> lock(inner_->core_mutex);
      auto result = inner_->core.drain(key);
      change = std::move(result.first);
      value = std::move(result.second);
    }
    apply(ctx, change);
    return value;
  }

  /// Admit everything `transport` has decoded, then ask it to replay any gap
  /// still open. Returns the admission outcomes in arrival order.
  ///
  /// The only method that touches a transport, and it makes no decision of its
  /// own: the gap it replays is the one the algebra reports.
  std::vector<IngressAdmission> pump(OwnerContext &ctx,
                                     IngressTransportSeam<K, T> &transport) {
    auto inbound = transport.drain_inbound();
    std::vector<IngressAdmission> outcomes;
    outcomes.reserve(inbound.size());
    std::vector<K> touched;
    for (auto &envelope : inbound) {
      K key = envelope.key;
      outcomes.push_back(admit(ctx, std::move(envelope)));
      if (std::find(touched.begin(), touched.end(), key) == touched.end())
        touched.push_back(std::move(key));
    }
    for (const auto &key : touched) {
      std::optional<ReplayRequest> gap;
      {
        std::lock_guard<std::mutex> lock(inner_->core_mutex);
        const auto view = inner_->core.view(key);
        if (view && view->has_gap())
          gap = ReplayRequest{view->generation, view->resume_from()};
      }
      if (gap)
        transport.request_replay(key, *gap);
    }
    return outcomes;
  }

  // -- Reactive reads. Each establishes a dependency on its memoized reader. --

  /// The coalesced window awaiting drain.
  std::optional<T> value(OwnerContext &ctx, const K &key) {
    return ingress_detail::read(ctx, ensure_readers(ctx, key).value);
  }

  /// Derived readiness.
  IngressReadiness readiness(OwnerContext &ctx, const K &key) {
    return ingress_detail::read(ctx, ensure_readers(ctx, key).readiness);
  }

  /// Derived authority.
  std::optional<IngressAuthority> authority(OwnerContext &ctx, const K &key) {
    return ingress_detail::read(ctx, ensure_readers(ctx, key).authority);
  }

  /// Derived retry decision.
  std::optional<IngressRetry> retry(OwnerContext &ctx, const K &key) {
    return ingress_detail::read(ctx, ensure_readers(ctx, key).retry);
  }

  /// Accepted receipts, oldest first.
  std::vector<IngressReceipt<K>> accepted(OwnerContext &ctx) {
    return ingress_detail::read(ctx, inner_->accepted);
  }
  /// Dropped receipts, oldest first.
  std::vector<IngressReceipt<K>> dropped(OwnerContext &ctx) {
    return ingress_detail::read(ctx, inner_->dropped);
  }
  /// Error receipts, oldest first.
  std::vector<IngressReceipt<K>> errors(OwnerContext &ctx) {
    return ingress_detail::read(ctx, inner_->errors);
  }

  /// The derived delivery schedule.
  IngressSchedule schedule(OwnerContext &ctx) {
    return ingress_detail::read(ctx, inner_->schedule);
  }

  // -- Reader handles, for composing further derives and for cache probes. --

  Computed<std::optional<T>> value_handle(OwnerContext &ctx, const K &key) {
    return ensure_readers(ctx, key).value;
  }
  Computed<IngressReadiness> readiness_handle(OwnerContext &ctx, const K &key) {
    return ensure_readers(ctx, key).readiness;
  }
  Computed<std::optional<IngressAuthority>>
  authority_handle(OwnerContext &ctx, const K &key) {
    return ensure_readers(ctx, key).authority;
  }
  Computed<std::optional<IngressRetry>> retry_handle(OwnerContext &ctx,
                                                     const K &key) {
    return ensure_readers(ctx, key).retry;
  }
  Computed<std::vector<IngressReceipt<K>>> accepted_handle() const {
    return inner_->accepted;
  }
  Computed<std::vector<IngressReceipt<K>>> dropped_handle() const {
    return inner_->dropped;
  }
  Computed<std::vector<IngressReceipt<K>>> errors_handle() const {
    return inner_->errors;
  }
  Computed<IngressSchedule> schedule_handle() const { return inner_->schedule; }

  /// Retune the transport live: falling back from an event channel to bounded
  /// polling is a cell write, so every schedule dependent reacts.
  void set_transport(OwnerContext &ctx, IngressTransportKind kind) {
    ingress_detail::graph(ctx).set(inner_->transport_kind, kind);
  }

  /// Retune the poll bound live.
  void set_poll_interval(OwnerContext &ctx, std::uint64_t interval) {
    ingress_detail::graph(ctx).template set<std::uint64_t>(
        inner_->poll_interval, interval);
  }

  // -- Non-reactive introspection (no dependency registered). --

  std::optional<IngressScopeView> view(const K &key) const {
    std::lock_guard<std::mutex> lock(inner_->core_mutex);
    return inner_->core.view(key);
  }

  IngressPolicy policy() const {
    std::lock_guard<std::mutex> lock(inner_->core_mutex);
    return inner_->core.policy();
  }

  std::vector<K> scope_keys() const {
    std::lock_guard<std::mutex> lock(inner_->core_mutex);
    return inner_->core.scope_keys();
  }

protected:
  std::shared_ptr<ingress_detail::IngressCellInner<K, T, M>> inner_;

  Computed<std::vector<IngressReceipt<K>>>
  receipt_reader(Context &g, IngressReceiptChannel channel) {
    const auto inner = inner_;
    return g.template computed<std::vector<IngressReceipt<K>>>(
        [inner, channel](Compute &) {
          std::lock_guard<std::mutex> lock(inner->core_mutex);
          return inner->core.receipts(channel);
        });
  }

  /// Mint (or return) one scope's four readers. Idempotent, so a consumer may
  /// hold a handle for a key that has not opened yet -- an unknown scope reads
  /// `Unknown`/none rather than failing.
  ingress_detail::ScopeReaders<T> ensure_readers(OwnerContext &ctx,
                                                 const K &key) {
    {
      std::lock_guard<std::mutex> lock(inner_->reader_mutex);
      const auto it = inner_->scopes.find(key);
      if (it != inner_->scopes.end())
        return it->second;
    }
    Context &g = ingress_detail::graph(ctx);
    const auto inner = inner_;
    ingress_detail::ScopeReaders<T> readers;
    readers.value = g.template computed<std::optional<T>>(
        [inner, key](Compute &) -> std::optional<T> {
          std::lock_guard<std::mutex> lock(inner->core_mutex);
          return inner->core.peek(key);
        });
    readers.readiness =
        g.template computed<IngressReadiness>([inner, key](Compute &) {
          std::lock_guard<std::mutex> lock(inner->core_mutex);
          return inner->core.readiness(key);
        });
    readers.authority = g.template computed<std::optional<IngressAuthority>>(
        [inner, key](Compute &) {
          std::lock_guard<std::mutex> lock(inner->core_mutex);
          return inner->core.authority(key);
        });
    readers.retry = g.template computed<std::optional<IngressRetry>>(
        [inner, key](Compute &) {
          std::lock_guard<std::mutex> lock(inner->core_mutex);
          return inner->core.retry(key);
        });
    std::lock_guard<std::mutex> lock(inner_->reader_mutex);
    return inner_->scopes.emplace(key, readers).first->second;
  }

  /// Apply one core-reported invalidation set. Every affected reader is cleared
  /// inside ONE batch, so no reader observes a partial fan-out -- a generation
  /// handoff must never be visible as "new value, old authority".
  void apply(OwnerContext &ctx, const IngressChange<K> &change) {
    if (change.empty())
      return;
    std::vector<SlotId> roots;
    for (const auto &entry : change.scopes) {
      // Minted BEFORE the batch opens: creating a reader is not an invalidation,
      // and it must not be folded into the frontier walk.
      const auto readers = ensure_readers(ctx, entry.first);
      if (entry.second.value)
        roots.push_back(readers.value.id());
      if (entry.second.readiness)
        roots.push_back(readers.readiness.id());
      if (entry.second.authority)
        roots.push_back(readers.authority.id());
      if (entry.second.retry)
        roots.push_back(readers.retry.id());
    }
    if (change.accepted_receipts)
      roots.push_back(inner_->accepted.id());
    if (change.dropped_receipts)
      roots.push_back(inner_->dropped.id());
    if (change.error_receipts)
      roots.push_back(inner_->errors.id());
    if (roots.empty())
      return;
    ingress_detail::batch(ctx, [&](Context &g) {
      for (const SlotId id : roots)
        g.clear_slot(id);
    });
  }
};

/// The single-threaded flavor.
template <typename K, typename T, typename M = KeepLatest>
class IngressCell : public BasicIngressCell<Context, K, T, M> {
  using Base = BasicIngressCell<Context, K, T, M>;

public:
  IngressCell(Context &ctx, IngressPolicy policy, IngressTransportKind kind,
              std::uint64_t poll_interval)
      : Base(ctx, policy, kind, poll_interval) {}
};

/// The `Send + Sync` flavor. Invalidation runs OUTSIDE the core lock and fans out
/// through `batch()`, so one admission is one frontier walk.
template <typename K, typename T, typename M = KeepLatest>
class ThreadSafeIngressCell
    : public BasicIngressCell<ThreadSafeContext, K, T, M> {
  using Base = BasicIngressCell<ThreadSafeContext, K, T, M>;

public:
  ThreadSafeIngressCell(ThreadSafeContext &ctx, IngressPolicy policy,
                        IngressTransportKind kind, std::uint64_t poll_interval)
      : Base(ctx, policy, kind, poll_interval) {}
};

/// The async flavor. Nothing here is async-coloured: an admission decision is a
/// function of the fence, the watermark, the reorder buffer, and the observed
/// clock, so there is nothing to await and no settle step.
template <typename K, typename T, typename M = KeepLatest>
class AsyncIngressCell : public BasicIngressCell<AsyncContext, K, T, M> {
  using Base = BasicIngressCell<AsyncContext, K, T, M>;

public:
  AsyncIngressCell(AsyncContext &ctx, IngressPolicy policy,
                   IngressTransportKind kind, std::uint64_t poll_interval)
      : Base(ctx, policy, kind, poll_interval) {}
};

} // namespace lazily

#endif // LAZILY_INGRESS_HPP
