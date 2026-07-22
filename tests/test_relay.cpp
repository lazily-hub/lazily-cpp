// RelayCell Phases 2–6 spike (#relaycell) for the header-only C++ binding. Proves
// the operational invariants — relay_converges, transport_independent,
// spill_lossless, spill_replay_idempotent — plus overflow behaviour, roles, and
// the Phase-6 policies. C++ converges identically to lazily-rs/kt/js.

#include <lazily/lazily.hpp>

#include <algorithm>
#include <cassert>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace lazily;

static int test_count = 0;
static int test_passed = 0;

#define TEST(name)                     \
  static void name();                  \
  struct name##_runner {               \
    name##_runner() {                  \
      ++test_count;                    \
      name();                          \
      ++test_passed;                   \
    }                                  \
  } name##_instance;                   \
  static void name()

// Build a wide-open relay (default effectively-unbounded high_water) unless the
// caller narrows it — mirrors the js `relay()` helper.
template <typename Policy>
static RelayCell<long, Policy> make_relay(Context& ctx, std::uint64_t high_water = 1000000,
                                          Overflow overflow = Overflow::Conflate) {
  return RelayCell<long, Policy>(
      ctx, BackpressurePolicy(ctx, BoundDim::Count, high_water, high_water / 2, overflow));
}

// -- Phase 2 -----------------------------------------------------------------

template <typename Policy>
static void converged_egress_case() {
  std::vector<long> ops{3, 1, 4, 1, 5, 9, 2, 6};
  long flat = ops[0];
  for (std::size_t i = 1; i < ops.size(); ++i) flat = Policy::template merge<long>(flat, ops[i]);

  // drain-every schedule
  Context ctxA;
  auto rA = make_relay<Policy>(ctxA);
  std::optional<long> accA;
  for (long op : ops) {
    rA.ingress(op);
    auto d = rA.drain();
    if (!d.has_value()) continue;
    accA = accA.has_value() ? std::optional<long>(Policy::template merge<long>(accA.value(), d.value()))
                            : d;
  }
  assert(accA.has_value() && accA.value() == flat);

  // drain-once schedule
  Context ctxB;
  auto rB = make_relay<Policy>(ctxB);
  for (long op : ops) rB.ingress(op);
  auto once = rB.drain();
  assert(once.has_value() && once.value() == flat);
}

TEST(test_converged_egress_independent_of_drain_schedule) {
  converged_egress_case<Sum>();
  converged_egress_case<Max>();
}

TEST(test_reactive_depth_isfull_isempty) {
  Context ctx;
  auto r = make_relay<Sum>(ctx, 3);
  assert(r.is_empty_value());
  assert(r.depth_value() == 0);
  assert(!r.is_full_value());

  r.ingress(1);
  r.ingress(1);
  assert(!r.is_empty_value());
  assert(r.depth_value() == 2);
  assert(!r.is_full_value());

  r.ingress(1);
  assert(r.depth_value() == 3);
  assert(r.is_full_value());

  r.drain();
  assert(r.is_empty_value());
  assert(r.depth_value() == 0);
}

TEST(test_reactive_readers_via_effect) {
  // The depth/is_full/is_empty slots drive an effect (demand-driven reactivity).
  Context ctx;
  auto r = make_relay<Sum>(ctx, 2);
  auto is_full = r.is_full();
  int fulls = 0;
  ctx.effect([&, is_full](Compute& c) -> CleanupFn {
    if (c.get(is_full)) ++fulls;
    return {};
  });
  assert(fulls == 0);
  r.ingress(1);
  r.ingress(1);  // depth 2 == high_water → is_full flips true, effect fires
  assert(fulls == 1);
  r.drain();     // is_full false again
  assert(fulls == 1);
}

TEST(test_block_overflow_refuses_ingress) {
  Context ctx;
  auto r = make_relay<Sum>(ctx, 2, Overflow::Block);
  assert(r.ingress(1) == IngressOutcome::Accepted);
  assert(r.ingress(1) == IngressOutcome::Conflated);
  assert(r.ingress(1) == IngressOutcome::Blocked);
  assert(r.drain().value() == 2);
}

TEST(test_drop_newest_and_drop_oldest) {
  Context ctxN;
  auto rn = make_relay<Sum>(ctxN, 2, Overflow::DropNewest);
  rn.ingress(1);
  rn.ingress(1);
  assert(rn.ingress(9) == IngressOutcome::Dropped);
  assert(rn.drain().value() == 2);

  Context ctxO;
  auto ro = make_relay<Sum>(ctxO, 2, Overflow::DropOldest);
  ro.ingress(1);
  ro.ingress(1);
  assert(ro.ingress(9) == IngressOutcome::Dropped);
  assert(ro.drain().value() == 9);
}

TEST(test_construction_rejects_conflate_for_rawfifo) {
  Context ctx;
  bool threw = false;
  try {
    RelayCell<std::vector<long>, RawFifo> bad(
        ctx, BackpressurePolicy(ctx, BoundDim::Count, 4, 2, Overflow::Conflate));
    (void)bad;
  } catch (const RelayConfigException& e) {
    threw = (e.error == RelayConfigError::ConflateNotBounding);
  }
  assert(threw);

  // A conflating policy (Sum) with Conflate is legal.
  auto ok = make_relay<Sum>(ctx, 4, Overflow::Conflate);
  assert(ok.overflow_is_legal());
}

// -- Phase 3 -----------------------------------------------------------------

TEST(test_spill_lossless_both_modes) {
  for (SpillMode mode : {SpillMode::CompactOnWrite, SpillMode::AppendCompact}) {
    SpillStore<long, Sum> store(mode, 2);
    std::vector<long> windows{1, 2, 3, 4, 5};
    for (long w : windows) store.spill(w, 1);
    long hot = 10;
    long flat = hot;
    for (long w : windows) flat += w;
    assert(store.reconstruct(0, std::optional<long>(hot)) == flat);
  }
}

TEST(test_spill_replay_idempotent) {
  SpillStore<long, Max> store(SpillMode::AppendCompact, 1);
  for (long w : {3L, 7L, 5L}) store.spill(w, 1);
  long once = store.replay_unacked(0);
  long twice = store.replay_unacked(once);
  assert(once == twice);
  assert(once == 7);
}

TEST(test_compact_on_write_bounds_and_ack_reclaims) {
  SpillStore<long, Sum> store(SpillMode::CompactOnWrite, 2);
  for (int i = 0; i < 5; ++i) store.spill(1, 1);  // page size 2 → 3 pages
  assert(store.page_count() == 3);
  auto first_id = store.manifest()[0].first;
  store.ack_through(first_id);
  assert(store.pending_pages().size() == 2);
  store.reclaim();
  assert(store.page_count() == 2);
}

TEST(test_relay_spill_overflow_end_to_end) {
  // A Spill-overflow relay degrades to Conflate merging in Phase 2; pairing it
  // with a SpillStore reconstructs the flat fold losslessly.
  Context ctx;
  auto r = make_relay<Sum>(ctx, 3, Overflow::Spill);
  SpillStore<long, Sum> store(SpillMode::AppendCompact, 4);
  std::vector<long> ops{1, 2, 3, 4, 5, 6, 7};
  long flat = 0;
  for (long op : ops) {
    flat += op;
    r.ingress(op);
    if (r.is_full_value()) {
      auto w = r.drain();
      if (w.has_value()) store.spill(w.value(), 1);
    }
  }
  auto tail = r.drain();
  assert(store.reconstruct(0, tail) == flat);
}

// -- Phase 4 -----------------------------------------------------------------

template <typename Policy>
static void transport_case() {
  std::vector<long> ops{3, 1, 4, 1, 5, 9};
  long flat = ops[0];
  for (std::size_t i = 1; i < ops.size(); ++i) flat = Policy::template merge<long>(flat, ops[i]);

  std::vector<std::unique_ptr<Transport<long>>> transports;
  transports.push_back(std::make_unique<InProcTransport<long>>());
  transports.push_back(std::make_unique<FramedTransport<long>>(2));
  transports.push_back(std::make_unique<FramedTransport<long>>(3));

  for (auto& transport : transports) {
    for (long op : ops) transport->deliver(op);
    Context ctx;
    auto r = make_relay<Policy>(ctx);
    while (transport->has_pending()) {
      for (long op : transport->poll()) r.ingress(op);
    }
    assert(r.drain().value() == flat);
  }
}

TEST(test_transport_independent_across_framing) {
  transport_case<Sum>();
  transport_case<Max>();
  transport_case<KeepLatest>();
}

// -- Phase 5 -----------------------------------------------------------------

TEST(test_outbox_conflates_state_broadcast) {
  Context ctx;
  Outbox<long, KeepLatest> out(ctx, 8);
  out.send(1);
  out.send(2);
  out.send(3);
  assert(out.drain().value() == 3);
}

TEST(test_outbox_block_backpressures_producer) {
  Context ctx;
  Outbox<long, Sum> out(ctx, 2, BoundDim::Count, Overflow::Block);
  assert(out.send(1) == IngressOutcome::Accepted);
  assert(out.send(1) == IngressOutcome::Conflated);
  auto is_full = out.is_full();
  assert(ctx.get(is_full));
  assert(out.send(9) == IngressOutcome::Blocked);
}

TEST(test_inbox_credit_meters_remote) {
  Context ctx;
  Inbox<long, Sum> inbox(ctx, 100, 2);
  assert(inbox.ready());
  inbox.receive(5);
  inbox.receive(5);
  assert(!inbox.ready());
  assert(inbox.consume(2).value() == 10);
  assert(inbox.ready());
}

TEST(test_outbox_inbox_link_converges) {
  Context ctx;
  Outbox<long, Sum> out(ctx, 64);
  Inbox<long, Sum> inbox(ctx, 64, 64);
  InProcTransport<long> transport;
  std::vector<long> ops{1, 2, 3, 4};
  for (long op : ops) out.send(op);
  transport.deliver(out.drain().value());
  while (transport.has_pending()) {
    for (long frame : transport.poll()) inbox.receive(frame);
  }
  long sum = 0;
  for (long op : ops) sum += op;
  assert(inbox.consume(64).value() == sum);
}

// -- Phase 6 -----------------------------------------------------------------

TEST(test_rate_policy_token_bucket) {
  RatePolicy rate(2, 1);
  assert(rate.try_egress());
  assert(rate.try_egress());
  assert(!rate.try_egress());
  rate.tick();
  assert(rate.try_egress());
}

TEST(test_window_policy_flush_on_fill_and_tick) {
  WindowPolicy window(3);
  assert(!window.on_ingress());
  assert(!window.on_ingress());
  assert(window.on_ingress());
  assert(!window.on_ingress());
  assert(window.tick());
  assert(!window.tick());
}

TEST(test_expiry_policy_drops_aged) {
  ExpiryPolicy expiry(5);
  expiry.advance(10);
  std::vector<std::pair<std::uint64_t, std::string>> batch{
      {3, "old"}, {7, "fresh"}, {10, "now"}};
  auto live = expiry.retain_live(batch);
  assert((live == std::vector<std::string>{"fresh", "now"}));
}

TEST(test_priority_storage_pops_highest_fifo_within) {
  PriorityStorage<std::string> pq;
  pq.push(1, "low");
  pq.push(3, "highA");
  pq.push(2, "mid");
  pq.push(3, "highB");
  assert(pq.pop().value() == "highA");
  assert(pq.pop().value() == "highB");
  assert(pq.pop().value() == "mid");
  assert(pq.pop().value() == "low");
  assert(!pq.pop().has_value());
}

TEST(test_keyed_relay_shards_per_key) {
  Context ctx;
  KeyedRelay<std::string, long, Sum> keyed(ctx, 64, Overflow::Conflate);
  keyed.ingress("a", 1);
  keyed.ingress("b", 10);
  keyed.ingress("a", 2);
  assert(keyed.drain("a").value() == 3);
  assert(keyed.drain("b").value() == 10);
  auto ks = keyed.keys();
  std::sort(ks.begin(), ks.end());
  assert((ks == std::vector<std::string>{"a", "b"}));
}

int main() {
  std::cout << "test_relay: " << test_passed << "/" << test_count << " passed\n";
  return test_passed == test_count ? 0 : 1;
}
