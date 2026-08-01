// Frame-codec round-trip conformance for the `json` REFERENCE codec
// (`#lzmsgpackparity`, `#lzcppjsoncodec`).
//
// protocol.md § Frame codecs makes `json` MUST-level for every binding and
// requires every frame to round-trip through it for all three IpcMessage
// variants. That requirement lived only in prose. The four conformance rungs —
// was the fixture OPENED, were its keys CONSUMED, were they ASSERTED, was every
// SCENARIO replayed — all reason about fixture *content*, and content replay
// never exercises a codec. lazily-cpp is the binding that proves the point: it
// shipped include/lazily/codec.hpp, looked complete, and could not read or
// write a single JSON frame.
//
// Each scenario decodes `wire`, RE-ENCODES the decoded message, decodes again,
// and evaluates every `expect` key against that SECOND decode. Asserting
// against the fixture literal would prove nothing — the literal never passed
// through an encoder.
//
// The fixture bytes are parsed twice on purpose: once by tests/test_json.hpp,
// which drives the harness (scenario ledger, assertion-key guards), and once by
// the library's own lazily::json_parse, which is what the codec consumes. The
// second parse is part of what is under test: a reference codec that cannot
// read the canonical corpus is not a reference codec.

#include <lazily/json_codec.hpp>

#include "test_assertion_keys.hpp"
#include "test_json.hpp"
#include "test_spec_fixture.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
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

static const char* const kFixtureArea = "codec";
static const char* const kFixtureName = "frame_roundtrip_json.json";
static const char* const kFixtureId = "codec/frame_roundtrip_json.json";

static const std::string& fixture_text() {
  static const std::string text = lazily_test::spec_fixture_text(kFixtureArea, kFixtureName);
  return text;
}

// Harness view of the fixture (scenario ledger + assertion-key guards).
static lazily_test::JsonPtr harness_fixture() { return lazily_test::parse_json(fixture_text()); }

// Library view of the same bytes — the input the codec actually consumes.
static const JsonValue& library_fixture() {
  static const JsonValue root = json_parse(fixture_text());
  return root;
}

// -- helpers over the ROUND-TRIPPED message -----------------------------------

static const char* node_state_tag(const NodeState& state) {
  if (std::holds_alternative<NodeStatePayload>(state)) return "Payload";
  if (std::holds_alternative<NodeStateSharedBlob>(state)) return "SharedBlob";
  return "Opaque";
}

static std::vector<uint8_t> json_bytes(const lazily_test::Json& value) {
  std::vector<uint8_t> out;
  for (const auto& element : lazily_test::json_array(value))
    out.push_back(static_cast<uint8_t>(lazily_test::json_u64(*element)));
  return out;
}

static std::vector<int64_t> json_ints(const lazily_test::Json& value) {
  std::vector<int64_t> out;
  for (const auto& element : lazily_test::json_array(value))
    out.push_back(static_cast<int64_t>(lazily_test::json_u64(*element)));
  return out;
}

static std::vector<std::string> json_strings(const lazily_test::Json& value) {
  std::vector<std::string> out;
  for (const auto& element : lazily_test::json_array(value))
    out.push_back(lazily_test::json_string(*element));
  return out;
}

static const std::vector<uint8_t>& node_state_payload(const NodeState& state) {
  const auto* payload = std::get_if<NodeStatePayload>(&state);
  REQUIRE(payload != nullptr, "node should carry Payload bytes after the round trip");
  return payload->bytes;
}

static const std::vector<uint8_t>& inline_payload(const IpcValue& value) {
  const auto* inline_value = std::get_if<IpcValueInline>(&value);
  REQUIRE(inline_value != nullptr, "op payload should be Inline after the round trip");
  return inline_value->bytes;
}

// -- per-variant assertions ---------------------------------------------------

static void assert_snapshot(lazily_test::AssertionKeys& expect, const Snapshot& snapshot) {
  expect.assert_key("epoch", snapshot.epoch);
  expect.assert_key("node_count", static_cast<int64_t>(snapshot.nodes.size()));
  expect.assert_key("edge_count", static_cast<int64_t>(snapshot.edges.size()));
  expect.assert_key("root_count", static_cast<int64_t>(snapshot.roots.size()));
  REQUIRE(!snapshot.nodes.empty(), "snapshot should carry nodes after the round trip");
  expect.assert_key("first_node_type_tag", snapshot.nodes.front().type_tag);
  // The discriminating field: a codec that re-encodes `Payload` as a base64
  // string or truncates the array still matches node_count/epoch.
  expect.assert_key("first_node_payload", node_state_payload(snapshot.nodes.front().state),
                    json_bytes);
  // Read the Opaque node POSITIONALLY (the fixture's last node) rather than by
  // searching for one that is still Opaque — a search would make the tag
  // assertion below tautological, and the unit variant decaying into
  // `{"Opaque": null}` or a Payload is exactly what it exists to catch.
  const NodeSnapshot& opaque = snapshot.nodes.back();
  expect.assert_key("opaque_node_id", opaque.node);
  expect.assert_key("opaque_node_state_tag", std::string(node_state_tag(opaque.state)));
  REQUIRE(!snapshot.edges.empty(), "snapshot should carry edges after the round trip");
  expect.assert_key(
      "first_edge",
      std::vector<int64_t>{snapshot.edges.front().dependent, snapshot.edges.front().dependency},
      json_ints);
  expect.assert_key("roots", std::vector<int64_t>(snapshot.roots.begin(), snapshot.roots.end()),
                    json_ints);
}

static void assert_delta(lazily_test::AssertionKeys& expect, const Delta& delta) {
  expect.assert_key("base_epoch", delta.base_epoch);
  expect.assert_key("epoch", delta.epoch);
  expect.assert_key("op_count", static_cast<int64_t>(delta.ops.size()));
  // Op ORDER and op IDENTITY together: an internally tagged re-encode or a
  // reordering fails here while op_count still matches.
  std::vector<std::string> variants;
  for (const auto& op : delta.ops)
    variants.emplace_back(delta_op_variant_name(op));
  expect.assert_key("op_variants", variants, json_strings);
  REQUIRE(!delta.ops.empty(), "delta should carry ops after the round trip");
  const auto* first = std::get_if<DeltaOpCellSet>(&delta.ops.front());
  REQUIRE(first != nullptr, "first delta op should be a CellSet after the round trip");
  expect.assert_key("first_op_payload", inline_payload(first->payload), json_bytes);
  const DeltaOpNodeAdd* node_add = nullptr;
  for (const auto& op : delta.ops) {
    if (const auto* candidate = std::get_if<DeltaOpNodeAdd>(&op)) node_add = candidate;
  }
  REQUIRE(node_add != nullptr, "fixture pins a NodeAdd op");
  expect.assert_key("node_add_type_tag", node_add->type_tag);
}

static void assert_crdt_sync(lazily_test::AssertionKeys& expect, const CrdtSync& sync) {
  expect.assert_key("frontier_len", static_cast<int64_t>(sync.frontier.size()));
  REQUIRE(!sync.frontier.empty(), "crdt sync should carry a frontier after the round trip");
  expect.assert_key("frontier_first_peer", sync.frontier.front().peer);
  expect.assert_key("frontier_first_stamp_wall_time", sync.frontier.front().stamp.wall_time);
  expect.assert_key("op_count", static_cast<int64_t>(sync.ops.size()));
  REQUIRE(sync.ops.size() >= 2, "fixture pins a keyless op and a keyed op");
  expect.assert_key("first_op_node", sync.ops[0].node);
  // A decoded-value assertion, not an encoding one: json WRITES `key` for a
  // CrdtOp (null when unset). What must survive is that the decoder reads that
  // null back as absent and the re-encoder does not invent a key.
  expect.assert_key("first_op_key_absent", !sync.ops[0].key.has_value());
  expect.assert_key("second_op_node", sync.ops[1].node);
  REQUIRE(sync.ops[1].key.has_value(), "second op should carry a NodeKey after the round trip");
  expect.assert_key("second_op_key", sync.ops[1].key->path());
  expect.assert_key("second_op_stamp_peer", sync.ops[1].stamp.peer);
}

static void assert_values(lazily_test::AssertionKeys& expect, const IpcMessage& message) {
  if (const auto* snapshot = std::get_if<IpcMessageSnapshot>(&message)) {
    assert_snapshot(expect, snapshot->value);
  } else if (const auto* delta = std::get_if<IpcMessageDelta>(&message)) {
    assert_delta(expect, delta->value);
  } else if (const auto* sync = std::get_if<IpcMessageCrdtSync>(&message)) {
    assert_crdt_sync(expect, sync->value);
  } else {
    REQUIRE(false, "codec fixture pins no runner for this IpcMessage variant");
  }
}

// -- the replay ---------------------------------------------------------------

// The fixture-level `assertions` block: the codec's identity and the two
// distinct senses of "canonical" protocol.md keeps apart (`role` is a ROLE,
// `byte_canonical` is a property of codec+message). An unread block is exactly
// the drift #lzassertunknownkeys exists to catch.
TEST(test_json_codec_fixture_block) {
  const auto fx = harness_fixture();
  REQUIRE(lazily_test::json_u64(lazily_test::json_member(*fx, "protocol_version")) == 1,
          "fixture protocol_version");
  REQUIRE(lazily_test::json_string(lazily_test::json_member(*fx, "kind")) == "FrameCodecRoundTrip",
          "fixture kind");
  REQUIRE(lazily_test::json_string(lazily_test::json_member(*fx, "codec")) == "json",
          "fixture codec field");

  const auto scenario_count =
      lazily_test::json_array(lazily_test::json_member(*fx, "scenarios")).size();
  lazily_test::AssertionKeys assertions(std::string(kFixtureId) + " assertions",
                                        lazily_test::json_member(*fx, "assertions"));
  assertions.assert_key("codec", std::string("json"));
  assertions.assert_key("self_describing", true);
  assertions.assert_key("byte_canonical", true);
  assertions.assert_key("required_of_binding", std::string("MUST"));
  assertions.assert_key("role", std::string("reference"));
  assertions.assert_key("scenario_count", static_cast<int64_t>(scenario_count));
  assertions.finish();
}

TEST(test_json_frames_round_trip) {
  const auto fx = harness_fixture();
  const auto& scenarios = lazily_test::json_array(lazily_test::json_member(*fx, "scenarios"));
  const JsonValue* library_scenarios = library_fixture().find("scenarios");
  REQUIRE(library_scenarios != nullptr && library_scenarios->is_array(),
          "lazily::json_parse should read the canonical fixture's scenarios");
  REQUIRE(library_scenarios->array.size() == scenarios.size(),
          "both parses should see the same scenario count");

  std::size_t replayed = 0;
  for (std::size_t i = 0; i < scenarios.size(); ++i) {
    const auto& scenario = *scenarios[i];
    const std::string id = lazily_test::record_scenario_at(kFixtureId, scenario, i);

    const JsonValue* wire = library_scenarios->array[i].find("wire");
    REQUIRE(wire != nullptr, "scenario should carry a `wire` frame");
    const IpcMessage source = json_to_ipc_message(*wire);
    REQUIRE(lazily_test::json_string(lazily_test::json_member(scenario, "variant")) ==
                ipc_message_variant_name(source),
            "fixture `variant` disagrees with the decoded frame");

    // Encode the DECODED message and decode the result. The fixture literal is
    // never re-asserted, so a codec that silently drops a field cannot hide
    // behind reading its own input back.
    const std::string encoded = encode_json(source);
    const IpcMessage round = decode_json(encoded);

    lazily_test::AssertionKeys expect(std::string(kFixtureId) + " scenarios[" + id + "].expect",
                                      lazily_test::json_member(scenario, "expect"));
    expect.assert_key("round_trip_equals_source", round == source);
    assert_values(expect, round);
    expect.finish();
    ++replayed;
  }
  REQUIRE(replayed == 3, "one scenario per IpcMessage variant");
}

// json is byte-canonical (§ Frame codecs): one message, one byte form. The
// round trip above pins the VALUE; this pins the byte property the fixture's
// `byte_canonical: true` claims, which no value assertion can see.
TEST(test_json_encoding_is_byte_canonical) {
  const JsonValue* library_scenarios = library_fixture().find("scenarios");
  REQUIRE(library_scenarios != nullptr, "scenarios");
  for (const auto& scenario : library_scenarios->array) {
    const JsonValue* wire = scenario.find("wire");
    REQUIRE(wire != nullptr, "scenario should carry a `wire` frame");
    const std::string first = encode_json(json_to_ipc_message(*wire));
    const std::string second = encode_json(decode_json(first));
    REQUIRE(first == second, "json encoding should be byte-canonical across a round trip");
  }
}

// The variants the corpus does not carry a frame for. They are part of the same
// externally tagged envelope (FFI message kinds 4/5), so a codec that handles
// only the three fixture variants is incomplete in a way the fixture cannot
// see.
TEST(test_json_codec_covers_control_frames) {
  const IpcMessage request = ipc_resync_request(12);
  REQUIRE(decode_json(encode_json(request)) == request, "ResyncRequest should round-trip");
  const IpcMessage ack = ipc_outbox_ack(41);
  REQUIRE(decode_json(encode_json(ack)) == ack, "OutboxAck should round-trip");

  // SharedBlob descriptors and the backend discriminator: `backend` is omitted
  // when default, so both arms have to survive independently.
  Snapshot snapshot;
  snapshot.epoch = 3;
  snapshot.nodes.push_back({1, "blob", NodeStateSharedBlob{{8, 16, 2, 3, 99, BlobBackendKind::Shm}},
                            NodeKey::create("docs/a")});
  snapshot.nodes.push_back(
      {2, "arrow", NodeStateSharedBlob{{9, 17, 4, 5, 100, BlobBackendKind::Arrow}}, std::nullopt});
  snapshot.roots = {1};
  const IpcMessage message = IpcMessageSnapshot{snapshot};
  const IpcMessage decoded = decode_json(encode_json(message));
  REQUIRE(decoded == message, "SharedBlob nodes and the backend discriminator should round-trip");

  // The omit-when-absent rule is an ENCODING property (§ NodeKey): a keyless
  // NodeSnapshot writes no `key` at all, while a keyless CrdtOp writes `null`.
  const std::string encoded = encode_json(message);
  REQUIRE(encoded.find("\"key\":\"docs/a\"") != std::string::npos,
          "a present NodeKey should be written");
  REQUIRE(encoded.find("\"key\":null") == std::string::npos,
          "an absent NodeSnapshot key should be OMITTED, not written as null");

  CrdtSync sync;
  sync.frontier.push_back({1, WireStamp{5, 0, 1}});
  sync.ops.push_back({7, std::nullopt, WireStamp{5, 0, 1}, IpcValueInline{{1}}});
  const IpcMessage crdt = IpcMessageCrdtSync{sync};
  const std::string crdt_encoded = encode_json(crdt);
  REQUIRE(crdt_encoded.find("\"key\":null") != std::string::npos,
          "an absent CrdtOp key should be written as null, not omitted");
  REQUIRE(decode_json(crdt_encoded) == crdt, "a keyless CrdtOp should round-trip");
}

// A malformed frame is not a frame: the codec has to refuse it rather than
// return a partially populated message.
TEST(test_json_codec_rejects_malformed_frames) {
  bool threw = false;
  try {
    (void)decode_json("{\"NotAVariant\":{}}");
  } catch (const std::runtime_error&) {
    threw = true;
  }
  REQUIRE(threw, "an unknown envelope tag should be rejected");

  threw = false;
  try {
    (void)decode_json("{\"Snapshot\":{\"epoch\":1,\"nodes\":[],\"edges\":[]}}");
  } catch (const std::runtime_error&) {
    threw = true;
  }
  REQUIRE(threw, "a missing required field should be rejected");

  threw = false;
  try {
    (void)decode_json("{\"Snapshot\":");
  } catch (const std::runtime_error&) {
    threw = true;
  }
  REQUIRE(threw, "truncated JSON should be rejected");
}

int main() {
  REQUIRE_FIXTURES_LOADED(1);
  return test_count == test_passed ? 0 : 1;
}
