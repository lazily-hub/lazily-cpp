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
// Every named fixture below is replayed from canonical bytes. The later wire and
// full-duplex tests remain binding-specific supplements.

#include <lazily/lazily.hpp>

#include "test_assertion_keys.hpp"
#include "test_json.hpp"
#include "test_spec_fixture.hpp"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace lazily;

static int test_count = 0;
static int test_passed = 0;

#define TEST(name)                                                                                 \
  static void name();                                                                              \
  struct name##_runner {                                                                           \
    name##_runner() {                                                                              \
      ++test_count;                                                                                \
      name();                                                                                      \
      ++test_passed;                                                                               \
    }                                                                                              \
  } name##_instance;                                                                               \
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
    for (const auto& n : s.nodes)
      nodes[n.node] = state_bytes(n.state);
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

static std::string fixture_json(const lazily_test::Json& value) {
  using Json = lazily_test::Json;
  switch (value.type) {
  case Json::Type::Null:
    return "null";
  case Json::Type::Bool:
    return value.boolean ? "true" : "false";
  case Json::Type::Number:
    return value.number_token;
  case Json::Type::String: {
    std::string out = "\"";
    for (const char ch : value.str) {
      switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += ch;
      }
    }
    return out + "\"";
  }
  case Json::Type::Array: {
    std::string out = "[";
    for (std::size_t i = 0; i < value.array.size(); ++i) {
      if (i != 0) out += ",";
      out += fixture_json(*value.array[i]);
    }
    return out + "]";
  }
  case Json::Type::Object: {
    std::string out = "{";
    for (std::size_t i = 0; i < value.object.size(); ++i) {
      if (i != 0) out += ",";
      lazily_test::Json key;
      key.type = Json::Type::String;
      key.str = value.object[i].first;
      out += fixture_json(key) + ":" + fixture_json(*value.object[i].second);
    }
    return out + "}";
  }
  }
  REQUIRE(false, "unknown fixture JSON node");
  return {};
}

static Delta fixture_delta(const lazily_test::Json& body) {
  const IpcMessage message = decode_json("{\"Delta\":" + fixture_json(body) + "}");
  REQUIRE(std::holds_alternative<IpcMessageDelta>(message),
          "fixture delta did not decode as Delta");
  return std::get<IpcMessageDelta>(message).value;
}

static IpcMessage fixture_message(const lazily_test::Json& body) {
  return decode_json(fixture_json(body));
}

static std::vector<Epoch> json_epochs(const lazily_test::Json& value) {
  std::vector<Epoch> out;
  for (const auto& item : lazily_test::json_array(value))
    out.push_back(lazily_test::json_u64(*item));
  return out;
}

static std::vector<std::string> json_strings(const lazily_test::Json& value) {
  std::vector<std::string> out;
  for (const auto& item : lazily_test::json_array(value))
    out.push_back(lazily_test::json_string(*item));
  return out;
}

static std::vector<uint8_t> json_bytes(const lazily_test::Json& value) {
  std::vector<uint8_t> out;
  for (const auto& item : lazily_test::json_array(value))
    out.push_back(static_cast<uint8_t>(lazily_test::json_u64(*item)));
  return out;
}

static std::string sync_action(const ResyncAction& action) {
  if (action.is_apply()) return "Apply";
  if (action.is_request_snapshot()) return "RequestSnapshot";
  return "Ignore";
}

static void require_fixture_keys(const lazily_test::Json& object,
                                 std::initializer_list<const char*> expected,
                                 const std::string& where) {
  REQUIRE(object.is_object(), where + ": expected object");
  std::set<std::string> actual;
  for (const auto& entry : object.object)
    actual.insert(entry.first);
  std::set<std::string> want;
  for (const char* key : expected)
    want.insert(key);
  REQUIRE(actual == want, where + ": fixture object keys changed");
}

static WireStamp fixture_stamp(const lazily_test::Json& value) {
  require_fixture_keys(value, {"wall_time", "logical", "peer"}, "liveness stamp");
  return WireStamp{
      static_cast<int64_t>(lazily_test::json_u64(lazily_test::json_member(value, "wall_time"))),
      static_cast<int64_t>(lazily_test::json_u64(lazily_test::json_member(value, "logical"))),
      static_cast<PeerId>(lazily_test::json_u64(lazily_test::json_member(value, "peer")))};
}

// ── resync_gap_converge.json ─────────────────────────────────────────────────

TEST(test_conformance_resync_fixture_replay) {
  constexpr const char* fixture_id = "reliable-sync/resync_gap_converge.json";
  const auto root = lazily_test::parse_json(
      lazily_test::spec_fixture_text("reliable-sync", "resync_gap_converge.json"));
  require_fixture_keys(
      *root, {"description", "protocol_version", "kind", "model", "scenarios", "wire"}, fixture_id);
  REQUIRE(lazily_test::json_u64(lazily_test::json_member(*root, "protocol_version")) == 1,
          "resync protocol_version");
  REQUIRE(lazily_test::json_string(lazily_test::json_member(*root, "kind")) == "ReliableSync",
          "resync kind");
  REQUIRE(lazily_test::json_string(lazily_test::json_member(*root, "model")) == "ResyncCoordinator",
          "resync model");

  for (const auto& view : lazily_test::scenario_views(
           fixture_id, lazily_test::json_array(lazily_test::json_member(*root, "scenarios")))) {
    const auto& scenario = view.replay();
    const std::string where = std::string(fixture_id) + " " + view.id();
    require_fixture_keys(
        scenario, {"id", "name", "description", "start_last_epoch", "inbound", "expect"}, where);
    ResyncCoordinator coordinator(
        lazily_test::json_u64(lazily_test::json_member(scenario, "start_last_epoch")));
    GraphModel graph;
    GraphModel covering_snapshot;
    bool saw_snapshot = false;
    int resync_requests = 0;

    for (const auto& raw_inbound :
         lazily_test::json_array(lazily_test::json_member(scenario, "inbound"))) {
      const auto& inbound = *raw_inbound;
      if (inbound.has("dropped")) {
        require_fixture_keys(inbound, {"dropped", "note"}, where + " dropped inbound");
        REQUIRE(lazily_test::json_bool(lazily_test::json_member(inbound, "dropped")),
                where + ": dropped selector must be true");
        continue;
      }
      if (inbound.has("request_from")) {
        require_fixture_keys(inbound,
                             {"frame", "expect_action", "request_from", "last_epoch_after"},
                             where + " request inbound");
      } else if (inbound.has("reason")) {
        require_fixture_keys(inbound, {"frame", "expect_action", "reason", "last_epoch_after"},
                             where + " ignored inbound");
      } else {
        require_fixture_keys(inbound, {"frame", "expect_action", "last_epoch_after"},
                             where + " inbound");
      }

      const IpcMessage message = fixture_message(lazily_test::json_member(inbound, "frame"));
      ResyncAction action;
      if (std::holds_alternative<IpcMessageDelta>(message)) {
        const auto& delta = std::get<IpcMessageDelta>(message).value;
        action = coordinator.ingest_delta(delta);
        if (action.is_apply()) graph.apply_delta(delta);
      } else {
        REQUIRE(std::holds_alternative<IpcMessageSnapshot>(message),
                where + ": inbound frame must be Delta or Snapshot");
        const auto& snapshot = std::get<IpcMessageSnapshot>(message).value;
        action = coordinator.ingest_snapshot(snapshot.epoch);
        if (action.is_apply()) graph.apply_snapshot(snapshot);
        covering_snapshot.apply_snapshot(snapshot);
        saw_snapshot = true;
      }
      if (action.is_request_snapshot()) ++resync_requests;
      REQUIRE(sync_action(action) ==
                  lazily_test::json_string(lazily_test::json_member(inbound, "expect_action")),
              where + ": action mismatch");
      REQUIRE(coordinator.last_epoch() ==
                  lazily_test::json_u64(lazily_test::json_member(inbound, "last_epoch_after")),
              where + ": cursor mismatch");
      if (inbound.has("request_from"))
        REQUIRE(action.from_epoch ==
                    lazily_test::json_u64(lazily_test::json_member(inbound, "request_from")),
                where + ": request cursor mismatch");
      if (inbound.has("reason"))
        REQUIRE(lazily_test::json_string(lazily_test::json_member(inbound, "reason")) ==
                        "resyncing_suppress_duplicate_request" &&
                    action.is_ignore(),
                where + ": ignore reason mismatch");
    }

    lazily_test::AssertionKeys expected(where + " expect",
                                        lazily_test::json_member(scenario, "expect"));
    expected.assert_key("final_last_epoch", coordinator.last_epoch());
    expected.assert_key("resync_requests_emitted", resync_requests);
    expected.with_sub_if_present("converged_nodes", [&](lazily_test::AssertionKeys& nodes) {
      for (const auto& node : graph.nodes)
        nodes.assert_key(std::to_string(node.first), node.second, json_bytes);
    });
    expected.assert_key_if_present("equals_no_drop_receiver",
                                   saw_snapshot && graph.nodes == covering_snapshot.nodes);
  }

  const IpcMessage wire = fixture_message(lazily_test::json_member(*root, "wire"));
  REQUIRE(std::holds_alternative<IpcMessageResyncRequest>(wire),
          "resync wire must be ResyncRequest");
}

// ── idempotent_redelivery.json ───────────────────────────────────────────────

TEST(test_conformance_idempotent_fixture_replay) {
  constexpr const char* fixture_id = "reliable-sync/idempotent_redelivery.json";
  const auto root = lazily_test::parse_json(
      lazily_test::spec_fixture_text("reliable-sync", "idempotent_redelivery.json"));
  require_fixture_keys(
      *root, {"description", "protocol_version", "kind", "model", "scenarios", "wire"}, fixture_id);
  REQUIRE(lazily_test::json_u64(lazily_test::json_member(*root, "protocol_version")) == 1,
          "idempotent protocol_version");

  for (const auto& view : lazily_test::scenario_views(
           fixture_id, lazily_test::json_array(lazily_test::json_member(*root, "scenarios")))) {
    const auto& scenario = view.replay();
    const std::string where = std::string(fixture_id) + " " + view.id();
    require_fixture_keys(
        scenario,
        {"id", "name", "description", "start_last_epoch", "state_before", "inbound", "expect"},
        where);
    ResyncCoordinator coordinator(
        lazily_test::json_u64(lazily_test::json_member(scenario, "start_last_epoch")));
    GraphModel graph;
    for (const auto& entry : lazily_test::json_member(scenario, "state_before").object)
      graph.nodes[std::stoull(entry.first)] = json_bytes(*entry.second);
    const auto before = graph.nodes;

    for (const auto& raw_inbound :
         lazily_test::json_array(lazily_test::json_member(scenario, "inbound"))) {
      const auto& inbound = *raw_inbound;
      require_fixture_keys(inbound, {"frame", "expect_action", "reason", "last_epoch_after"},
                           where + " inbound");
      const IpcMessage message = fixture_message(lazily_test::json_member(inbound, "frame"));
      REQUIRE(std::holds_alternative<IpcMessageDelta>(message),
              where + ": idempotent inbound must be Delta");
      const auto& delta = std::get<IpcMessageDelta>(message).value;
      const ResyncAction action = coordinator.ingest_delta(delta);
      if (action.is_apply()) graph.apply_delta(delta);
      REQUIRE(sync_action(action) ==
                  lazily_test::json_string(lazily_test::json_member(inbound, "expect_action")),
              where + ": action mismatch");
      REQUIRE(action.is_ignore() &&
                  lazily_test::json_string(lazily_test::json_member(inbound, "reason")) ==
                      "base_epoch_below_last_epoch_already_applied",
              where + ": ignore reason mismatch");
      REQUIRE(coordinator.last_epoch() ==
                  lazily_test::json_u64(lazily_test::json_member(inbound, "last_epoch_after")),
              where + ": cursor mismatch");
    }

    lazily_test::AssertionKeys expected(where + " expect",
                                        lazily_test::json_member(scenario, "expect"));
    expected.assert_key("final_last_epoch", coordinator.last_epoch());
    expected.with_sub("state_after", [&](lazily_test::AssertionKeys& state) {
      for (const auto& node : graph.nodes)
        state.assert_key(std::to_string(node.first), node.second, json_bytes);
    });
    expected.assert_key("net_effect_unchanged", graph.nodes == before);
  }

  const IpcMessage wire = fixture_message(lazily_test::json_member(*root, "wire"));
  REQUIRE(std::holds_alternative<IpcMessageOutboxAck>(wire), "idempotent wire must be OutboxAck");
}

// ── multi_epoch_delta.json ───────────────────────────────────────────────────

TEST(test_conformance_multi_epoch_fixture_replay) {
  constexpr const char* fixture_id = "reliable-sync/multi_epoch_delta.json";
  const auto root = lazily_test::parse_json(
      lazily_test::spec_fixture_text("reliable-sync", "multi_epoch_delta.json"));
  require_fixture_keys(
      *root,
      {"description", "protocol_version", "kind", "model", "assertions", "scenarios", "wire"},
      fixture_id);
  REQUIRE(lazily_test::json_u64(lazily_test::json_member(*root, "protocol_version")) == 1,
          "multi-epoch protocol_version");
  REQUIRE(lazily_test::json_string(lazily_test::json_member(*root, "kind")) == "ReliableSync",
          "multi-epoch kind");
  REQUIRE(lazily_test::json_string(lazily_test::json_member(*root, "model")) == "MultiEpochDelta",
          "multi-epoch model");

  const IpcMessage wire = decode_json(fixture_json(lazily_test::json_member(*root, "wire")));
  REQUIRE(std::holds_alternative<IpcMessageDelta>(wire), "multi-epoch wire must be Delta");
  const Delta& root_delta = std::get<IpcMessageDelta>(wire).value;
  lazily_test::AssertionKeys assertions(std::string(fixture_id) + " assertions",
                                        lazily_test::json_member(*root, "assertions"));
  assertions.assert_key("base_epoch", root_delta.base_epoch);
  assertions.assert_key("epoch", root_delta.epoch);
  assertions.assert_key("span", root_delta.epoch - root_delta.base_epoch);
  assertions.assert_key("is_multi_epoch", root_delta.epoch > root_delta.base_epoch + 1);
  assertions.assert_key("op_count", root_delta.ops.size());

  const auto& raw_scenarios = lazily_test::json_array(lazily_test::json_member(*root, "scenarios"));
  for (const auto& view : lazily_test::scenario_views(fixture_id, raw_scenarios)) {
    const auto& scenario = view.replay();
    const auto& delta_json = lazily_test::json_member(scenario, "delta");
    const Delta delta = fixture_delta(delta_json);
    const Epoch start =
        lazily_test::json_u64(lazily_test::json_member(scenario, "receiver_last_epoch"));
    ResyncCoordinator coordinator(start);
    GraphModel batch;
    const ResyncAction action = coordinator.ingest_delta(delta);
    if (action.is_apply()) batch.apply_delta(delta);

    lazily_test::AssertionKeys expected(std::string(fixture_id) + " " + view.id() + " expect",
                                        lazily_test::json_member(scenario, "expect"));
    expected.assert_key("action", sync_action(action));
    expected.assert_key("applied", action.is_apply());
    expected.assert_key("receiver_last_epoch_after", coordinator.last_epoch());

    if (view.id() == "span_3_applies_equal_to_unit_fold") {
      require_fixture_keys(scenario,
                           {"id", "name", "description", "receiver_last_epoch", "delta",
                            "equivalent_unit_fold", "expect"},
                           std::string(fixture_id) + " " + view.id());
      ResyncCoordinator unit_coordinator(start);
      GraphModel unit;
      std::vector<Epoch> cursors;
      for (const auto& unit_json :
           lazily_test::json_array(lazily_test::json_member(scenario, "equivalent_unit_fold"))) {
        const Delta one = fixture_delta(*unit_json);
        const auto unit_action = unit_coordinator.ingest_delta(one);
        REQUIRE(unit_action.is_apply(), "equivalent unit fold must apply");
        unit.apply_delta(one);
        cursors.push_back(unit_coordinator.last_epoch());
      }
      const bool fold_equivalent =
          unit.nodes == batch.nodes && unit_coordinator.last_epoch() == coordinator.last_epoch();
      expected.assert_key("fold_equivalent", fold_equivalent);
      const bool atomic_advance =
          action.is_apply() && coordinator.last_epoch() == delta.epoch && cursors.size() > 1;
      expected.assert_key("atomic_advance", atomic_advance);
    } else if (view.id() == "gap_rule_unchanged_under_span") {
      require_fixture_keys(scenario,
                           {"id", "name", "description", "receiver_last_epoch", "delta", "expect"},
                           std::string(fixture_id) + " " + view.id());
      expected.assert_key("request_from", action.from_epoch);
    } else {
      REQUIRE(false, std::string(fixture_id) + ": unknown scenario " + view.id());
    }
  }
}

// ── outbox_replay_after_crash.json ───────────────────────────────────────────

TEST(test_conformance_outbox_replay_fixture) {
  constexpr const char* fixture_id = "reliable-sync/outbox_replay_after_crash.json";
  const auto root = lazily_test::parse_json(
      lazily_test::spec_fixture_text("reliable-sync", "outbox_replay_after_crash.json"));
  require_fixture_keys(
      *root, {"description", "protocol_version", "kind", "model", "scenarios", "wire"}, fixture_id);
  REQUIRE(lazily_test::json_u64(lazily_test::json_member(*root, "protocol_version")) == 1,
          "outbox replay protocol_version");

  for (const auto& view : lazily_test::scenario_views(
           fixture_id, lazily_test::json_array(lazily_test::json_member(*root, "scenarios")))) {
    const auto& scenario = view.replay();
    const std::string where = std::string(fixture_id) + " " + view.id();
    std::vector<std::pair<Epoch, IpcMessage>> appended;
    for (const auto& raw_entry :
         lazily_test::json_array(lazily_test::json_member(scenario, "appended"))) {
      const auto& entry = *raw_entry;
      require_fixture_keys(entry, {"epoch", "frame"}, where + " appended");
      appended.emplace_back(lazily_test::json_u64(lazily_test::json_member(entry, "epoch")),
                            fixture_message(lazily_test::json_member(entry, "frame")));
    }

    if (scenario.has("crash")) {
      require_fixture_keys(scenario,
                           {"id", "name", "description", "appended", "ack_through", "crash",
                            "reconnect_cursor", "expect"},
                           where);
      REQUIRE(lazily_test::json_bool(lazily_test::json_member(scenario, "crash")),
              where + ": crash selector must be true");
      InMemoryOutbox outbox;
      for (const auto& entry : appended)
        outbox.append(entry.first, entry.second);
      outbox.ack_through(lazily_test::json_u64(lazily_test::json_member(scenario, "ack_through")));
      const auto retained = outbox.retained_epochs();
      auto store = std::move(outbox).into_store();
      InMemoryOutbox reopened(std::move(store));
      const Epoch cursor =
          lazily_test::json_u64(lazily_test::json_member(scenario, "reconnect_cursor"));
      const auto replay = reopened.replay_from(cursor);
      std::vector<Epoch> replayed;
      std::vector<Epoch> applied;
      ResyncCoordinator receiver(cursor);
      GraphModel graph;
      for (const auto& entry : replay) {
        replayed.push_back(entry.first);
        const ResyncAction action = receiver.ingest(entry.second);
        if (!action.is_apply()) continue;
        REQUIRE(std::holds_alternative<IpcMessageDelta>(entry.second),
                where + ": replayed frame must be Delta");
        graph.apply_delta(std::get<IpcMessageDelta>(entry.second).value);
        applied.push_back(entry.first);
      }
      const std::set<Epoch> unique_applied(applied.begin(), applied.end());
      const std::size_t ops_lost = replayed.size() - applied.size();
      const std::size_t ops_doubled = applied.size() - unique_applied.size();

      lazily_test::AssertionKeys expected(where + " expect",
                                          lazily_test::json_member(scenario, "expect"));
      expected.assert_key("retained_after_ack", retained, json_epochs);
      expected.assert_key("replayed_from_cursor", replayed, json_epochs);
      expected.assert_key("replay_order", replayed, json_epochs);
      expected.assert_key("receiver_applies", applied, json_epochs);
      expected.assert_key("receiver_last_epoch_after", receiver.last_epoch());
      expected.assert_key("ops_lost", ops_lost);
      expected.assert_key("ops_doubled", ops_doubled);
      expected.assert_key("exactly_once_effect", ops_lost == 0 && ops_doubled == 0);
    } else {
      require_fixture_keys(scenario,
                           {"id", "name", "description", "appended", "send_fails_first_attempt",
                            "ack_through", "expect"},
                           where);
      REQUIRE(
          lazily_test::json_bool(lazily_test::json_member(scenario, "send_fails_first_attempt")),
          where + ": send failure selector must be true");
      REQUIRE(lazily_test::json_member(scenario, "ack_through").type ==
                  lazily_test::Json::Type::Null,
              where + ": failed send must remain unacked");

      auto outbox = std::make_shared<InMemoryOutbox>();
      bool fail_next = true;
      std::vector<Epoch> resent;
      IpcSink sink = [&](const IpcMessage& message) {
        if (fail_next) {
          REQUIRE(std::holds_alternative<IpcMessageDelta>(message),
                  where + ": first failed frame must be Delta");
          fail_next = false;
          return false;
        }
        if (std::holds_alternative<IpcMessageDelta>(message))
          resent.push_back(std::get<IpcMessageDelta>(message).value.epoch);
        return true;
      };
      IpcSource source = [&]() -> std::optional<IpcMessage> { return std::nullopt; };
      Clock clock = [] { return int64_t(0); };
      SnapshotProvider provider = [](Epoch) {
        return IpcMessage{IpcMessageSnapshot{Snapshot{0, {}, {}, {}}}};
      };
      SyncDriver driver(sink, source, outbox, clock, provider);
      for (const auto& entry : appended)
        driver.enqueue(entry.first, entry.second);
      const Progress failed = driver.tick();
      const bool stalled_after_failure = driver.is_stalled();
      const auto retained_after_failure = outbox->retained_epochs();
      driver.on_reconnect();
      const Progress retried = driver.tick();

      lazily_test::AssertionKeys expected(where + " expect",
                                          lazily_test::json_member(scenario, "expect"));
      expected.assert_key("frame_retained_after_failed_send", failed.sent == 0 &&
                                                                  stalled_after_failure &&
                                                                  !retained_after_failure.empty());
      expected.assert_key("retained", retained_after_failure, json_epochs);
      expected.assert_key("resent_on_next_tick", resent, json_epochs);
      expected.assert_key("permanent_gap",
                          retried.sent != appended.size() || resent.size() != appended.size());
    }
  }

  const IpcMessage wire = fixture_message(lazily_test::json_member(*root, "wire"));
  REQUIRE(std::holds_alternative<IpcMessageOutboxAck>(wire),
          "outbox replay wire must be OutboxAck");
}

// ── liveness_orset_lww.json ──────────────────────────────────────────────────

static WireStamp ws(int64_t wall, int64_t logical, PeerId peer) {
  return WireStamp{wall, logical, peer};
}

static std::set<std::string>
live_docs(const std::map<std::string, std::pair<std::string, OrSet>>&
              open_set,                                              // key -> (doc, pid) OR-set
          const std::map<std::string, WireLwwRegister<bool>>& alive, // pid -> alive
          const std::map<std::string, std::string>& key_pid) {       // key -> pid
  std::set<std::string> docs;
  for (const auto& kv : open_set) {
    if (!kv.second.second.present()) continue;
    const std::string& pid = key_pid.at(kv.first);
    auto it = alive.find(pid);
    if (it != alive.end() && it->second.value()) docs.insert(kv.second.first);
  }
  return docs;
}

static std::pair<std::string, std::string> doc_pid(const std::string& key) {
  const auto slash = key.find('/');
  REQUIRE(slash != std::string::npos, "liveness key must be doc/pid");
  std::string pid = key.substr(slash + 1);
  if (pid.rfind("pid", 0) == 0) pid = pid.substr(3);
  return {key.substr(0, slash), pid};
}

static void apply_orset_op(const lazily_test::Json& op, OrSet& set, const std::string& where) {
  const std::string kind = lazily_test::json_string(lazily_test::json_member(op, "op"));
  if (kind == "add") {
    require_fixture_keys(op, {"op", "tag", "stamp"}, where);
    (void)fixture_stamp(lazily_test::json_member(op, "stamp"));
    set.add(lazily_test::json_string(lazily_test::json_member(op, "tag")));
  } else {
    REQUIRE(kind == "remove", where + ": unknown OR-set op");
    require_fixture_keys(op, {"op", "observed_tags", "stamp"}, where);
    (void)fixture_stamp(lazily_test::json_member(op, "stamp"));
    set.remove_observed(json_strings(lazily_test::json_member(op, "observed_tags")));
  }
}

struct LivenessState {
  std::map<std::string, std::pair<std::string, OrSet>> open_set;
  std::map<std::string, std::string> key_pid;
  std::map<std::string, WireLwwRegister<bool>> alive;
};

static void apply_liveness_op(const lazily_test::Json& op, LivenessState& state,
                              const std::string& where) {
  const std::string register_kind =
      lazily_test::json_string(lazily_test::json_member(op, "register_kind"));
  const std::string key = lazily_test::json_string(lazily_test::json_member(op, "key"));
  if (register_kind == "orset") {
    require_fixture_keys(op, {"register_kind", "key", "op", "tag", "stamp"}, where);
    const auto parts = doc_pid(key);
    state.open_set[key].first = parts.first;
    state.key_pid[key] = parts.second;
    (void)fixture_stamp(lazily_test::json_member(op, "stamp"));
    state.open_set[key].second.add(lazily_test::json_string(lazily_test::json_member(op, "tag")));
  } else {
    REQUIRE(register_kind == "lww", where + ": unknown liveness register kind");
    require_fixture_keys(op, {"register_kind", "key", "value", "stamp"}, where);
    const auto slash = key.rfind('/');
    REQUIRE(slash != std::string::npos, where + ": alive key must have a pid suffix");
    std::string pid = key.substr(slash + 1);
    if (pid.rfind("pid", 0) == 0) pid = pid.substr(3);
    const bool value = lazily_test::json_bool(lazily_test::json_member(op, "value"));
    const WireStamp stamp = fixture_stamp(lazily_test::json_member(op, "stamp"));
    const auto found = state.alive.find(pid);
    if (found == state.alive.end())
      state.alive.emplace(pid, WireLwwRegister<bool>(stamp, value));
    else
      found->second.set(stamp, value);
  }
}

TEST(test_conformance_liveness_fixture_replay) {
  constexpr const char* fixture_id = "reliable-sync/liveness_orset_lww.json";
  const auto root = lazily_test::parse_json(
      lazily_test::spec_fixture_text("reliable-sync", "liveness_orset_lww.json"));
  require_fixture_keys(*root, {"description", "protocol_version", "kind", "model", "scenarios"},
                       fixture_id);
  REQUIRE(lazily_test::json_u64(lazily_test::json_member(*root, "protocol_version")) == 1,
          "liveness protocol_version");

  for (const auto& view : lazily_test::scenario_views(
           fixture_id, lazily_test::json_array(lazily_test::json_member(*root, "scenarios")))) {
    const auto& scenario = view.replay();
    const std::string where = std::string(fixture_id) + " " + view.id();
    if (view.id() == "open_set_add_wins_over_stale_remove") {
      require_fixture_keys(
          scenario, {"id", "name", "description", "register_kind", "key", "ops", "expect"}, where);
      REQUIRE(lazily_test::json_string(lazily_test::json_member(scenario, "register_kind")) ==
                  "orset",
              where + ": register kind");
      REQUIRE(!lazily_test::json_string(lazily_test::json_member(scenario, "key")).empty(),
              where + ": liveness key");
      const auto& ops = lazily_test::json_array(lazily_test::json_member(scenario, "ops"));
      OrSet forward;
      for (const auto& op : ops)
        apply_orset_op(*op, forward, where + " op");
      OrSet reverse;
      for (auto it = ops.rbegin(); it != ops.rend(); ++it)
        apply_orset_op(**it, reverse, where + " reverse op");
      const bool before_redelivery = forward.present();
      for (const auto& op : ops)
        apply_orset_op(*op, forward, where + " redelivered op");
      const int redeliver_applied = forward.present() == before_redelivery ? 0 : 1;

      lazily_test::AssertionKeys expected(where + " expect",
                                          lazily_test::json_member(scenario, "expect"));
      expected.assert_key("present", forward.present());
      expected.assert_key("reason", forward.present()
                                        ? std::string("add_tag_t3_not_observed_by_remove")
                                        : std::string("removed"));
      expected.assert_key("order_independent", forward.present() == reverse.present());
      expected.assert_key("redeliver_applied_count", redeliver_applied);
    } else if (view.id() == "lww_alive_highest_stamp_wins") {
      require_fixture_keys(
          scenario, {"id", "name", "description", "register_kind", "key", "ops", "expect"}, where);
      REQUIRE(lazily_test::json_string(lazily_test::json_member(scenario, "register_kind")) ==
                  "lww",
              where + ": register kind");
      const auto& ops = lazily_test::json_array(lazily_test::json_member(scenario, "ops"));
      REQUIRE(!ops.empty(), where + ": LWW fixture needs at least one op");
      auto make_register = [&](bool reverse_order) {
        std::vector<const lazily_test::Json*> ordered;
        for (const auto& op : ops) {
          require_fixture_keys(*op, {"value", "stamp"}, where + " lww op");
          ordered.push_back(op.get());
        }
        if (reverse_order) std::reverse(ordered.begin(), ordered.end());
        WireLwwRegister<bool> result(
            fixture_stamp(lazily_test::json_member(*ordered.front(), "stamp")),
            lazily_test::json_bool(lazily_test::json_member(*ordered.front(), "value")));
        for (std::size_t i = 1; i < ordered.size(); ++i)
          result.set(fixture_stamp(lazily_test::json_member(*ordered[i], "stamp")),
                     lazily_test::json_bool(lazily_test::json_member(*ordered[i], "value")));
        return result;
      };
      const auto forward = make_register(false);
      const auto reverse = make_register(true);

      lazily_test::AssertionKeys expected(where + " expect",
                                          lazily_test::json_member(scenario, "expect"));
      expected.assert_key("value", forward.value());
      expected.assert_key("resolution", std::string("max_stamp"));
      expected.assert_key("order_independent", forward.value() == reverse.value());
    } else if (view.id() == "whole_editor_death_cascades") {
      require_fixture_keys(
          scenario, {"id", "name", "description", "open_set", "alive_before", "op", "expect"},
          where);
      LivenessState state;
      for (const auto& raw_open :
           lazily_test::json_array(lazily_test::json_member(scenario, "open_set"))) {
        const auto& open = *raw_open;
        require_fixture_keys(open, {"key", "present"}, where + " open_set");
        const std::string key = lazily_test::json_string(lazily_test::json_member(open, "key"));
        const auto parts = doc_pid(key);
        state.open_set[key].first = parts.first;
        state.key_pid[key] = parts.second;
        if (lazily_test::json_bool(lazily_test::json_member(open, "present")))
          state.open_set[key].second.add(key);
      }
      for (const auto& alive : lazily_test::json_member(scenario, "alive_before").object)
        state.alive.emplace(
            alive.first, WireLwwRegister<bool>(ws(0, 0, 0), lazily_test::json_bool(*alive.second)));
      const auto before = live_docs(state.open_set, state.alive, state.key_pid);
      apply_liveness_op(lazily_test::json_member(scenario, "op"), state, where + " death op");
      const auto after = live_docs(state.open_set, state.alive, state.key_pid);

      lazily_test::AssertionKeys expected(where + " expect",
                                          lazily_test::json_member(scenario, "expect"));
      expected.assert_key("live_docs_before",
                          std::vector<std::string>(before.begin(), before.end()), json_strings);
      expected.assert_key("live_docs_after", std::vector<std::string>(after.begin(), after.end()),
                          json_strings);
      expected.assert_key("cascade", after.size() < before.size());
    } else if (view.id() == "derived_live_doc_aggregate_converges_under_retry") {
      require_fixture_keys(scenario,
                           {"id", "name", "description", "replicas", "ops",
                            "reverse_order_equivalent", "redeliver", "expect"},
                           where);
      REQUIRE(json_strings(lazily_test::json_member(scenario, "replicas")).size() == 2,
              where + ": fixture must name two replicas");
      REQUIRE(
          lazily_test::json_bool(lazily_test::json_member(scenario, "reverse_order_equivalent")),
          where + ": reverse-order selector");
      REQUIRE(lazily_test::json_bool(lazily_test::json_member(scenario, "redeliver")),
              where + ": redelivery selector");
      const auto& ops = lazily_test::json_array(lazily_test::json_member(scenario, "ops"));
      auto build = [&](bool reverse_order, bool redeliver) {
        LivenessState state;
        std::vector<const lazily_test::Json*> ordered;
        for (const auto& op : ops)
          ordered.push_back(op.get());
        if (reverse_order) std::reverse(ordered.begin(), ordered.end());
        for (const auto* op : ordered)
          apply_liveness_op(*op, state, where + " op");
        const auto before_redelivery = live_docs(state.open_set, state.alive, state.key_pid);
        if (redeliver)
          for (const auto* op : ordered)
            apply_liveness_op(*op, state, where + " redelivered op");
        return std::make_pair(live_docs(state.open_set, state.alive, state.key_pid),
                              before_redelivery);
      };
      const auto forward = build(false, true);
      const auto reverse = build(true, false);
      const int redeliver_applied = forward.first == forward.second ? 0 : 1;

      lazily_test::AssertionKeys expected(where + " expect",
                                          lazily_test::json_member(scenario, "expect"));
      expected.assert_key("converged_live_docs",
                          std::vector<std::string>(forward.first.begin(), forward.first.end()),
                          json_strings);
      expected.assert_key("order_independent", forward.first == reverse.first);
      expected.assert_key("redeliver_applied_count", redeliver_applied);
      expected.assert_key("per_doc_isolation", forward.first.size() == 2);
    } else {
      REQUIRE(false, std::string(fixture_id) + ": unknown scenario " + view.id());
    }
  }
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

  IpcSink sink = [&](const IpcMessage& m) {
    wire.push_back(m);
    return true;
  };
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
  inbound.push_back(IpcMessageSnapshot{
      Snapshot{4, {node_snap(1, {10}), node_snap(2, {20}), node_snap(3, {30})}, {}, {1, 2, 3}}});

  Progress p = driver.tick();
  for (const auto& m : p.applied) {
    if (std::holds_alternative<IpcMessageDelta>(m))
      g.apply_delta(std::get<IpcMessageDelta>(m).value);
    else if (std::holds_alternative<IpcMessageSnapshot>(m))
      g.apply_snapshot(std::get<IpcMessageSnapshot>(m).value);
  }

  assert(p.resync_requested);       // gap detected on 3->4
  assert(driver.last_epoch() == 4); // snapshot adopted
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
  REQUIRE_FIXTURES_LOADED(5);
  return test_passed == test_count ? 0 : 1;
}
