// Reactive egress -- core invariants and transport-attachment reactivity
// (`#lzegress`).
//
// Mirrors the `#[cfg(test)]` tails of lazily-rs `src/egress_core.rs`,
// `src/egress.rs`, `src/thread_safe_egress.rs`, and `src/async_egress.rs`. The
// cross-language corpus replay lives in test_egress_conformance.cpp; this file
// pins what the corpus cannot name: the ONE-Effect-per-attachment transport
// seam, that a reconnect makes the old attachment inert at the fence, that an
// acknowledgement reopens the window with no request/ack handshake, and that the
// async attachment delivers off the owner thread while its readers stay
// synchronous.

#include <lazily/egress.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "test_require.hpp"

using lazily::AsyncContext;
using lazily::AsyncEgressAttachment;
using lazily::AsyncEgressCell;
using lazily::Compute;
using lazily::Context;
using lazily::Effect;
using lazily::EgressAckKind;
using lazily::EgressCell;
using lazily::EgressClaimKind;
using lazily::EgressConfigError;
using lazily::EgressConfigException;
using lazily::EgressCore;
using lazily::EgressEnvelope;
using lazily::EgressFailureKind;
using lazily::EgressPolicy;
using lazily::EgressReconnectKind;
using lazily::EgressRetryActionKind;
using lazily::EgressTransport;
using lazily::ThreadSafeContext;
using lazily::ThreadSafeEgressCell;

namespace {

EgressPolicy small_policy() {
  EgressPolicy policy;
  policy.inflight_limit = 2;
  policy.retry_budget = 1;
  policy.retry_base = 5;
  policy.retry_ceiling = 20;
  return policy;
}

/// Records accepted sends; optionally refuses the next one. Thread-safe and
/// waitable, so the async attachment can be observed without sleeping.
struct RecordingTransport : EgressTransport<int> {
  mutable std::mutex mutex;
  std::condition_variable changed;
  std::vector<std::uint64_t> sent;
  std::vector<std::thread::id> threads;
  bool fail_next = false;

  bool send(const EgressEnvelope<int>& envelope) override {
    std::lock_guard<std::mutex> lock(mutex);
    if (fail_next) {
      fail_next = false;
      changed.notify_all();
      return false;
    }
    sent.push_back(envelope.sequence);
    threads.push_back(std::this_thread::get_id());
    changed.notify_all();
    return true;
  }

  std::vector<std::uint64_t> snapshot() const {
    std::lock_guard<std::mutex> lock(mutex);
    return sent;
  }

  /// Wait (bounded) until at least `count` sends were accepted. A bound, not a
  /// sleep: it returns the moment the condition holds.
  bool wait_for(std::size_t count) {
    std::unique_lock<std::mutex> lock(mutex);
    return changed.wait_for(lock, std::chrono::seconds(30), [&] { return sent.size() >= count; });
  }
};

// ── Core ────────────────────────────────────────────────────────────────────

void ack_is_monotone_and_reconnect_fences_old_attachment() {
  EgressCore<std::string> core(7, small_policy());
  core.enqueue("a");
  core.enqueue("b");
  REQUIRE(core.claim(7).second.kind == EgressClaimKind::Claimed, "claim");
  const auto advanced = core.ack(7, 0).second;
  REQUIRE(advanced.kind == EgressAckKind::Advanced && advanced.through == 0u, "ack advances");
  REQUIRE(core.ack(7, 0).second.kind == EgressAckKind::Unchanged, "a repeated ack is unchanged");
  const auto reconnect = core.reconnect(8).second;
  REQUIRE(reconnect.kind == EgressReconnectKind::Advanced && reconnect.generation == 8,
          "reconnect advances");
  const auto stale = core.claim(7).second;
  REQUIRE(stale.kind == EgressClaimKind::StaleGeneration && stale.current == 8,
          "the old attachment is fenced");
  REQUIRE(core.acked_through() == 0u, "the watermark survives a reconnect");
}

void retry_budget_is_bounded() {
  EgressCore<int> core(1, small_policy());
  core.enqueue(1);
  const auto first = core.claim(1).second;
  REQUIRE(first.kind == EgressClaimKind::Claimed, "first claim");
  REQUIRE(core.fail(1, first.envelope->sequence).second.kind == EgressFailureKind::Retrying,
          "the first failure is within budget");
  const auto scheduled = core.retry_now(1, first.envelope->sequence).second;
  REQUIRE(scheduled.kind == EgressRetryActionKind::Scheduled && scheduled.sequence == 0,
          "retry_now reschedules");
  const auto second = core.claim(1).second;
  REQUIRE(second.kind == EgressClaimKind::Claimed && second.envelope->attempt == 2,
          "the retry claim is attempt 2");
  REQUIRE(core.fail(1, second.envelope->sequence).second.kind == EgressFailureKind::Exhausted,
          "past the budget a failure is terminal");
  REQUIRE(core.pending().empty(), "an exhausted record does not return to pending");
}

void backoff_saturates_at_the_ceiling() {
  EgressPolicy policy;
  policy.retry_budget = 100;
  policy.retry_base = 3;
  policy.retry_ceiling = 40;
  EgressCore<int> core(1, policy);
  core.enqueue(1);
  std::vector<std::uint64_t> backoffs;
  for (int i = 0; i < 6; ++i) {
    const auto claim = core.claim(1).second;
    REQUIRE(claim.kind == EgressClaimKind::Claimed, "claim");
    backoffs.push_back(core.fail(1, 0).second.retry->backoff);
    core.retry_now(1, 0);
  }
  REQUIRE((backoffs == std::vector<std::uint64_t>{3, 6, 12, 24, 40, 40}),
          "backoff doubles and clamps at the ceiling");
}

void unsound_policies_are_refused() {
  const auto refuses = [](EgressPolicy policy, EgressConfigError want) {
    try {
      EgressCore<int> core(1, policy);
    } catch (const EgressConfigException& error) {
      return error.error == want;
    }
    return false;
  };
  EgressPolicy zero_window;
  zero_window.inflight_limit = 0;
  REQUIRE(refuses(zero_window, EgressConfigError::ZeroInflightLimit), "zero window refused");
  EgressPolicy zero_base;
  zero_base.retry_base = 0;
  REQUIRE(refuses(zero_base, EgressConfigError::ZeroRetryBase), "zero base refused");
  EgressPolicy inverted;
  inverted.retry_base = 50;
  inverted.retry_ceiling = 10;
  REQUIRE(refuses(inverted, EgressConfigError::RetryCeilingBelowBase), "inverted ceiling refused");
}

// ── Single-threaded shell ───────────────────────────────────────────────────

void one_effect_projects_pending_to_transport_and_inflight() {
  Context ctx;
  EgressCell<int> egress(ctx, 1);
  auto transport = std::make_shared<RecordingTransport>();
  const Effect effect = egress.attach_transport(ctx, transport);
  egress.enqueue(ctx, 10);
  egress.enqueue(ctx, 20);
  REQUIRE((transport->snapshot() == std::vector<std::uint64_t>{0, 1}), "both sent in order");
  REQUIRE(egress.pending(ctx).empty(), "nothing left pending");
  REQUIRE(egress.inflight(ctx).size() == 2, "both in flight");
  ctx.dispose_effect(effect);
}

void reconnect_makes_old_attachment_inert() {
  Context ctx;
  EgressCell<int> egress(ctx, 3);
  auto old_transport = std::make_shared<RecordingTransport>();
  const Effect old_effect = egress.attach_transport(ctx, old_transport);
  egress.enqueue(ctx, 10);
  egress.reconnect(ctx, 4);
  egress.enqueue(ctx, 20);
  REQUIRE((old_transport->snapshot() == std::vector<std::uint64_t>{0}),
          "the old attachment sends nothing after the fence");
  auto fresh = std::make_shared<RecordingTransport>();
  const Effect fresh_effect = egress.attach_transport(ctx, fresh);
  REQUIRE((fresh->snapshot() == std::vector<std::uint64_t>{0, 1}),
          "the replacement replays the unacknowledged record, then the new one");
  ctx.dispose_effect(old_effect);
  ctx.dispose_effect(fresh_effect);
}

void ack_reopens_the_window_without_a_request_ack_handshake() {
  Context ctx;
  EgressPolicy policy;
  policy.inflight_limit = 1;
  EgressCell<int> egress(ctx, 1, policy);
  auto transport = std::make_shared<RecordingTransport>();
  const Effect effect = egress.attach_transport(ctx, transport);
  egress.enqueue(ctx, 10);
  egress.enqueue(ctx, 20);
  REQUIRE((transport->snapshot() == std::vector<std::uint64_t>{0}), "window of one");
  REQUIRE(egress.pending(ctx).size() == 1, "one still pending");
  REQUIRE(egress.ack(ctx, 1, 0).kind == EgressAckKind::Advanced, "ack advances");
  REQUIRE((transport->snapshot() == std::vector<std::uint64_t>{0, 1}),
          "the ack alone re-ran the attachment");
  REQUIRE(egress.pending(ctx).empty(), "nothing left pending");
  ctx.dispose_effect(effect);
}

void a_refused_send_parks_the_record() {
  Context ctx;
  EgressCell<int> egress(ctx, 1, small_policy());
  auto transport = std::make_shared<RecordingTransport>();
  transport->fail_next = true;
  const Effect effect = egress.attach_transport(ctx, transport);
  egress.enqueue(ctx, 10);
  REQUIRE(transport->snapshot().empty(), "the refused send is not recorded");
  const auto retry = egress.retry(ctx);
  REQUIRE(retry.has_value() && retry->sequence == 0 && retry->attempt == 1 && !retry->exhausted,
          "the refusal is parked as a retry");
  REQUIRE(egress.retry_now(ctx, 1, 0).kind == EgressRetryActionKind::Scheduled, "rescheduled");
  REQUIRE((transport->snapshot() == std::vector<std::uint64_t>{0}),
          "rescheduling re-ran the attachment, which resent it");
  ctx.dispose_effect(effect);
}

void reader_kinds_invalidate_independently() {
  Context ctx;
  EgressCell<int> egress(ctx, 1);
  int pending_runs = 0;
  int ack_runs = 0;
  const auto pending_handle = egress.pending_handle();
  const auto ack_handle = egress.acked_through_handle();
  const Effect pending_effect = ctx.effect_void([&pending_runs, pending_handle](Compute& c) {
    (void)c.get(pending_handle);
    ++pending_runs;
  });
  const Effect ack_effect = ctx.effect_void([&ack_runs, ack_handle](Compute& c) {
    (void)c.get(ack_handle);
    ++ack_runs;
  });
  egress.enqueue(ctx, 1);
  REQUIRE(pending_runs == 2, "an enqueue reruns the pending reader's effect");
  REQUIRE(ack_runs == 1, "an enqueue does NOT rerun the watermark reader's effect");
  ctx.dispose_effect(pending_effect);
  ctx.dispose_effect(ack_effect);
}

// ── Thread-safe shell ───────────────────────────────────────────────────────

void thread_safe_attachment_drives_the_shared_projection() {
  ThreadSafeContext ctx;
  ThreadSafeEgressCell<int> egress(ctx, 1);
  auto transport = std::make_shared<RecordingTransport>();
  const Effect effect = egress.attach_transport(ctx, transport);
  egress.enqueue(ctx, 7);
  REQUIRE((transport->snapshot() == std::vector<std::uint64_t>{0}), "sent");
  REQUIRE(egress.inflight(ctx).size() == 1, "in flight");
  ctx.dispose_effect(effect);
}

void thread_safe_concurrent_producers_get_unique_ordered_sequences() {
  ThreadSafeContext ctx;
  EgressPolicy policy;
  policy.inflight_limit = 4096;
  ThreadSafeEgressCell<int> egress(ctx, 1, policy);
  auto transport = std::make_shared<RecordingTransport>();
  const Effect effect = egress.attach_transport(ctx, transport);
  constexpr int kThreads = 4;
  constexpr int kPerThread = 100;
  std::vector<std::thread> producers;
  for (int t = 0; t < kThreads; ++t)
    producers.emplace_back([&egress, &ctx, t] {
      for (int i = 0; i < kPerThread; ++i)
        egress.enqueue(ctx, t * kPerThread + i);
    });
  for (auto& producer : producers)
    producer.join();
  const auto sent = transport->snapshot();
  REQUIRE(sent.size() == static_cast<std::size_t>(kThreads * kPerThread),
          "every enqueued record was sent exactly once");
  for (std::size_t i = 0; i < sent.size(); ++i)
    REQUIRE(sent[i] == i, "the single attachment sends in sequence order");
  REQUIRE(egress.next_sequence() == static_cast<std::uint64_t>(kThreads * kPerThread),
          "sequence assignment is monotone across producers");
  ctx.dispose_effect(effect);
}

// ── Async shell ─────────────────────────────────────────────────────────────

void async_attachment_delivers_off_the_owner_thread() {
  AsyncContext ctx;
  AsyncEgressCell<int> egress(ctx, 1);
  auto transport = std::make_shared<RecordingTransport>();
  {
    AsyncEgressAttachment attachment = egress.attach_transport(ctx, transport);
    egress.enqueue(ctx, 7);
    egress.enqueue(ctx, 8);
    REQUIRE(transport->wait_for(2), "the async attachment delivered both records");
    REQUIRE((transport->snapshot() == std::vector<std::uint64_t>{0, 1}), "in sequence order");
    {
      std::lock_guard<std::mutex> lock(transport->mutex);
      for (const auto id : transport->threads)
        REQUIRE(id != std::this_thread::get_id(), "delivery ran off the owner thread");
    }
    // The readers stay synchronous: no await, no settle -- a plain read.
    REQUIRE(egress.inflight(ctx).size() == 2, "both in flight");
    REQUIRE(egress.pending(ctx).empty(), "nothing pending");
  }
  // The attachment is gone (tracker disposed, worker joined): a new enqueue
  // stays pending.
  egress.enqueue(ctx, 9);
  REQUIRE(egress.pending(ctx).size() == 1, "a disposed attachment delivers nothing");
  REQUIRE(transport->snapshot().size() == 2, "nothing more was sent");
}

void async_ack_reopens_the_window() {
  AsyncContext ctx;
  EgressPolicy policy;
  policy.inflight_limit = 1;
  AsyncEgressCell<int> egress(ctx, 1, policy);
  auto transport = std::make_shared<RecordingTransport>();
  AsyncEgressAttachment attachment = egress.attach_transport(ctx, transport);
  egress.enqueue(ctx, 10);
  egress.enqueue(ctx, 20);
  REQUIRE(transport->wait_for(1), "first record delivered");
  REQUIRE(egress.ack(ctx, 1, 0).kind == EgressAckKind::Advanced, "ack advances");
  REQUIRE(transport->wait_for(2), "the ack re-ran the attachment");
  REQUIRE((transport->snapshot() == std::vector<std::uint64_t>{0, 1}), "in order");
  attachment.reset();
}

} // namespace

int main() {
  ack_is_monotone_and_reconnect_fences_old_attachment();
  retry_budget_is_bounded();
  backoff_saturates_at_the_ceiling();
  unsound_policies_are_refused();

  one_effect_projects_pending_to_transport_and_inflight();
  reconnect_makes_old_attachment_inert();
  ack_reopens_the_window_without_a_request_ack_handshake();
  a_refused_send_parks_the_record();
  reader_kinds_invalidate_independently();

  thread_safe_attachment_drives_the_shared_projection();
  thread_safe_concurrent_producers_get_unique_ordered_sequences();

  async_attachment_delivers_off_the_owner_thread();
  async_ack_reopens_the_window();

  std::cout << "egress: core invariants + attachment reactivity x 3 flavours OK" << std::endl;
  return 0;
}
