// Transport-agnostic reactive ingress -- algebra invariants and shell
// reactivity (`#designimplementtransport`).
//
// Mirrors the `#[cfg(test)]` tails of lazily-rs `src/ingress_core.rs`,
// `src/ingress.rs`, `src/thread_safe_ingress.rs`, and `src/async_ingress.rs`.
// The cross-language corpus replay lives in test_ingress_conformance.cpp; this
// file pins the invariants the corpus cannot name, above all the NEGATIVE
// reactive ones: a buffered envelope reruns no effect, an in-horizon tick reruns
// no readiness effect, and one admission that dirties four readers is ONE
// frontier walk rather than four.

#include <lazily/ingress.hpp>
#include <lazily/merge.hpp>

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "test_require.hpp"

using lazily::AsyncContext;
using lazily::AsyncIngressCell;
using lazily::Compute;
using lazily::Context;
using lazily::IngressAdmission;
using lazily::IngressAuthority;
using lazily::IngressCell;
using lazily::IngressConfigError;
using lazily::IngressConfigException;
using lazily::IngressCore;
using lazily::IngressDropReason;
using lazily::IngressEnvelope;
using lazily::IngressError;
using lazily::IngressPolicy;
using lazily::IngressReadiness;
using lazily::IngressReceiptChannel;
using lazily::IngressReceiptOutcome;
using lazily::IngressRetry;
using lazily::IngressSchedule;
using lazily::IngressTransportKind;
using lazily::InProcIngress;
using lazily::KeepLatest;
using lazily::Overflow;
using lazily::RawFifo;
using lazily::ReplayRequest;
using lazily::Sum;
using lazily::ThreadSafeContext;
using lazily::ThreadSafeIngressCell;

namespace {

using Key = std::string;
using Payload = std::uint64_t;
using Env = IngressEnvelope<Key, Payload>;
using SumCore = IngressCore<Key, Payload, Sum>;

Env env(const char *key, std::uint64_t generation, std::uint64_t sequence,
        std::uint64_t stamped_at, Payload payload) {
  return Env(Key(key), generation, sequence, stamped_at, payload);
}

IngressPolicy default_policy() { return IngressPolicy{}; }

// ── The admission algebra ───────────────────────────────────────────────────

void conflate_is_rejected_for_a_non_conflating_algebra() {
  IngressPolicy policy = default_policy();
  policy.overflow = Overflow::Conflate;
  bool threw = false;
  try {
    IngressCore<Key, std::vector<Payload>, RawFifo> core(policy);
    (void)core;
  } catch (const IngressConfigException &e) {
    threw = true;
    REQUIRE(e.error == IngressConfigError::ConflateNotBounding,
            "wrong config error for Conflate over a non-conflating merge");
  }
  REQUIRE(threw, "Conflate must be refused for a non-conflating merge algebra");
}

void zero_receipt_capacity_is_rejected() {
  IngressPolicy policy = default_policy();
  policy.receipt_capacity = 0;
  bool threw = false;
  try {
    SumCore core(policy);
    (void)core;
  } catch (const IngressConfigException &e) {
    threw = true;
    REQUIRE(e.error == IngressConfigError::ZeroReceiptCapacity,
            "wrong config error for a zero receipt capacity");
  }
  REQUIRE(threw, "a zero receipt capacity must be refused");
}

void in_order_delivery_conflates_and_receipts() {
  SumCore core(default_policy());
  auto first = core.admit(env("a", 1, 0, 0, 5));
  REQUIRE(first.second == IngressAdmission::accepted(0),
          "first in-order envelope is Accepted");
  REQUIRE(first.first.accepted_receipts, "a delivery mints an accept receipt");
  REQUIRE(first.first.scopes.size() == 1 &&
              first.first.scopes[0].second == lazily::IngressScopeChange::all(),
          "a delivery dirties all four reader kinds");

  auto second = core.admit(env("a", 1, 1, 0, 7));
  REQUIRE(second.second == IngressAdmission::conflated(1),
          "a second in-order envelope coalesces");
  REQUIRE(core.peek(Key("a")) == std::optional<Payload>(12),
          "the window is the merge of both payloads");
  REQUIRE(core.receipts(IngressReceiptChannel::Accepted).size() == 2,
          "two accepted receipts");
  REQUIRE(core.receipts(IngressReceiptChannel::Dropped).empty(),
          "no dropped receipts");
}

void reorder_buffers_then_flushes_in_one_invalidation() {
  SumCore core(default_policy());
  auto buffered = core.admit(env("a", 1, 2, 0, 4));
  REQUIRE(buffered.second == IngressAdmission::buffered(0),
          "an out-of-order envelope buffers");
  // A buffered envelope mints no receipt and moves no value. The scope's first
  // appearance DOES move it off `Unknown`, and saying so is the difference
  // between a sound invalidation set and a reader stuck on `Unknown` forever.
  REQUIRE(!buffered.first.accepted_receipts && !buffered.first.dropped_receipts,
          "a buffered envelope mints no receipt");
  REQUIRE(buffered.first.scopes.size() == 1 &&
              buffered.first.scopes[0].second ==
                  lazily::IngressScopeChange::creation(),
          "the scope's first appearance is readiness+authority only");
  REQUIRE(!core.peek(Key("a")).has_value(), "nothing is visible yet");

  auto again = core.admit(env("a", 1, 1, 0, 2));
  REQUIRE(again.second == IngressAdmission::buffered(0), "still buffered");
  REQUIRE(again.first.empty(),
          "a second buffered envelope on a known scope is invisible");

  auto flush = core.admit(env("a", 1, 0, 0, 1));
  REQUIRE(flush.second == IngressAdmission::conflated(2),
          "the delivery that closes the gap flushes the whole run");
  REQUIRE(core.peek(Key("a")) == std::optional<Payload>(7),
          "1 + 2 + 4 lands as ONE window");
  REQUIRE(core.view(Key("a"))->buffered == 0, "the reorder buffer is drained");
  REQUIRE(core.receipts(IngressReceiptChannel::Accepted).size() == 1,
          "exactly one accepted receipt for the delivery that unblocked");
}

void duplicates_are_dropped_after_delivery_and_while_buffered() {
  SumCore core(default_policy());
  core.admit(env("a", 1, 0, 0, 1));
  REQUIRE(core.admit(env("a", 1, 0, 0, 1)).second ==
              IngressAdmission::dropped(IngressDropReason::DuplicateSequence),
          "a redelivered sequence is a duplicate");
  core.admit(env("a", 1, 5, 0, 1));
  REQUIRE(core.admit(env("a", 1, 5, 0, 1)).second ==
              IngressAdmission::dropped(IngressDropReason::DuplicateBuffered),
          "a re-buffered sequence is a buffered duplicate");
  REQUIRE(core.peek(Key("a")) == std::optional<Payload>(1),
          "duplicates never reach the window");
}

void reorder_window_overflow_drops_rather_than_growing() {
  IngressPolicy policy = default_policy();
  policy.reorder_window = 2;
  SumCore core(policy);
  core.admit(env("a", 1, 1, 0, 1));
  core.admit(env("a", 1, 2, 0, 1));
  REQUIRE(core.admit(env("a", 1, 3, 0, 1)).second ==
              IngressAdmission::dropped(
                  IngressDropReason::ReorderWindowOverflow),
          "the reorder buffer is bounded");
  REQUIRE(core.view(Key("a"))->buffered == 2, "the bound held");
}

void a_zero_reorder_window_drops_every_gap_immediately() {
  IngressPolicy policy = default_policy();
  policy.reorder_window = 0;
  SumCore core(policy);
  REQUIRE(core.admit(env("a", 1, 1, 0, 1)).second ==
              IngressAdmission::dropped(
                  IngressDropReason::ReorderWindowOverflow),
          "a zero reorder window disables buffering");
}

void a_stale_generation_is_fenced_before_its_sequence_is_consulted() {
  SumCore core(default_policy());
  core.admit(env("a", 2, 0, 0, 1));
  // Sequence 0 would be a duplicate; generation 1 is stale. The fence WINS,
  // which is what makes a zombie producer distinguishable from a retry.
  REQUIRE(core.admit(env("a", 1, 0, 0, 9)).second ==
              IngressAdmission::dropped(IngressDropReason::StaleGeneration),
          "the fence outranks dedupe");
  REQUIRE(core.peek(Key("a")) == std::optional<Payload>(1),
          "a zombie payload never merges");
}

void a_newer_generation_hands_off_and_resets_the_sequence_space() {
  SumCore core(default_policy());
  core.admit(env("a", 1, 0, 0, 1));
  core.admit(env("a", 1, 7, 0, 1));
  REQUIRE(core.admit(env("a", 2, 0, 0, 4)).second ==
              IngressAdmission::generation_handoff(1, 2),
          "a newer generation is a handoff");
  const auto view = core.view(Key("a"));
  REQUIRE(view->generation == 2, "the fence advanced");
  REQUIRE(view->delivered_through == std::optional<std::uint64_t>(0),
          "the sequence space restarted");
  // The old generation's buffered successor is not replayed under the new fence
  // -- its sequence numbers mean something else now.
  REQUIRE(view->buffered == 0, "the handoff discarded the buffered successor");
  // Nor is its undrained window folded into the new baseline.
  REQUIRE(core.peek(Key("a")) == std::optional<Payload>(4),
          "a handoff is a baseline reset, not a continuation");
}

void a_handoff_that_buffers_still_reports_the_baseline_reset() {
  // The case the formal model caught: a NEWER generation arriving out of order
  // resets the fence, the watermark, AND the window before parking the envelope.
  // Reporting that as "buffered, nothing changed" would leave every reader
  // showing the superseded generation's value forever.
  SumCore core(default_policy());
  core.admit(env("a", 1, 0, 0, 5));
  auto handoff = core.admit(env("a", 2, 3, 0, 9));
  REQUIRE(handoff.second == IngressAdmission::buffered(0),
          "the newer generation's envelope is out of order");
  REQUIRE(handoff.first.scopes.size() == 1, "one scope moved");
  const auto change = handoff.first.scopes[0].second;
  REQUIRE(change.value && change.readiness && change.authority && !change.retry,
          "a handoff that buffers dirties value, readiness, and authority");
  REQUIRE(!core.peek(Key("a")).has_value(),
          "the superseded window is discarded");
  const auto view = core.view(Key("a"));
  REQUIRE(view->generation == 2 && !view->delivered_through.has_value() &&
              view->buffered == 1,
          "the baseline reset landed");
  // A buffered envelope under the SAME generation is still invisible.
  REQUIRE(core.admit(env("a", 2, 4, 0, 1)).first.empty(),
          "a same-generation buffered envelope invalidates nothing");
}

void an_expired_envelope_never_occupies_a_reorder_slot() {
  IngressPolicy policy = default_policy();
  policy.freshness_horizon = 10;
  policy.reorder_window = 1;
  SumCore core(policy);
  core.tick(100);
  REQUIRE(core.admit(env("a", 1, 3, 50, 1)).second ==
              IngressAdmission::dropped(IngressDropReason::Expired),
          "freshness outranks ordering");
  // A refused envelope leaves no scope behind: an expired message for an
  // untracked key is not an admission plane.
  REQUIRE(!core.view(Key("a")).has_value(),
          "a refusal must not materialize a scope");
  REQUIRE(core.admit(env("a", 1, 3, 95, 1)).second ==
              IngressAdmission::buffered(0),
          "the reorder slot is still free for fresh data");
}

void block_overflow_refuses_without_losing_the_window() {
  IngressPolicy policy = default_policy();
  policy.high_water = 1;
  policy.overflow = Overflow::Block;
  IngressCore<Key, Payload, KeepLatest> core(policy);
  core.admit(env("a", 1, 0, 0, 5));
  auto blocked = core.admit(env("a", 1, 1, 0, 9));
  REQUIRE(blocked.second == IngressAdmission::blocked(), "Block refuses");
  REQUIRE(blocked.first.dropped_receipts, "the refusal is receipted");
  REQUIRE(core.peek(Key("a")) == std::optional<Payload>(5),
          "Block is lossless: the window survives");
  // The blocked envelope did not advance the watermark, so a producer retry
  // after a drain is still in order rather than a duplicate.
  REQUIRE(core.view(Key("a"))->delivered_through ==
              std::optional<std::uint64_t>(0),
          "Block does not advance the watermark");
  core.drain(Key("a"));
  REQUIRE(core.admit(env("a", 1, 1, 0, 9)).second ==
              IngressAdmission::accepted(1),
          "the retry after a drain is in order");
}

void drop_oldest_restarts_the_window_at_the_incoming_op() {
  IngressPolicy policy = default_policy();
  policy.high_water = 2;
  policy.overflow = Overflow::DropOldest;
  SumCore core(policy);
  core.admit(env("a", 1, 0, 0, 1));
  core.admit(env("a", 1, 1, 0, 2));
  REQUIRE(core.admit(env("a", 1, 2, 0, 30)).second ==
              IngressAdmission::accepted(2),
          "DropOldest keeps delivering");
  REQUIRE(core.peek(Key("a")) == std::optional<Payload>(30),
          "the window restarts at the incoming op");
}

void drop_newest_keeps_the_window_and_receipts_the_drop() {
  IngressPolicy policy = default_policy();
  policy.high_water = 1;
  policy.overflow = Overflow::DropNewest;
  SumCore core(policy);
  core.admit(env("a", 1, 0, 0, 5));
  auto dropped = core.admit(env("a", 1, 1, 0, 9));
  REQUIRE(dropped.second ==
              IngressAdmission::dropped(IngressDropReason::Backpressure),
          "DropNewest discards the incoming op");
  REQUIRE(dropped.first.dropped_receipts, "the drop is receipted");
  REQUIRE(core.peek(Key("a")) == std::optional<Payload>(5),
          "the accumulated window survives");
}

void readiness_derives_from_lifecycle_and_freshness() {
  IngressPolicy policy = default_policy();
  policy.freshness_horizon = 10;
  SumCore core(policy);
  REQUIRE(core.readiness(Key("a")) == IngressReadiness::Unknown,
          "an unknown scope is Unknown");
  core.open(Key("a"), 1);
  REQUIRE(core.readiness(Key("a")) == IngressReadiness::Warming,
          "an opened scope with no delivery is Warming, not Stale");
  core.admit(env("a", 1, 0, 0, 1));
  REQUIRE(core.readiness(Key("a")) == IngressReadiness::Ready, "delivered");

  auto crossing = core.tick(50);
  REQUIRE(crossing.scopes.size() == 1 &&
              crossing.scopes[0].second ==
                  lazily::IngressScopeChange::readiness_only(),
          "crossing the horizon is a readiness-ONLY transition");
  REQUIRE(core.readiness(Key("a")) == IngressReadiness::Stale, "now stale");
  REQUIRE(core.tick(60).empty(),
          "a further tick inside the same readiness dirties nothing");
}

void suspend_retains_the_watermark_and_reconnect_replays_the_gap() {
  SumCore core(default_policy());
  core.admit(env("a", 1, 0, 0, 1));
  core.admit(env("a", 1, 1, 0, 1));
  auto suspended = core.suspend(Key("a"));
  REQUIRE(suspended.second == std::optional<ReplayRequest>(ReplayRequest{1, 2}),
          "suspend reports the replay request a reconnect needs");
  REQUIRE(core.readiness(Key("a")) == IngressReadiness::Suspended, "suspended");
  REQUIRE(core.peek(Key("a")) == std::optional<Payload>(2),
          "the coalesced window survives a disconnect");
  auto again = core.suspend(Key("a"));
  REQUIRE(again.first.empty() && !again.second.has_value(),
          "suspending twice is idempotent and dirties nothing");

  auto reconnected = core.reconnect(Key("a"), 1);
  REQUIRE(reconnected.second == (ReplayRequest{1, 2}),
          "a reconnect at the same generation resumes from the watermark");
  REQUIRE(core.readiness(Key("a")) == IngressReadiness::Ready, "live again");
}

void reconnect_at_a_higher_generation_discards_the_stale_window() {
  SumCore core(default_policy());
  core.admit(env("a", 1, 0, 0, 5));
  core.suspend(Key("a"));
  auto reconnected = core.reconnect(Key("a"), 3);
  REQUIRE(reconnected.second == (ReplayRequest{3, 0}),
          "a higher generation restarts the sequence space");
  bool value_and_authority = false;
  for (const auto &entry : reconnected.first.scopes)
    if (entry.second.value && entry.second.authority)
      value_and_authority = true;
  REQUIRE(value_and_authority, "the discard is reported to value + authority");
  REQUIRE(!core.peek(Key("a")).has_value(), "the stale window is gone");
}

void errors_deepen_backoff_and_a_delivery_clears_it() {
  IngressPolicy policy = default_policy();
  policy.retry_base = 10;
  policy.retry_ceiling = 25;
  SumCore core(policy);
  core.open(Key("a"), 1);
  REQUIRE(!core.retry(Key("a")).has_value(),
          "a healthy scope has no backoff, not a zero one");

  core.fail(Key("a"), IngressError::TransportClosed);
  REQUIRE(core.retry(Key("a")) ==
              std::optional<IngressRetry>(IngressRetry{1, 10, 0}),
          "the first error yields the base backoff");
  core.fail(Key("a"), IngressError::TransportClosed);
  REQUIRE(core.retry(Key("a"))->backoff == 20, "the backoff doubles");
  core.fail(Key("a"), IngressError::TransportClosed);
  REQUIRE(core.retry(Key("a"))->backoff == 25,
          "clamped to the ceiling, not doubled past it");
  REQUIRE(core.receipts(IngressReceiptChannel::Error).size() == 3,
          "each error is receipted on the error channel");

  core.admit(env("a", 1, 0, 0, 1));
  REQUIRE(!core.retry(Key("a")).has_value(), "a delivery clears the streak");
}

void a_reconnect_clears_the_error_streak_without_a_delivery() {
  SumCore core(default_policy());
  core.open(Key("a"), 1);
  core.fail(Key("a"), IngressError::AuthorityLost);
  auto reconnected = core.reconnect(Key("a"), 1);
  bool retry_marked = false;
  for (const auto &entry : reconnected.first.scopes)
    if (entry.second.retry)
      retry_marked = true;
  REQUIRE(retry_marked, "clearing the streak dirties the retry reader");
  REQUIRE(!core.retry(Key("a")).has_value(), "the streak is cleared");
}

void closed_scopes_admit_nothing_and_claim_no_authority() {
  SumCore core(default_policy());
  core.admit(env("a", 1, 0, 0, 1));
  core.close(Key("a"));
  REQUIRE(!core.authority(Key("a")).has_value(),
          "a closed scope claims no authority");
  REQUIRE(core.admit(env("a", 1, 1, 0, 1)).second ==
              IngressAdmission::dropped(IngressDropReason::ScopeClosed),
          "a closed scope admits nothing");
  core.open(Key("a"), 1);
  REQUIRE(core.admit(env("a", 1, 0, 0, 4)).second ==
              IngressAdmission::accepted(0),
          "reopening a CLOSED scope restarts its sequence space");
}

void scopes_are_independent() {
  SumCore core(default_policy());
  core.admit(env("a", 1, 0, 0, 1));
  auto other = core.admit(env("b", 1, 0, 0, 2));
  REQUIRE(other.first.scopes.size() == 1 && other.first.scopes[0].first == "b",
          "one delivery dirties exactly one scope");
  core.close(Key("b"));
  REQUIRE(core.readiness(Key("a")) == IngressReadiness::Ready,
          "closing one scope never touches another");
  REQUIRE(core.peek(Key("a")) == std::optional<Payload>(1), "nor its window");
}

void receipts_are_bounded_and_offsets_stay_monotone() {
  IngressPolicy policy = default_policy();
  policy.receipt_capacity = 2;
  SumCore core(policy);
  for (std::uint64_t seq = 0; seq < 4; ++seq)
    core.admit(env("a", 1, seq, 0, 1));
  const auto accepted = core.receipts(IngressReceiptChannel::Accepted);
  REQUIRE(accepted.size() == 2, "the receipt log is bounded");
  REQUIRE(accepted[0].offset == 2 && accepted[1].offset == 3,
          "offsets survive eviction, so a consumer can tell 'seen everything' "
          "from 'the log wrapped'");
}

void a_schedule_offers_a_poll_interval_only_without_event_delivery() {
  REQUIRE(!IngressSchedule::for_kind(IngressTransportKind::EventChannel, 50)
               .poll_interval.has_value(),
          "an event channel needs no poll");
  REQUIRE(!IngressSchedule::for_kind(IngressTransportKind::RpcTriggered, 50)
               .poll_interval.has_value(),
          "an RPC trigger needs no poll");
  REQUIRE(IngressSchedule::for_kind(IngressTransportKind::BoundedPolling, 50)
                  .poll_interval == std::optional<std::uint64_t>(50),
          "bounded polling carries its bound");
  REQUIRE(IngressSchedule::for_kind(IngressTransportKind::BoundedPolling, 0)
                  .poll_interval == std::optional<std::uint64_t>(1),
          "a zero interval would be an unbounded refresh loop");
}

void drain_is_a_value_only_transition_and_empty_drains_dirty_nothing() {
  SumCore core(default_policy());
  core.admit(env("a", 1, 0, 0, 3));
  auto drained = core.drain(Key("a"));
  REQUIRE(drained.second == std::optional<Payload>(3), "the window drains");
  REQUIRE(drained.first.scopes.size() == 1 &&
              drained.first.scopes[0].second ==
                  lazily::IngressScopeChange::value_only(),
          "a drain is a value-only transition");
  auto empty = core.drain(Key("a"));
  REQUIRE(!empty.second.has_value() && empty.first.empty(),
          "an empty drain invalidates nothing");
  REQUIRE(core.view(Key("a"))->delivered_through ==
              std::optional<std::uint64_t>(0),
          "a drain is an egress, not an ack: the watermark does not move");
}

void out_of_order_arrival_converges_to_the_in_order_fold() {
  // The reordering tax is paid by the BUFFER, not by the algebra: for any arrival
  // permutation of a contiguous run, the drained window equals the in-order fold
  // even though `Sum` is merely associative here (`reorder_needs_no_commutativity`).
  const std::uint64_t permutations[5][4] = {
      {0, 1, 2, 3}, {3, 2, 1, 0}, {1, 0, 3, 2}, {2, 0, 1, 3}, {0, 3, 1, 2}};
  for (const auto &order : permutations) {
    SumCore core(default_policy());
    for (const std::uint64_t seq : order)
      core.admit(env("a", 1, seq, 0, std::uint64_t{1} << seq));
    REQUIRE(core.peek(Key("a")) == std::optional<Payload>(1 + 2 + 4 + 8),
            "every arrival permutation converges to the in-order fold");
    REQUIRE(core.view(Key("a"))->delivered_through ==
                std::optional<std::uint64_t>(3),
            "and to the same watermark");
  }
}

// ── The reactive shells ─────────────────────────────────────────────────────

// One templated body per behaviour, instantiated against every flavour: the
// family's claim is that all three shells obey ONE contract, so a per-flavour
// copy of the assertion would be the wrong shape.

template <typename Cell, typename Cx> struct Shell {
  Cx ctx;
  Cell cell;
  Shell(IngressPolicy policy, IngressTransportKind kind,
        std::uint64_t poll_interval)
      : ctx(), cell(ctx, policy, kind, poll_interval) {}
  // Effects always attach to the underlying sync graph: the readers live there
  // for every flavour, because admission is not async-coloured.
  Context &graph() { return lazily::ingress_detail::graph(ctx); }
};

template <typename Cell, typename Cx>
void delivery_is_visible_through_the_value_reader(const char *flavour) {
  Shell<Cell, Cx> s(default_policy(), IngressTransportKind::EventChannel, 25);
  REQUIRE(!s.cell.value(s.ctx, Key("a")).has_value(),
          std::string(flavour) + ": an unknown scope has no window");
  s.cell.admit(s.ctx, env("a", 1, 0, 0, 5));
  REQUIRE(s.cell.value(s.ctx, Key("a")) == std::optional<Payload>(5),
          std::string(flavour) + ": the delivery is visible");
  s.cell.admit(s.ctx, env("a", 1, 1, 0, 7));
  REQUIRE(s.cell.value(s.ctx, Key("a")) == std::optional<Payload>(12),
          std::string(flavour) + ": the second delivery coalesced");
  REQUIRE(s.cell.drain(s.ctx, Key("a")) == std::optional<Payload>(12),
          std::string(flavour) + ": the window drains");
  REQUIRE(!s.cell.value(s.ctx, Key("a")).has_value(),
          std::string(flavour) + ": the drain is visible to the value reader");
}

template <typename Cell, typename Cx>
void readiness_authority_and_retry_are_derives(const char *flavour) {
  IngressPolicy policy = default_policy();
  policy.freshness_horizon = 10;
  policy.retry_base = 4;
  Shell<Cell, Cx> s(policy, IngressTransportKind::EventChannel, 25);
  REQUIRE(s.cell.readiness(s.ctx, Key("a")) == IngressReadiness::Unknown,
          std::string(flavour) + ": unknown before open");
  REQUIRE(!s.cell.authority(s.ctx, Key("a")).has_value(),
          std::string(flavour) + ": no authority before open");
  s.cell.open(s.ctx, Key("a"), 3);
  REQUIRE(s.cell.readiness(s.ctx, Key("a")) == IngressReadiness::Warming,
          std::string(flavour) + ": warming after open");
  s.cell.admit(s.ctx, env("a", 3, 0, 5, 1));
  REQUIRE(s.cell.readiness(s.ctx, Key("a")) == IngressReadiness::Ready,
          std::string(flavour) + ": ready after delivery");
  REQUIRE(s.cell.authority(s.ctx, Key("a")) ==
              std::optional<IngressAuthority>(
                  IngressAuthority{3, std::optional<std::uint64_t>(0), 5}),
          std::string(flavour) + ": authority is a derive of the same transition");
  s.cell.tick(s.ctx, 100);
  REQUIRE(s.cell.readiness(s.ctx, Key("a")) == IngressReadiness::Stale,
          std::string(flavour) + ": crossing the horizon is observable");
  s.cell.fail(s.ctx, Key("a"), IngressError::TransportClosed);
  REQUIRE(s.cell.retry(s.ctx, Key("a"))->backoff == 4,
          std::string(flavour) + ": retry is a derive too");
}

template <typename Cell, typename Cx>
void a_buffered_envelope_reruns_no_effect(const char *flavour) {
  Shell<Cell, Cx> s(default_policy(), IngressTransportKind::EventChannel, 25);
  s.cell.open(s.ctx, Key("a"), 1);
  const auto value = s.cell.value_handle(s.ctx, Key("a"));
  int runs = 0;
  std::vector<std::optional<Payload>> observed;
  auto effect = s.graph().effect_void([&](Compute &c) {
    ++runs;
    observed.push_back(c.get(value));
  });
  REQUIRE(runs == 1, std::string(flavour) + ": the effect ran once on attach");

  // Out of order: nothing observable moved, so the value effect must not run.
  s.cell.admit(s.ctx, env("a", 1, 2, 0, 4));
  s.cell.admit(s.ctx, env("a", 1, 1, 0, 2));
  REQUIRE(runs == 1,
          std::string(flavour) + ": a buffered envelope invalidates nothing");

  // The delivery that closes the gap flushes all three as ONE value change.
  s.cell.admit(s.ctx, env("a", 1, 0, 0, 1));
  REQUIRE(runs == 2, std::string(flavour) + ": the flush is one value change");
  REQUIRE(observed.size() == 2 && !observed[0].has_value() &&
              observed[1] == std::optional<Payload>(7),
          std::string(flavour) + ": readers never see a partial replay");
  s.graph().dispose_effect(effect);
}

template <typename Cell, typename Cx>
void a_tick_inside_the_horizon_reruns_no_readiness_effect(const char *flavour) {
  IngressPolicy policy = default_policy();
  policy.freshness_horizon = 100;
  Shell<Cell, Cx> s(policy, IngressTransportKind::EventChannel, 25);
  s.cell.admit(s.ctx, env("a", 1, 0, 0, 1));
  const auto readiness = s.cell.readiness_handle(s.ctx, Key("a"));
  int runs = 0;
  auto effect = s.graph().effect_void([&](Compute &c) {
    ++runs;
    (void)c.get(readiness);
  });
  REQUIRE(runs == 1, std::string(flavour) + ": attached");
  s.cell.tick(s.ctx, 50);
  REQUIRE(runs == 1,
          std::string(flavour) + ": a tick inside the horizon is not a change");
  s.cell.tick(s.ctx, 500);
  REQUIRE(runs == 2,
          std::string(flavour) + ": crossing the horizon IS a change");
  s.graph().dispose_effect(effect);
}

template <typename Cell, typename Cx>
void a_generation_handoff_lands_in_one_frontier_walk(const char *flavour) {
  // The frontier-walk gate. One admission that dirties value, readiness,
  // authority, and retry must be ONE effect run: a shell that cleared each
  // reader outside a batch would expose "new value, old authority" -- the
  // partial fan-out a generation handoff must never show.
  Shell<Cell, Cx> s(default_policy(), IngressTransportKind::EventChannel, 25);
  s.cell.admit(s.ctx, env("a", 1, 0, 0, 5));
  const auto value = s.cell.value_handle(s.ctx, Key("a"));
  const auto authority = s.cell.authority_handle(s.ctx, Key("a"));
  int runs = 0;
  std::vector<std::pair<std::optional<Payload>, std::uint64_t>> seen;
  auto effect = s.graph().effect_void([&](Compute &c) {
    ++runs;
    const auto v = c.get(value);
    const auto a = c.get(authority);
    seen.emplace_back(v, a ? a->generation : 0);
  });
  REQUIRE(runs == 1, std::string(flavour) + ": attached");
  s.cell.admit(s.ctx, env("a", 2, 0, 0, 9));
  REQUIRE(runs == 2, std::string(flavour) +
                         ": one admission is ONE effect run, not one per "
                         "dirtied reader kind");
  REQUIRE(seen.size() == 2 && seen[0].first == std::optional<Payload>(5) &&
              seen[0].second == 1 && seen[1].first == std::optional<Payload>(9) &&
              seen[1].second == 2,
          std::string(flavour) +
              ": value and authority land together, never new/old");
  s.graph().dispose_effect(effect);
}

template <typename Cell, typename Cx>
void scopes_do_not_invalidate_each_other(const char *flavour) {
  Shell<Cell, Cx> s(default_policy(), IngressTransportKind::EventChannel, 25);
  s.cell.admit(s.ctx, env("a", 1, 0, 0, 1));
  const auto value = s.cell.value_handle(s.ctx, Key("a"));
  int runs = 0;
  auto effect = s.graph().effect_void([&](Compute &c) {
    ++runs;
    (void)c.get(value);
  });
  REQUIRE(runs == 1, std::string(flavour) + ": attached");
  s.cell.admit(s.ctx, env("b", 1, 0, 0, 2));
  s.cell.close(s.ctx, Key("b"));
  REQUIRE(runs == 1, std::string(flavour) + ": scopes are independent planes");
  REQUIRE(s.cell.value(s.ctx, Key("a")) == std::optional<Payload>(1),
          std::string(flavour) + ": and keep their windows");
  s.graph().dispose_effect(effect);
}

template <typename Cell, typename Cx>
void receipt_channels_are_independent_readers(const char *flavour) {
  Shell<Cell, Cx> s(default_policy(), IngressTransportKind::EventChannel, 25);
  s.cell.admit(s.ctx, env("a", 2, 0, 0, 1));
  REQUIRE(s.cell.accepted(s.ctx).size() == 1 &&
              s.cell.dropped(s.ctx).empty() && s.cell.errors(s.ctx).empty(),
          std::string(flavour) + ": an accept lands only on the accept channel");
  // A fenced zombie shows up ONLY on the dropped channel.
  s.cell.admit(s.ctx, env("a", 1, 0, 0, 1));
  REQUIRE(s.cell.accepted(s.ctx).size() == 1,
          std::string(flavour) + ": a drop must not touch the accept channel");
  const auto dropped = s.cell.dropped(s.ctx);
  REQUIRE(dropped.size() == 1 &&
              dropped[0].outcome ==
                  IngressReceiptOutcome::dropped(
                      IngressDropReason::StaleGeneration),
          std::string(flavour) + ": the zombie is receipted as stale");
  s.cell.fail(s.ctx, Key("a"), IngressError::DecodeFailed);
  REQUIRE(s.cell.errors(s.ctx).size() == 1 && s.cell.dropped(s.ctx).size() == 1,
          std::string(flavour) + ": an error lands only on the error channel");
}

template <typename Cell, typename Cx>
void the_schedule_derives_from_the_transport_and_retunes_live(
    const char *flavour) {
  Shell<Cell, Cx> s(default_policy(), IngressTransportKind::EventChannel, 25);
  REQUIRE(!s.cell.schedule(s.ctx).poll_interval.has_value(),
          std::string(flavour) + ": an event channel schedules no poll");
  s.cell.set_transport(s.ctx, IngressTransportKind::BoundedPolling);
  REQUIRE(s.cell.schedule(s.ctx).poll_interval ==
              std::optional<std::uint64_t>(25),
          std::string(flavour) + ": the fallback bound was retained");
  s.cell.set_poll_interval(s.ctx, 200);
  REQUIRE(s.cell.schedule(s.ctx).poll_interval ==
              std::optional<std::uint64_t>(200),
          std::string(flavour) + ": the bound retunes live");
  s.cell.set_transport(s.ctx, IngressTransportKind::RpcTriggered);
  REQUIRE(!s.cell.schedule(s.ctx).poll_interval.has_value(),
          std::string(flavour) + ": and disengages again");
}

template <typename Cell, typename Cx>
void pump_admits_a_batch_and_requests_replay_for_a_surviving_gap(
    const char *flavour) {
  Shell<Cell, Cx> s(default_policy(), IngressTransportKind::EventChannel, 25);
  InProcIngress<Key, Payload> transport(IngressTransportKind::EventChannel);
  transport.push(env("a", 1, 0, 0, 1));
  transport.push(env("a", 1, 2, 0, 4));
  const auto outcomes = s.cell.pump(s.ctx, transport);
  REQUIRE(outcomes.size() == 2 && outcomes[0].is_delivered() &&
              outcomes[1] == IngressAdmission::buffered(1),
          std::string(flavour) + ": the batch admits in arrival order");
  REQUIRE(transport.replays().size() == 1 &&
              transport.replays()[0].first == "a" &&
              transport.replays()[0].second == (ReplayRequest{1, 1}),
          std::string(flavour) +
              ": the gap the algebra reports is the gap replayed");
  transport.push(env("a", 1, 1, 0, 2));
  s.cell.pump(s.ctx, transport);
  REQUIRE(s.cell.value(s.ctx, Key("a")) == std::optional<Payload>(7),
          std::string(flavour) + ": the replay closed the gap");
  REQUIRE(transport.replays().size() == 1,
          std::string(flavour) + ": and a second pump asks for nothing more");
}

template <typename Cell, typename Cx>
void a_polling_transport_cannot_serve_a_replay(const char *flavour) {
  Shell<Cell, Cx> s(default_policy(), IngressTransportKind::BoundedPolling, 25);
  InProcIngress<Key, Payload> transport(IngressTransportKind::BoundedPolling);
  transport.push(env("a", 1, 3, 0, 1));
  s.cell.pump(s.ctx, transport);
  REQUIRE(transport.replays().empty(),
          std::string(flavour) +
              ": a bounded poll has no addressable history, so 'this gap will "
              "never close' is observable rather than silent");
}

template <typename Cell, typename Cx> void every_shell_behaviour(const char *flavour) {
  delivery_is_visible_through_the_value_reader<Cell, Cx>(flavour);
  readiness_authority_and_retry_are_derives<Cell, Cx>(flavour);
  a_buffered_envelope_reruns_no_effect<Cell, Cx>(flavour);
  a_tick_inside_the_horizon_reruns_no_readiness_effect<Cell, Cx>(flavour);
  a_generation_handoff_lands_in_one_frontier_walk<Cell, Cx>(flavour);
  scopes_do_not_invalidate_each_other<Cell, Cx>(flavour);
  receipt_channels_are_independent_readers<Cell, Cx>(flavour);
  the_schedule_derives_from_the_transport_and_retunes_live<Cell, Cx>(flavour);
  pump_admits_a_batch_and_requests_replay_for_a_surviving_gap<Cell, Cx>(flavour);
  a_polling_transport_cannot_serve_a_replay<Cell, Cx>(flavour);
}

} // namespace

int main() {
  conflate_is_rejected_for_a_non_conflating_algebra();
  zero_receipt_capacity_is_rejected();
  in_order_delivery_conflates_and_receipts();
  reorder_buffers_then_flushes_in_one_invalidation();
  duplicates_are_dropped_after_delivery_and_while_buffered();
  reorder_window_overflow_drops_rather_than_growing();
  a_zero_reorder_window_drops_every_gap_immediately();
  a_stale_generation_is_fenced_before_its_sequence_is_consulted();
  a_newer_generation_hands_off_and_resets_the_sequence_space();
  a_handoff_that_buffers_still_reports_the_baseline_reset();
  an_expired_envelope_never_occupies_a_reorder_slot();
  block_overflow_refuses_without_losing_the_window();
  drop_oldest_restarts_the_window_at_the_incoming_op();
  drop_newest_keeps_the_window_and_receipts_the_drop();
  readiness_derives_from_lifecycle_and_freshness();
  suspend_retains_the_watermark_and_reconnect_replays_the_gap();
  reconnect_at_a_higher_generation_discards_the_stale_window();
  errors_deepen_backoff_and_a_delivery_clears_it();
  a_reconnect_clears_the_error_streak_without_a_delivery();
  closed_scopes_admit_nothing_and_claim_no_authority();
  scopes_are_independent();
  receipts_are_bounded_and_offsets_stay_monotone();
  a_schedule_offers_a_poll_interval_only_without_event_delivery();
  drain_is_a_value_only_transition_and_empty_drains_dirty_nothing();
  out_of_order_arrival_converges_to_the_in_order_fold();

  every_shell_behaviour<IngressCell<Key, Payload, Sum>, Context>(
      "single-threaded");
  every_shell_behaviour<ThreadSafeIngressCell<Key, Payload, Sum>,
                        ThreadSafeContext>("thread-safe");
  every_shell_behaviour<AsyncIngressCell<Key, Payload, Sum>, AsyncContext>(
      "async");

  std::cout << "ingress: algebra invariants + 10 shell behaviours x 3 flavours OK"
            << std::endl;
  return 0;
}
