// CRDT-plane anti-entropy conformance, replayed from the canonical lazily-spec
// bytes.
//
// `conformance/distributed/anti_entropy_converge.json` had NO runner in
// lazily-cpp. `tests/test_family_sync.cpp` drives `CrdtPlaneRuntime` with
// hand-built ops, so nothing here replayed the spec's convergence corpus: the
// stamp ordering, the applied-op accounting, state-based idempotence, and
// delivery-order independence were all unverified against the canonical data.
//
// This runner ingests each scenario's ACTUAL ops through `CrdtPlaneRuntime` and
// asserts `expect.applied_count`, `expect.converged`, `expect.redeliver_applied_
// count`, and — when the scenario declares `reverse_order_equivalent` — that a
// fresh runtime fed the reversed op sequence converges identically. It mirrors
// lazily-kt's CrdtPlaneTest so the two bindings assert the same properties.
//
// ## No silent defaults
//
// The `state` variant tag, the `resolution` rule, and every key in a scenario's
// `expect` block are mapped through checks that hard-fail on an unrecognised
// value. An unknown value resolving to a default replays a different frame and
// still reports green.
//
// ## Positive assertion
//
// `REQUIRE_FIXTURES_LOADED(1)` proves the canonical file was opened;
// `g_ops_ingested` proves ops were actually replayed.

#include <lazily/lazily.hpp>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "test_assertion_keys.hpp"
#include "test_json.hpp"
#include "test_require.hpp"
#include "test_spec_fixture.hpp"

using namespace lazily;
using lazily_test::Json;
using lazily_test::JsonPtr;
using lazily_test::parse_json;

static const char* kArea = "distributed";
static const char* kFixture = "anti_entropy_converge.json";
static const char* kFramesFixture = "crdt_sync_frames.json";

static size_t g_scenarios = 0;
static size_t g_ops_ingested = 0;
static size_t g_checks = 0;

// `state` is an externally-tagged IpcValue. Only the variants this binding
// models are accepted; an unknown tag aborts rather than silently becoming an
// empty inline payload, which would make every converged comparison vacuous.
static IpcValue ipc_value_of(const Json* node) {
  REQUIRE(node != nullptr && node->is_object(),
          "an op `state` must be an externally-tagged object");
  REQUIRE(node->object.size() == 1, "an IpcValue carries exactly one variant tag");
  const std::string& tag = node->object.front().first;
  const Json& body = *node->object.front().second;
  if (tag == "Inline") {
    REQUIRE(body.is_array(), "Inline carries a byte array");
    std::vector<uint8_t> bytes;
    for (const auto& b : body.array) {
      REQUIRE(b->type == Json::Type::Number, "an inline byte must be a number");
      const long long value = b->as_int();
      REQUIRE(value >= 0 && value <= 255, "an inline byte is out of range");
      bytes.push_back(static_cast<uint8_t>(value));
    }
    return IpcValueInline{std::move(bytes)};
  }
  if (tag == "SharedBlob") {
    ShmBlobRef blob{};
    const Json* offset = body.find("offset");
    const Json* len = body.find("len");
    const Json* generation = body.find("generation");
    const Json* epoch = body.find("epoch");
    const Json* checksum = body.find("checksum");
    REQUIRE(offset && len && generation && epoch && checksum,
            "a SharedBlob ref needs offset/len/generation/epoch/checksum");
    blob.offset = static_cast<int64_t>(offset->as_int());
    blob.len = static_cast<int64_t>(len->as_int());
    blob.generation = static_cast<int64_t>(generation->as_int());
    blob.epoch = static_cast<int64_t>(epoch->as_int());
    blob.checksum = static_cast<int64_t>(checksum->as_int());
    return IpcValueSharedBlob{blob};
  }
  REQUIRE(false, "unknown IpcValue variant tag in fixture");
  return IpcValueInline{}; // unreachable
}

static WireStamp stamp_of(const Json* node) {
  REQUIRE(node != nullptr && node->is_object(), "an op needs a stamp object");
  const Json* wall = node->find("wall_time");
  const Json* logical = node->find("logical");
  // The spec is explicit that WireStamp spells the replica field `peer`, never
  // `peer_id`. Requiring it keeps a renamed field from defaulting to peer 0 and
  // silently changing the tie-break.
  const Json* peer = node->find("peer");
  REQUIRE(wall && logical && peer, "a WireStamp needs wall_time, logical and peer");
  WireStamp stamp{};
  stamp.wall_time = static_cast<uint64_t>(wall->as_int());
  stamp.logical = static_cast<uint32_t>(logical->as_int());
  stamp.peer = static_cast<PeerId>(peer->as_int());
  return stamp;
}

static CrdtOp op_of(const Json* node) {
  REQUIRE(node != nullptr && node->is_object(), "an op is not an object");
  const Json* node_id = node->find("node");
  REQUIRE(node_id != nullptr, "an op needs a node id");
  // `key` is always present in the corpus, null when unset — absence would be a
  // renamed field, not an unkeyed op.
  const Json* key = node->find("key");
  REQUIRE(key != nullptr, "an op must carry `key` (null when unset)");
  CrdtOp op;
  op.node = static_cast<NodeId>(node_id->as_int());
  if (!key->is_null()) {
    REQUIRE(key->type == Json::Type::String, "an op key must be a string or null");
    auto parsed = NodeKey::create(key->str);
    REQUIRE(parsed.has_value(), "an op key is not a valid NodeKey");
    op.key = std::move(*parsed);
  }
  op.stamp = stamp_of(node->find("stamp"));
  op.state = ipc_value_of(node->find("state"));
  return op;
}

// Keys a scenario's `expect` block may carry.
static bool is_known_expect_key(const std::string& key) {
  return key == "resolution" || key == "applied_count" || key == "redeliver_applied_count" ||
         key == "order_independent" || key == "converged";
}

static bool is_known_scenario_key(const std::string& key) {
  // `id` is the canonical scenario identity (#recommendedconformanceco) and is
  // what record_scenario_at books into the replay ledger; `name` is its prose
  // label.
  return key == "id" || key == "name" || key == "description" || key == "ops" || key == "expect" ||
         key == "redeliver" || key == "reverse_order_equivalent";
}

static void assert_converged(const std::string& scenario, const CrdtPlaneRuntime& runtime,
                             const Json* converged) {
  REQUIRE(converged != nullptr && converged->is_array() && !converged->array.empty(),
          "a scenario's expect block has no converged entries");
  for (const auto& entry : converged->array) {
    const Json* node_id = entry->find("node");
    const Json* state = entry->find("state");
    REQUIRE(node_id != nullptr && state != nullptr, "a converged entry needs a node and a state");
    const auto node = static_cast<NodeId>(node_id->as_int());
    const auto got = runtime.value(node);
    ++g_checks;
    REQUIRE(got.has_value(), "a converged node has no value in the runtime");
    if (!ipc_value_equal(*got, ipc_value_of(state))) {
      std::cout << "FAIL: " << scenario << ": converged state mismatch for node " << node
                << std::endl;
      std::abort();
    }
  }
}

// `crdt_sync_frames.json` is a WIRE fixture, not a plane-behaviour one: each
// frame carries a `CrdtSync` payload plus assertions about its shape, and it
// pins the CODEC boundary rather than convergence. It was excused in
// scripts/check-conformance-coverage.sh for one concrete reason — lazily-cpp had
// no JSON codec for the IpcMessage envelope, so there was nothing to replay
// `wire` through. `#lzcppjsoncodec` removed that reason; this removes the
// excuse.
//
// The frames are decoded through `json_to_ipc_message` and then RE-ENCODED, so
// this asserts the codec rather than the fixture against itself. Two gaps
// surfaced immediately and are fixed in include/lazily/json_codec.hpp: the
// decoder REQUIRED `frontier`, which made the suppressed frame undecodable, and
// the encoder always wrote it, which turned "unchanged since the last accepted
// frame" into "the sender advertises nothing" on the way out. Neither was
// visible in codec/frame_roundtrip_json.json, whose CrdtSync frame always
// carries a frontier.
static void replay_crdt_sync_frames() {
  const std::string text = lazily_test::spec_fixture_text(kArea, kFramesFixture);
  const JsonPtr fixture = parse_json(text);
  REQUIRE(fixture->is_object(), "crdt_sync_frames fixture root is not an object");
  REQUIRE(lazily_test::json_u64(lazily_test::json_member(*fixture, "protocol_version")) == 1,
          "crdt_sync_frames protocol_version");
  REQUIRE(lazily_test::json_string(lazily_test::json_member(*fixture, "kind")) == "CrdtSyncFrames",
          "crdt_sync_frames kind");

  const Json* frames = fixture->find("frames");
  REQUIRE(frames != nullptr && frames->is_array() && !frames->array.empty(),
          "a replay of zero frames is not a replay");

  // The library's own parser reads the same bytes; that view is what the codec
  // consumes, and being able to read the canonical corpus is part of what a
  // reference codec has to prove.
  const JsonValue library = json_parse(text);
  const JsonValue* library_frames = library.find("frames");
  REQUIRE(library_frames != nullptr && library_frames->is_array(),
          "lazily::json_parse should read the fixture's frames");
  REQUIRE(library_frames->array.size() == frames->array.size(),
          "both parses should see the same frame count");

  std::size_t checked = 0;
  for (std::size_t i = 0; i < frames->array.size(); ++i) {
    const Json& frame = *frames->array[i];
    const std::string label = lazily_test::json_string(lazily_test::json_member(frame, "label"));

    const JsonValue* wire = library_frames->array[i].find("wire");
    REQUIRE(wire != nullptr, "each frame carries a `wire` envelope");
    const IpcMessage message = json_to_ipc_message(*wire);
    const auto* envelope = std::get_if<IpcMessageCrdtSync>(&message);
    REQUIRE(envelope != nullptr, "each frame is an externally-tagged CrdtSync envelope");
    const CrdtSync& sync = envelope->value;

    // Re-encode and decode again: the assertions below then describe what the
    // CODEC produced, not what the fixture said. A codec that drops ops or
    // frontier entries on the way out satisfies a decode-only check.
    const std::string encoded = encode_json(message);
    const IpcMessage round = decode_json(encoded);
    REQUIRE(round == message, "a CrdtSync frame must survive a json round trip");
    const CrdtSync& round_sync = std::get<IpcMessageCrdtSync>(round).value;

    lazily_test::AssertionKeys expect(std::string(kArea) + "/" + kFramesFixture + " frames[" +
                                          label + "].assertions",
                                      lazily_test::json_member(frame, "assertions"));
    // Every key is optional per frame, so each comparison binds to the key's
    // presence rather than to a bare read (`#lzconsumednotasserted`).
    if (expect.assert_key_if_present("frontier_len",
                                     static_cast<int64_t>(round_sync.frontier.size())))
      ++checked;
    if (expect.assert_key_if_present("op_count", static_cast<int64_t>(round_sync.ops.size())))
      ++checked;
    if (expect.assert_key_with_if_present("frontier_omitted", [&](const Json& want) {
          const JsonValue& body = wire->object.front().second;
          if (lazily_test::json_bool(want) != (body.find("frontier") == nullptr)) return false;
          // An omitted frontier decodes as empty rather than as a placeholder,
          // and — the half a decode-only runner cannot see — re-encoding must
          // keep it omitted. Writing `"frontier": []` back out would turn
          // "unchanged since the last accepted frame" into a positive claim
          // that the sender advertises nothing.
          if (!round_sync.frontier.empty()) return false;
          const JsonValue reencoded = json_parse(encoded);
          return reencoded.object.front().second.find("frontier") == nullptr;
        }))
      ++checked;
    if (expect.assert_key_with_if_present("has_keyed_op", [&](const Json& want) {
          const bool keyed = std::any_of(round_sync.ops.begin(), round_sync.ops.end(),
                                         [](const CrdtOp& op) { return op.key.has_value(); });
          return keyed == lazily_test::json_bool(want);
        }))
      ++checked;
    if (expect.assert_key_with_if_present("has_keyless_op", [&](const Json& want) {
          const bool keyless = std::any_of(round_sync.ops.begin(), round_sync.ops.end(),
                                           [](const CrdtOp& op) { return !op.key.has_value(); });
          return keyless == lazily_test::json_bool(want);
        }))
      ++checked;
    expect.finish();
    ++g_scenarios;
  }

  REQUIRE(checked >= 4, "too little asserted across the crdt sync frames");
  g_checks += checked;
  std::cout << "crdt sync frames: " << frames->array.size() << " frames, " << checked
            << " assertions" << std::endl;
}

int main() {
  replay_crdt_sync_frames();
  const std::string text = lazily_test::spec_fixture_text(kArea, kFixture);
  const JsonPtr fixture = parse_json(text);
  REQUIRE(fixture->is_object(), "fixture root is not an object");

  const Json* model = fixture->find("model");
  REQUIRE(model != nullptr && model->str == "CrdtPlane", "fixture is not a CrdtPlane corpus");

  const Json* scenarios = fixture->find("scenarios");
  REQUIRE(scenarios != nullptr && scenarios->is_array() && !scenarios->array.empty(),
          "fixture has no scenarios to replay");

  for (std::size_t scenario_index = 0; scenario_index < scenarios->array.size(); ++scenario_index) {
    const auto& scenario_node = scenarios->array[scenario_index];
    REQUIRE(scenario_node->is_object(), "a scenario is not an object");
    lazily_test::record_scenario_at(std::string(kArea) + "/" + kFixture, *scenario_node,
                                    scenario_index);
    for (const auto& kv : scenario_node->object)
      REQUIRE(is_known_scenario_key(kv.first),
              "unrecognised distributed scenario key in fixture — it would be "
              "silently ignored");

    const Json* name_node = scenario_node->find("name");
    REQUIRE(name_node != nullptr && name_node->type == Json::Type::String,
            "a scenario has no name");
    const std::string name = name_node->str;

    const Json* ops_node = scenario_node->find("ops");
    REQUIRE(ops_node != nullptr && ops_node->is_array() && !ops_node->array.empty(),
            "a scenario has no ops");
    std::vector<CrdtOp> ops;
    for (const auto& op_node : ops_node->array)
      ops.push_back(op_of(op_node.get()));

    const Json* expect = scenario_node->find("expect");
    REQUIRE(expect != nullptr && expect->is_object(), "a scenario has no expect block");
    for (const auto& kv : expect->object)
      REQUIRE(is_known_expect_key(kv.first),
              "unrecognised distributed expect key in fixture — it would be "
              "silently ignored");

    // Only the max-stamp rule is modelled. A new rule landing upstream must fail
    // here rather than being replayed under the wrong resolution.
    const Json* resolution = expect->find("resolution");
    REQUIRE(resolution != nullptr && resolution->str == "max_stamp",
            "unknown convergence resolution rule in fixture");

    const Json* want_applied = expect->find("applied_count");
    REQUIRE(want_applied != nullptr, "a scenario has no applied_count");

    CrdtPlaneRuntime runtime(99);
    CrdtSync frame{{}, ops};
    const int applied = runtime.ingest(frame);
    g_ops_ingested += ops.size();
    ++g_scenarios;
    ++g_checks;
    if (applied != static_cast<int>(want_applied->as_int())) {
      std::cout << "FAIL: " << name << ": applied_count expected " << want_applied->as_int()
                << ", got " << applied << std::endl;
      std::abort();
    }
    assert_converged(name, runtime, expect->find("converged"));

    // State-based CvRDT idempotence: re-delivering the same frame applies
    // nothing new and leaves the converged state untouched. Asserted for EVERY
    // scenario, not only the one that names `redeliver` — idempotence is a
    // property of the plane, not of a fixture flag.
    const int re_applied = runtime.ingest(frame);
    ++g_checks;
    if (re_applied != 0) {
      std::cout << "FAIL: " << name << ": re-delivery applied " << re_applied << " ops, expected 0"
                << std::endl;
      std::abort();
    }
    if (const Json* want_re = expect->find("redeliver_applied_count")) {
      ++g_checks;
      REQUIRE(re_applied == static_cast<int>(want_re->as_int()),
              "redeliver_applied_count does not match the fixture");
    }
    assert_converged(name, runtime, expect->find("converged"));

    // Delivery-order independence: a fresh replica fed the reversed sequence
    // converges to the identical winner and accounts for the same op count.
    CrdtPlaneRuntime reversed_runtime(99);
    std::vector<CrdtOp> reversed(ops.rbegin(), ops.rend());
    const int rev_applied = reversed_runtime.ingest(CrdtSync{{}, reversed});
    g_ops_ingested += reversed.size();
    ++g_checks;
    if (rev_applied != static_cast<int>(want_applied->as_int())) {
      std::cout << "FAIL: " << name << ": reversed applied_count expected "
                << want_applied->as_int() << ", got " << rev_applied << std::endl;
      std::abort();
    }
    assert_converged(name + " (reversed)", reversed_runtime, expect->find("converged"));
  }

  REQUIRE(g_scenarios >= 3 && g_ops_ingested >= 20 && g_checks >= 20,
          "the anti-entropy replay did too little work to be meaningful — the "
          "corpus is empty, truncated, or short-circuited");

  std::cout << "distributed conformance: " << g_scenarios << " scenarios, " << g_ops_ingested
            << " ops ingested, " << g_checks << " assertions" << std::endl;

  REQUIRE_FIXTURES_LOADED(2);
  return 0;
}
