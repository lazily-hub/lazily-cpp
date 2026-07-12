// Reliable sync conformance (`#lzsync`, `#sync-driver`).
//
// Replays the canonical `lazily-spec/conformance/reliable-sync/*.json` fixtures —
// the language-agnostic conformance every binding MUST validate (`lazily-spec/
// protocol.md` § "Reliable Sync", proved in `lazily-formal` `ReliableSync.lean`):
//
//   resync_gap_converge.json     — ResyncCoordinator gap recovery + convergence
//   idempotent_redelivery.json   — re-delivered deltas Ignored (exactly-once effect)
//   multi_epoch_delta.json       — span-N delta apply == unit fold
//   outbox_replay_after_crash.json — DurableOutbox at-least-once replay + send-failure retain
//   liveness_orset_lww.json      — OR-set / LWW liveness cells + derived aggregate
//
// The scenarios are transcribed by hand, the same fixture-mirroring pattern the
// other conformance tests use.

#include <lazily/lazily.hpp>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace lazily;

static int test_count = 0;
static int test_passed = 0;

#define TEST(name)                                        \
  static void name();                                     \
  struct name##_runner {                                  \
    name##_runner() {                                     \
      ++test_count;                                       \
      name();                                             \
      ++test_passed;                                      \
    }                                                     \
  } name##_instance;                                      \
  static void name()

// -- Tiny graph-state model: fold Delta/Snapshot into node -> bytes --

struct GraphModel {
  std::map<NodeId, std::vector<uint8_t>> nodes;

  static std::vector<uint8_t> inline_bytes(const IpcValue& v) {
    return std::get<IpcValueInline>(v).bytes;
  }
  static std::vector<uint8_t> state_bytes(const NodeState& s) {
    return std::get<NodeStatePayload>(s).bytes;
  }

  void apply_delta(const Delta& d) {
    for (const auto& op : d.ops) {
      if (std::holds_alternative<DeltaOpCellSet>(op)) {
        const auto& o = std::get<DeltaOpCellSet>(op);
        nodes[o.node] = inline_bytes(o.payload);
      } else if (std::holds_alternative<DeltaOpSlotValue>(op)) {
        const auto& o = std::get<DeltaOpSlotValue>(op);
        nodes[o.node] = inline_bytes(o.payload);
      }
    }
  }
  void apply_snapshot(const Snapshot& s) {
    nodes.clear();
    for (const auto& n : s.nodes) nodes[n.node] = state_bytes(n.state);
  }
};

static DeltaOp cellset(NodeId n, std::vector<uint8_t> b) {
  return DeltaOpCellSet{n, IpcValueInline{std::move(b)}};
}
static DeltaOp slotvalue(NodeId n, std::vector<uint8_t> b) {
  return DeltaOpSlotValue{n, IpcValueInline{std::move(b)}};
}
static Delta mk_delta(Epoch base, Epoch epoch, std::vector<DeltaOp> ops) {
  return Delta{base, epoch, std::move(ops)};
}
static NodeSnapshot node_snap(NodeId n, std::vector<uint8_t> b) {
  return NodeSnapshot{n, "u64", NodeStatePayload{std::move(b)}, std::nullopt};
}

// ── resync_gap_converge.json ─────────────────────────────────────────────────

// drop_suffix_then_resync_converges: receiver misses delta 2->3, detects the gap
// on 3->4, emits ResyncRequest{from:2}, applies the covering Snapshot{epoch:4},
// and reaches the SAME graph as a receiver that saw every delta.
TEST(test_conformance_resync_drop_suffix_converges) {
  ResyncCoordinator coord(1);
  GraphModel a;

  // delta 1->2 : Apply
  Delta d12 = mk_delta(1, 2, {cellset(1, {10})});
  auto act = coord.ingest_delta(d12);
  assert(act.is_apply());
  a.apply_delta(d12);
  assert(coord.last_epoch() == 2);

  // delta 2->3 dropped (never arrives at A)

  // delta 3->4 : base_epoch 3 > last_epoch 2 -> RequestSnapshot{from:2}
  int resync_requests = 0;
  Delta d34 = mk_delta(3, 4, {cellset(3, {30})});
  act = coord.ingest_delta(d34);
  assert(act.is_request_snapshot());
  assert(act.from_epoch == 2);
  resync_requests += 1;
  assert(coord.last_epoch() == 2);  // unchanged

  // covering Snapshot{epoch:4} : Apply
  Snapshot snap{4, {node_snap(1, {10}), node_snap(2, {20}), node_snap(3, {30})}, {}, {1, 2, 3}};
  act = coord.ingest_snapshot(snap.epoch);
  assert(act.is_apply());
  a.apply_snapshot(snap);
  assert(coord.last_epoch() == 4);

  // Receiver B saw every delta 1->2->3->4.
  GraphModel b;
  b.apply_delta(mk_delta(1, 2, {cellset(1, {10})}));
  b.apply_delta(mk_delta(2, 3, {cellset(2, {20})}));
  b.apply_delta(mk_delta(3, 4, {cellset(3, {30})}));

  assert(resync_requests == 1);
  assert((a.nodes == std::map<NodeId, std::vector<uint8_t>>{{1, {10}}, {2, {20}}, {3, {30}}}));
  assert(a.nodes == b.nodes);  // equals_no_drop_receiver
}

// single_request_per_gap: while resyncing, further ahead-of-cursor deltas are
// Ignored and do NOT emit duplicate ResyncRequests.
TEST(test_conformance_resync_single_request_per_gap) {
  ResyncCoordinator coord(2);
  int resync_requests = 0;

  auto act = coord.ingest_delta(mk_delta(3, 4, {}));
  assert(act.is_request_snapshot() && act.from_epoch == 2);
  resync_requests += 1;

  act = coord.ingest_delta(mk_delta(4, 5, {}));
  assert(act.is_ignore());  // resyncing — suppress duplicate request
  assert(coord.last_epoch() == 2);

  act = coord.ingest_delta(mk_delta(5, 6, {}));
  assert(act.is_ignore());
  assert(coord.last_epoch() == 2);

  act = coord.ingest_snapshot(6);
  assert(act.is_apply());
  assert(coord.last_epoch() == 6);

  assert(resync_requests == 1);
}

// ── idempotent_redelivery.json ───────────────────────────────────────────────

// replayed_delta_is_ignored: a re-delivered delta 40->41 (base_epoch 40 < 42) is
// Ignored; net state and last_epoch unchanged.
TEST(test_conformance_idempotent_replayed_delta_ignored) {
  ResyncCoordinator coord(42);
  GraphModel g;
  g.nodes[1] = {10};

  Delta redeliver = mk_delta(40, 41, {cellset(1, {99})});
  auto act = coord.ingest_delta(redeliver);
  assert(act.is_ignore());  // base_epoch_below_last_epoch_already_applied
  assert(coord.last_epoch() == 42);
  // Ignored -> the caller does NOT fold; state stays put.
  assert((g.nodes == std::map<NodeId, std::vector<uint8_t>>{{1, {10}}}));

  // OutboxAck wire frame advertises through_epoch = 42.
  IpcMessage ack = coord.ack();
  assert(std::holds_alternative<IpcMessageOutboxAck>(ack));
  assert(std::get<IpcMessageOutboxAck>(ack).value.through_epoch == 42);
}

// duplicate_current_head_is_ignored: an exact re-delivery of the last-applied
// delta is also Ignored — a duplicate never double-applies.
TEST(test_conformance_idempotent_duplicate_head_ignored) {
  ResyncCoordinator coord(41);
  auto act = coord.ingest_delta(mk_delta(40, 41, {cellset(1, {10})}));
  assert(act.is_ignore());
  assert(coord.last_epoch() == 41);
}

// ── multi_epoch_delta.json ───────────────────────────────────────────────────

// span_3_applies_equal_to_unit_fold: one span-3 delta reaches the same graph and
// last_epoch as three unit deltas carrying the same ops in order.
TEST(test_conformance_multi_epoch_apply_eq_fold) {
  Delta span3 = mk_delta(40, 43, {cellset(1, {10}), cellset(2, {20}), slotvalue(3, {30})});

  // assertions block
  assert(span3.base_epoch == 40);
  assert(span3.epoch == 43);
  assert(span3.epoch - span3.base_epoch == 3);          // span
  assert(span3.epoch > span3.base_epoch + 1);           // is_multi_epoch
  assert(span3.ops.size() == 3);                        // op_count

  ResyncCoordinator coord(40);
  GraphModel batch;
  auto act = coord.ingest_delta(span3);
  assert(act.is_apply());
  batch.apply_delta(span3);
  assert(coord.last_epoch() == 43);  // atomic advance

  // Equivalent unit fold.
  ResyncCoordinator unit_coord(40);
  GraphModel unit;
  for (const auto& d : {mk_delta(40, 41, {cellset(1, {10})}),
                        mk_delta(41, 42, {cellset(2, {20})}),
                        mk_delta(42, 43, {slotvalue(3, {30})})}) {
    auto a = unit_coord.ingest_delta(d);
    assert(a.is_apply());
    unit.apply_delta(d);
  }
  assert(unit_coord.last_epoch() == 43);
  assert(batch.nodes == unit.nodes);  // fold_equivalent
}

// gap_rule_unchanged_under_span: a span-3 delta whose base_epoch != last_epoch is
// still a gap; the span does not relax gap detection.
TEST(test_conformance_multi_epoch_gap_rule_unchanged) {
  ResyncCoordinator coord(39);
  auto act = coord.ingest_delta(mk_delta(40, 43, {}));
  assert(act.is_request_snapshot());
  assert(act.from_epoch == 39);
  assert(coord.last_epoch() == 39);  // unchanged
}

// ── outbox_replay_after_crash.json ───────────────────────────────────────────

// crash_between_append_and_ack_replays_on_reconnect: appended 41,42,43; peer acks
// through 41; on reconnect replay_from(41) re-sends 42,43 in order; receiver
// applies both -> last_epoch 43. Exactly-once effect: none lost, none doubled.
TEST(test_conformance_outbox_replay_after_crash) {
  InMemoryOutbox outbox;
  outbox.append(41, IpcMessageDelta{mk_delta(40, 41, {cellset(1, {10})})});
  outbox.append(42, IpcMessageDelta{mk_delta(41, 42, {cellset(2, {20})})});
  outbox.append(43, IpcMessageDelta{mk_delta(42, 43, {cellset(3, {30})})});

  outbox.ack_through(41);
  assert((outbox.retained_epochs() == std::vector<Epoch>{42, 43}));  // retained_after_ack

  auto replay = outbox.replay_from(41);  // reconnect cursor = 41
  assert(replay.size() == 2);
  assert(replay[0].first == 42 && replay[1].first == 43);  // replay_order

  ResyncCoordinator coord(41);
  GraphModel g;
  std::vector<Epoch> applied;
  for (const auto& e : replay) {
    auto act = coord.ingest(e.second);
    assert(act.is_apply());
    g.apply_delta(std::get<IpcMessageDelta>(e.second).value);
    applied.push_back(e.first);
  }
  assert((applied == std::vector<Epoch>{42, 43}));  // receiver_applies
  assert(coord.last_epoch() == 43);
}

// send_failure_retains_frame_for_next_tick: a send error does not lose the frame
// (append succeeded, send failed) — it stays in the outbox and is retried on a
// later tick. Driven through the SyncDriver loop.
TEST(test_conformance_outbox_send_failure_retains) {
  auto outbox = std::make_shared<InMemoryOutbox>();
  bool fail_next = true;
  IpcSink sink = [&](const IpcMessage&) {
    if (fail_next) { fail_next = false; return false; }
    return true;
  };
  IpcSource source = [&]() -> std::optional<IpcMessage> { return std::nullopt; };
  Clock clock = [] { return int64_t(0); };
  SnapshotProvider provider = [](Epoch) {
    return IpcMessage{IpcMessageSnapshot{Snapshot{0, {}, {}, {}}}};
  };
  SyncDriver driver(sink, source, outbox, clock, provider);

  driver.enqueue(44, IpcMessageDelta{mk_delta(43, 44, {cellset(4, {40})})});

  Progress p1 = driver.tick();
  assert(p1.sent == 0);
  assert(driver.is_stalled());
  assert((outbox->retained_epochs() == std::vector<Epoch>{44}));  // frame_retained_after_failed_send

  driver.on_reconnect();
  Progress p2 = driver.tick();
  assert(p2.sent == 1);  // resent_on_next_tick: [44]
  assert((outbox->retained_epochs() == std::vector<Epoch>{44}));  // still unacked (no permanent gap)
}

// ── liveness_orset_lww.json ──────────────────────────────────────────────────

static WireStamp ws(int64_t wall, int64_t logical, PeerId peer) { return WireStamp{wall, logical, peer}; }

// open_set_add_wins_over_stale_remove: a re-open (add t3) concurrent with a
// lagging close (remove observing only t1) keeps the doc open; order-independent.
TEST(test_conformance_liveness_orset_add_wins) {
  OrSet s;
  s.add("t1");
  s.remove_observed({"t1"});
  s.add("t3");
  assert(s.present());  // add_tag_t3_not_observed_by_remove

  // order_independent: apply in reverse order, same result.
  OrSet r;
  r.add("t3");
  r.add("t1");
  r.remove_observed({"t1"});
  assert(r.present());

  // redeliver_applied_count 0: joining an identical replica changes nothing.
  OrSet before = s;
  s.join(r);
  assert(s.present() == before.present());
}

// lww_alive_highest_stamp_wins: the OS process-exit write (alive=false at higher
// stamp) wins; a stale re-assert (alive=true at lower stamp) is dominated.
TEST(test_conformance_liveness_lww_highest_stamp_wins) {
  WireLwwRegister<bool> alive(ws(20, 0, 1), true);
  alive.set(ws(25, 0, 1), false);
  alive.set(ws(22, 0, 1), true);  // stale — dominated
  assert(alive.value() == false);  // max_stamp resolution

  // order_independent: apply in a different order.
  WireLwwRegister<bool> alive2(ws(22, 0, 1), true);
  alive2.set(ws(20, 0, 1), true);
  alive2.set(ws(25, 0, 1), false);
  assert(alive2.value() == false);
}

// Derived per-doc live aggregate: a doc is live iff some present (doc,pid) has
// alive[pid] == true.
static std::set<std::string> live_docs(
    const std::map<std::string, std::pair<std::string, OrSet>>& open_set,  // key -> (doc, pid) OR-set
    const std::map<std::string, WireLwwRegister<bool>>& alive,             // pid -> alive
    const std::map<std::string, std::string>& key_pid) {                   // key -> pid
  std::set<std::string> docs;
  for (const auto& kv : open_set) {
    if (!kv.second.second.present()) continue;
    const std::string& pid = key_pid.at(kv.first);
    auto it = alive.find(pid);
    if (it != alive.end() && it->second.value()) docs.insert(kv.second.first);
  }
  return docs;
}

// whole_editor_death_cascades: one alive[pid100]=false recomputes the derived
// live aggregate for BOTH docs pid100 held; docC (pid200) unaffected.
TEST(test_conformance_liveness_whole_editor_death_cascades) {
  std::map<std::string, std::pair<std::string, OrSet>> open_set;
  std::map<std::string, std::string> key_pid;
  auto open = [&](const std::string& key, const std::string& doc, const std::string& pid) {
    OrSet s; s.add(key);  // one add tag = present
    open_set[key] = {doc, s};
    key_pid[key] = pid;
  };
  open("docA/pid100", "docA", "100");
  open("docB/pid100", "docB", "100");
  open("docC/pid200", "docC", "200");

  std::map<std::string, WireLwwRegister<bool>> alive;
  alive.emplace("100", WireLwwRegister<bool>(ws(1, 0, 1), true));
  alive.emplace("200", WireLwwRegister<bool>(ws(1, 0, 1), true));

  assert((live_docs(open_set, alive, key_pid) == std::set<std::string>{"docA", "docB", "docC"}));

  // pid100 dies (higher stamp).
  alive.at("100").set(ws(30, 0, 1), false);
  assert((live_docs(open_set, alive, key_pid) == std::set<std::string>{"docC"}));  // cascade
}

// derived_live_doc_aggregate_converges_under_retry: two replicas exchange the same
// liveness ops in different orders + re-delivery; the derived per-doc live
// aggregate converges identically (semilattice join).
TEST(test_conformance_liveness_converges_under_retry) {
  // Replica op set: add docA/pid100, alive[100]=true, add docB/pid100.
  auto build = [](bool reverse) {
    std::map<std::string, std::pair<std::string, OrSet>> open_set;
    std::map<std::string, std::string> key_pid;
    auto open = [&](const std::string& key, const std::string& doc, const std::string& pid,
                    const std::string& tag) {
      OrSet s; s.add(tag);
      open_set[key] = {doc, s};
      key_pid[key] = pid;
    };
    if (!reverse) {
      open("docA/pid100", "docA", "100", "a1");
      open("docB/pid100", "docB", "100", "b1");
    } else {
      open("docB/pid100", "docB", "100", "b1");
      open("docA/pid100", "docA", "100", "a1");
    }
    std::map<std::string, WireLwwRegister<bool>> alive;
    alive.emplace("100", WireLwwRegister<bool>(ws(41, 0, 1), true));
    return live_docs(open_set, alive, key_pid);
  };

  auto r1 = build(false);
  auto r2 = build(true);
  assert(r1 == r2);  // order_independent
  assert((r1 == std::set<std::string>{"docA", "docB"}));  // converged_live_docs; per_doc_isolation
}

// ── wire round-trip: the new control frames survive the codec ────────────────

TEST(test_reliable_sync_wire_roundtrip) {
  // ResyncRequest { from_epoch: 2 }
  IpcMessage rq = ipc_resync_request(2);
  IpcMessage rq_dec = decode(encode(rq));
  assert(std::holds_alternative<IpcMessageResyncRequest>(rq_dec));
  assert(std::get<IpcMessageResyncRequest>(rq_dec).value.from_epoch == 2);

  // OutboxAck { through_epoch: 42 }
  IpcMessage ak = ipc_outbox_ack(42);
  IpcMessage ak_dec = decode(encode(ak));
  assert(std::holds_alternative<IpcMessageOutboxAck>(ak_dec));
  assert(std::get<IpcMessageOutboxAck>(ak_dec).value.through_epoch == 42);
}

// ── full-duplex SyncDriver loop: gap -> ResyncRequest -> Snapshot -> ack ──────

TEST(test_sync_driver_full_duplex_resync) {
  // Inbound queue feeds a receiver driver; outbound frames land in `wire`.
  std::vector<IpcMessage> inbound;
  std::vector<IpcMessage> wire;
  size_t rx = 0;

  IpcSink sink = [&](const IpcMessage& m) { wire.push_back(m); return true; };
  IpcSource source = [&]() -> std::optional<IpcMessage> {
    if (rx < inbound.size()) return inbound[rx++];
    return std::nullopt;
  };
  Clock clock = [] { return int64_t(0); };
  SnapshotProvider provider = [](Epoch from) {
    // Never asked on the receiver side here; return a trivial covering snapshot.
    return IpcMessage{IpcMessageSnapshot{Snapshot{from, {}, {}, {}}}};
  };
  auto outbox = std::make_shared<InMemoryOutbox>();
  SyncDriver driver(sink, source, outbox, clock, provider, /*last_epoch=*/1);
  GraphModel g;

  // Feed: apply 1->2, then a gap 3->4 (should emit ResyncRequest), then Snapshot{4}.
  inbound.push_back(IpcMessageDelta{mk_delta(1, 2, {cellset(1, {10})})});
  inbound.push_back(IpcMessageDelta{mk_delta(3, 4, {cellset(3, {30})})});
  inbound.push_back(IpcMessageSnapshot{Snapshot{4, {node_snap(1, {10}), node_snap(2, {20}), node_snap(3, {30})}, {}, {1, 2, 3}}});

  Progress p = driver.tick();
  for (const auto& m : p.applied) {
    if (std::holds_alternative<IpcMessageDelta>(m)) g.apply_delta(std::get<IpcMessageDelta>(m).value);
    else if (std::holds_alternative<IpcMessageSnapshot>(m)) g.apply_snapshot(std::get<IpcMessageSnapshot>(m).value);
  }

  assert(p.resync_requested);          // gap detected on 3->4
  assert(driver.last_epoch() == 4);    // snapshot adopted
  assert((g.nodes == std::map<NodeId, std::vector<uint8_t>>{{1, {10}}, {2, {20}}, {3, {30}}}));

  // A ResyncRequest{from:2} and an OutboxAck{through:4} crossed the wire.
  bool saw_request = false, saw_ack = false;
  for (const auto& m : wire) {
    if (std::holds_alternative<IpcMessageResyncRequest>(m)) {
      saw_request = true;
      assert(std::get<IpcMessageResyncRequest>(m).value.from_epoch == 2);
    }
    if (std::holds_alternative<IpcMessageOutboxAck>(m)) {
      saw_ack = true;
      assert(std::get<IpcMessageOutboxAck>(m).value.through_epoch == 4);
    }
  }
  assert(saw_request && saw_ack);
}

int main() {
  // Static initializers above ran every TEST; report the tally.
  std::printf("test_reliable_sync: %d/%d passed\n", test_passed, test_count);
  return test_passed == test_count ? 0 : 1;
}
