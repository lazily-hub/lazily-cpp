// Frame-codec round-trip conformance for BOTH MUST-level codecs: `json`, the
// REFERENCE codec (`#lzmsgpackparity`, `#lzcppjsoncodec`), and `msgpack`, the
// CROSS-LANGUAGE BINARY DEFAULT (`#lzcppmsgpackwire`).
//
// protocol.md § Frame codecs makes both MUST-level for every binding and
// requires every frame to round-trip through each for all three IpcMessage
// variants. That requirement lived only in prose. The four conformance rungs —
// was the fixture OPENED, were its keys CONSUMED, were they ASSERTED, was every
// SCENARIO replayed — all reason about fixture *content*, and content replay
// never exercises a codec. lazily-cpp is the binding that proves the point
// twice: it shipped include/lazily/codec.hpp, looked complete, and could not
// read or write a single JSON frame — and the MessagePack codec that header
// does ship is a private internally-tagged framing, not the `msgpack` wire, a
// divergence no file-presence audit can see.
//
// Each scenario decodes `wire`, RE-ENCODES the decoded message, decodes again,
// and evaluates every `expect` key against that SECOND decode. Asserting
// against the fixture literal would prove nothing — the literal never passed
// through an encoder.
//
// The msgpack half additionally introspects the ENCODED BYTES through
// lazily::msgpack_to_json. The named-field rule is a property of the encoding,
// so no assertion over a decoded message can see it: a positional encoder
// round-trips every value correctly and is still non-conforming.
//
// The fixture bytes are parsed twice on purpose: once by tests/test_json.hpp,
// which drives the harness (scenario ledger, assertion-key guards), and once by
// the library's own lazily::json_parse, which is what the codec consumes. The
// second parse is part of what is under test: a reference codec that cannot
// read the canonical corpus is not a reference codec.

#include <lazily/codec.hpp>
#include <lazily/json_codec.hpp>
#include <lazily/msgpack_codec.hpp>

#include "test_assertion_keys.hpp"
#include "test_json.hpp"
#include "test_spec_fixture.hpp"

#include <algorithm>
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
static const char* const kMsgpackFixtureName = "frame_roundtrip_msgpack.json";
static const char* const kMsgpackFixtureId = "codec/frame_roundtrip_msgpack.json";

static const std::string& fixture_text() {
  static const std::string text = lazily_test::spec_fixture_text(kFixtureArea, kFixtureName);
  return text;
}

static const std::string& msgpack_fixture_text() {
  static const std::string text = lazily_test::spec_fixture_text(kFixtureArea, kMsgpackFixtureName);
  return text;
}

// Harness view of the fixture (scenario ledger + assertion-key guards).
static lazily_test::JsonPtr harness_fixture() { return lazily_test::parse_json(fixture_text()); }

static lazily_test::JsonPtr harness_msgpack_fixture() {
  return lazily_test::parse_json(msgpack_fixture_text());
}

// Library view of the same bytes — the input the codec actually consumes.
static const JsonValue& library_fixture() {
  static const JsonValue root = json_parse(fixture_text());
  return root;
}

static const JsonValue& library_msgpack_fixture() {
  static const JsonValue root = json_parse(msgpack_fixture_text());
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

// -- the fixture `assertions` block, asserted against THE RUN ------------------
//
// `#lznullformblind`. `scenario_count` was compared against
// `scenarios.size()` — the fixture measured against ITSELF. Delete the replay
// loop entirely and it still passed: the exact vacuity
// `assertions.anti_vacuity` exists to name, sitting in the guard meant to
// enforce it.
//
// The reason it was written that way is structural, and it is why the shape
// went unnoticed here longest: the `assertions` block is a DIFFERENT `TEST`
// from the replay, and being declared first it also RUNS first, so nothing the
// run produced is in scope yet. `assert_key_against_run` captures the fixture's
// value here and performs the comparison at `verify_prose` in `main`, against
// the facts the replay records — the same fixture-scoped ledger
// `#lzprosekeyconvention` already uses to check a discharge from a block that
// has gone out of scope. Restructuring the tests was the alternative; carrying
// the tally is the mechanism that already exists.
//
// The OTHER keys in these blocks stay fixture-vs-literal on purpose, and the
// distinction is the point. `codec`, `role`, `self_describing`,
// `byte_canonical` and `required_of_binding` are CORPUS DECLARATIONS a binding
// pins by agreement, not facts a replay produces. `byte_canonical` is the
// clearest case: it states what two conforming bindings may do with the same
// message, so no single implementation's run can produce a comparable value —
// this encoder is deterministic, and every observation it can make is
// consistent with `true` even where the wire says `false`. Deferring those to a
// run tally would not remove a vacuity, it would manufacture a wrong assertion.

// The declared scenario tally, against the scenarios this run actually reached.
// Shared by both codec blocks: the two are the same guard over two wires, and a
// per-codec copy is where a self-comparison creeps back in.
static void assert_scenario_count_against_run(lazily_test::AssertionKeys& block) {
  block.assert_key_against_run("scenario_count", [](const lazily_test::Json& want,
                                                    const lazily_test::RunFacts& run) {
    return static_cast<long long>(lazily_test::json_u64(want)) == run.count("scenarios_replayed");
  });
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
  // Fixture IDENTITY, not an assertion key: which of the two corpora this test
  // opened. It says nothing about the run and is not meant to.
  REQUIRE(lazily_test::json_string(lazily_test::json_member(*fx, "codec")) == "json",
          "fixture codec field");

  lazily_test::AssertionKeys assertions(std::string(kFixtureId) + " assertions",
                                        lazily_test::json_member(*fx, "assertions"));
  // Corpus declarations, pinned by agreement (see the header comment above).
  assertions.assert_key("codec", std::string("json"));
  assertions.assert_key("self_describing", true);
  assertions.assert_key("byte_canonical", true);
  assertions.assert_key("required_of_binding", std::string("MUST"));
  assertions.assert_key("role", std::string("reference"));
  // The one key here that IS a fact about the run.
  assert_scenario_count_against_run(assertions);
  // `note` is declared prose by the corpus (`#lzprosekeyconvention`), so the
  // by-name annotation exemption does not reach it: it states an obligation.
  // The obligation is that ROLE and BYTE-CANONICALITY stay distinct senses, and
  // the two keys it names are the ones asserted separately just above. Both are
  // corpus declarations rather than run facts, which is what the paragraph is
  // ABOUT — it asks that two declared senses not be conflated, so the keys that
  // carry it are the declarations themselves.
  assertions.prose_key("note", {"role", "byte_canonical"});
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
  for (const auto& sv : lazily_test::scenario_views(kFixtureId, scenarios)) {
    // Rung 4 books on the PAYLOAD handoff (#lzscenariobodyskip), so a body
    // that stops short of replaying stops being booked.
    const auto& scenario = sv.replay();
    const std::string id = sv.id();
    const std::size_t i = sv.index();

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
  // The tally the `assertions` block — a different, and EARLIER, `TEST` — is
  // compared against (`#lznullformblind`). Deleting this loop leaves the fact
  // unrecorded, and reading an unrecorded fact aborts, so `scenario_count` can
  // no longer pass over a runner that decodes nothing.
  //
  // Recorded BEFORE the floor below, and the floor demoted to "something ran".
  // A hand-maintained `replayed == 3` sitting AHEAD of the fixture's own
  // `scenario_count` aborts on exactly the input that key exists to catch, so
  // the corpus assertion was unreachable for the only run that would falsify it.
  // The exact number is the corpus's to state.
  lazily_test::record_run_count(kFixtureId, "scenarios_replayed", static_cast<long long>(replayed));
  REQUIRE(replayed > 0, "the replay loop entered no scenario at all");
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

// -- msgpack: the cross-language binary default -------------------------------

// Field names of a msgpack map, SORTED. A MessagePack map's key order is
// encoder-defined (§ Frame codecs), so order is not a conformance property;
// membership is.
static std::vector<std::string> sorted_field_names(const JsonValue& value, const char* what) {
  REQUIRE(value.is_object(),
          std::string(what) + " should encode as a named-field map, not a positional array");
  std::vector<std::string> names;
  names.reserve(value.object.size());
  for (const auto& member : value.object)
    names.push_back(member.first);
  std::sort(names.begin(), names.end());
  return names;
}

static const JsonValue& encoded_member(const JsonValue& value, const char* key) {
  const JsonValue* found = value.find(key);
  REQUIRE(found != nullptr, std::string("encoded frame should carry `") + key + "`");
  return *found;
}

static const JsonValue& encoded_element(const JsonValue& value, std::size_t index,
                                        const char* what) {
  REQUIRE(value.is_array() && value.array.size() > index,
          std::string(what) + " should be an array with the pinned element");
  return value.array[index];
}

TEST(test_msgpack_codec_fixture_block) {
  const auto fx = harness_msgpack_fixture();
  REQUIRE(lazily_test::json_u64(lazily_test::json_member(*fx, "protocol_version")) == 1,
          "fixture protocol_version");
  REQUIRE(lazily_test::json_string(lazily_test::json_member(*fx, "kind")) == "FrameCodecRoundTrip",
          "fixture kind");
  // Fixture IDENTITY, not an assertion key: which of the two corpora this test
  // opened.
  REQUIRE(lazily_test::json_string(lazily_test::json_member(*fx, "codec")) == "msgpack",
          "fixture codec field");

  lazily_test::AssertionKeys assertions(std::string(kMsgpackFixtureId) + " assertions",
                                        lazily_test::json_member(*fx, "assertions"));
  // Corpus declarations, pinned by agreement (see the header comment above).
  assertions.assert_key("codec", std::string("msgpack"));
  assertions.assert_key("self_describing", true);
  // The distinction protocol.md keeps apart: `msgpack` is self-describing AND
  // not byte-canonical, which is why every assertion below reads a decoded
  // value or a field-name set rather than a golden byte string.
  assertions.assert_key("byte_canonical", false);
  assertions.assert_key("required_of_binding", std::string("MUST"));
  assertions.assert_key("role", std::string("cross_language_binary_default"));
  // The one key here that IS a fact about the run.
  assert_scenario_count_against_run(assertions);
  // `note` is declared prose by the corpus (`#lzprosekeyconvention`), so the
  // by-name annotation exemption does not reach it. Its obligation — a frame is
  // a MAP KEYED BY FIELD NAME, not a positional array, which is what keeps
  // omit-when-absent uniform across the two codecs — is carried by
  // `encoded_body_field_names` (asserted in the round-trip test below, which is
  // why verification is fixture-scoped and happens in `main`), by
  // `round_trip_equals_source` for the decoded values this fixture pins in place
  // of golden bytes, and by `byte_canonical` for the property that forced that
  // choice. The first two are run facts; the third is the corpus declaration the
  // paragraph exists to explain.
  assertions.prose_key("note",
                       {"byte_canonical", "encoded_body_field_names", "round_trip_equals_source"});
  assertions.finish();
}

TEST(test_msgpack_frames_round_trip) {
  const auto fx = harness_msgpack_fixture();
  const auto& scenarios = lazily_test::json_array(lazily_test::json_member(*fx, "scenarios"));
  const JsonValue* library_scenarios = library_msgpack_fixture().find("scenarios");
  REQUIRE(library_scenarios != nullptr && library_scenarios->is_array(),
          "lazily::json_parse should read the canonical fixture's scenarios");
  REQUIRE(library_scenarios->array.size() == scenarios.size(),
          "both parses should see the same scenario count");

  std::size_t replayed = 0;
  for (const auto& sv : lazily_test::scenario_views(kMsgpackFixtureId, scenarios)) {
    // Rung 4 books on the PAYLOAD handoff (#lzscenariobodyskip), so a body
    // that stops short of replaying stops being booked.
    const auto& scenario = sv.replay();
    const std::string id = sv.id();
    const std::size_t i = sv.index();

    // `wire` is written in the reference json form in both codec fixtures, so
    // the msgpack half starts from the same value and differs only in the
    // encoder under test.
    const JsonValue* wire = library_scenarios->array[i].find("wire");
    REQUIRE(wire != nullptr, "scenario should carry a `wire` frame");
    const IpcMessage source = json_to_ipc_message(*wire);
    REQUIRE(lazily_test::json_string(lazily_test::json_member(scenario, "variant")) ==
                ipc_message_variant_name(source),
            "fixture `variant` disagrees with the decoded frame");

    const std::vector<uint8_t> bytes = encode_msgpack(source);
    const IpcMessage round = decode_msgpack(bytes);

    // Schema-less view of the bytes actually produced. This is the only way to
    // see the named-field rule: codec.hpp's private framing passes every value
    // assertion below and fails here, and so does a positional encoder.
    const JsonValue generic = msgpack_to_json(bytes);
    REQUIRE(generic.is_object() && generic.object.size() == 1,
            "IpcMessage is externally tagged: a one-entry map");
    const std::string& tag = generic.object.front().first;
    const JsonValue& body = generic.object.front().second;

    lazily_test::AssertionKeys expect(std::string(kMsgpackFixtureId) + " scenarios[" + id +
                                          "].expect",
                                      lazily_test::json_member(scenario, "expect"));
    expect.assert_key("round_trip_equals_source", round == source);
    expect.assert_key("encoded_envelope_key", tag);
    expect.assert_key("encoded_body_field_names", sorted_field_names(body, "a frame body"),
                      json_strings);

    if (tag == "Snapshot") {
      // `NodeSnapshot.key` is optional and OMITTED when absent in a
      // self-describing codec — the rule that lets a pre-`key` decoder read a
      // post-`key` frame. It has to hold under msgpack exactly as under json,
      // and the pinned list carries no `key`.
      expect.assert_key("first_node_encoded_field_names",
                        sorted_field_names(
                            encoded_element(encoded_member(body, "nodes"), 0, "`nodes`"), "a node"),
                        json_strings);
    } else if (tag == "CrdtSync") {
      // A `CrdtOp` ALWAYS carries `key` (null when unset), so both lists do.
      const JsonValue& ops = encoded_member(body, "ops");
      expect.assert_key("first_op_encoded_field_names",
                        sorted_field_names(encoded_element(ops, 0, "`ops`"), "a crdt op"),
                        json_strings);
      expect.assert_key("second_op_encoded_field_names",
                        sorted_field_names(encoded_element(ops, 1, "`ops`"), "a crdt op"),
                        json_strings);
    }

    assert_values(expect, round);
    expect.finish();
    ++replayed;
  }
  // Recorded ahead of the floor, for the reason spelled out in the json half.
  lazily_test::record_run_count(kMsgpackFixtureId, "scenarios_replayed",
                                static_cast<long long>(replayed));
  REQUIRE(replayed > 0, "the replay loop entered no scenario at all");
}

// The variants the corpus carries no frame for, plus the two encoding rules the
// fixture pins only for the shapes it happens to contain. `msgpack` is a
// distinct wire from `json`, so neither rule is inherited from the json suite.
TEST(test_msgpack_codec_covers_control_frames) {
  const IpcMessage request = ipc_resync_request(12);
  REQUIRE(decode_msgpack(encode_msgpack(request)) == request, "ResyncRequest should round-trip");
  const IpcMessage ack = ipc_outbox_ack(41);
  REQUIRE(decode_msgpack(encode_msgpack(ack)) == ack, "OutboxAck should round-trip");

  Snapshot snapshot;
  snapshot.epoch = 3;
  snapshot.nodes.push_back({1, "blob", NodeStateSharedBlob{{8, 16, 2, 3, 99, BlobBackendKind::Shm}},
                            NodeKey::create("docs/a")});
  snapshot.nodes.push_back(
      {2, "arrow", NodeStateSharedBlob{{9, 17, 4, 5, 100, BlobBackendKind::Arrow}}, std::nullopt});
  snapshot.roots = {1};
  const IpcMessage message = IpcMessageSnapshot{snapshot};
  REQUIRE(decode_msgpack(encode_msgpack(message)) == message,
          "SharedBlob nodes and the backend discriminator should round-trip");

  const JsonValue encoded = msgpack_to_json(encode_msgpack(message));
  const JsonValue& nodes = encoded_member(encoded_member(encoded, "Snapshot"), "nodes");
  REQUIRE(encoded_element(nodes, 0, "`nodes`").find("key") != nullptr,
          "a present NodeKey should be written");
  REQUIRE(encoded_element(nodes, 1, "`nodes`").find("key") == nullptr,
          "an absent NodeSnapshot key should be OMITTED, not written as null");

  CrdtSync sync;
  sync.frontier.push_back({1, WireStamp{5, 0, 1}});
  sync.ops.push_back({7, std::nullopt, WireStamp{5, 0, 1}, IpcValueInline{{1}}});
  const IpcMessage crdt = IpcMessageCrdtSync{sync};
  const JsonValue crdt_encoded = msgpack_to_json(encode_msgpack(crdt));
  const JsonValue& op =
      encoded_element(encoded_member(encoded_member(crdt_encoded, "CrdtSync"), "ops"), 0, "`ops`");
  const JsonValue* op_key = op.find("key");
  REQUIRE(op_key != nullptr && op_key->is_null(),
          "an absent CrdtOp key should be written as null, not omitted");
  REQUIRE(decode_msgpack(encode_msgpack(crdt)) == crdt, "a keyless CrdtOp should round-trip");
}

// The `msgpack` wire is NOT codec.hpp's wire, and the whole point of this
// header is that the difference is observable. codec.hpp packs an internally
// tagged envelope with integer discriminators; a decoder for one must refuse
// the other rather than silently produce a message.
TEST(test_msgpack_codec_is_not_the_private_framing) {
  const IpcMessage message = ipc_resync_request(12);
  const std::vector<uint8_t> spec_bytes = encode_msgpack(message);
  const std::vector<uint8_t> private_bytes = encode(message);
  REQUIRE(spec_bytes != private_bytes, "the spec wire and the private framing should not coincide");

  const JsonValue spec_view = msgpack_to_json(spec_bytes);
  REQUIRE(spec_view.is_object() && spec_view.object.size() == 1 &&
              spec_view.object.front().first == "ResyncRequest",
          "the spec wire is externally tagged by variant NAME");
  const JsonValue private_view = msgpack_to_json(private_bytes);
  REQUIRE(private_view.find("type") != nullptr && private_view.find("value") != nullptr,
          "the private framing is internally tagged with an integer discriminator");

  bool threw = false;
  try {
    (void)decode_msgpack(private_bytes);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  REQUIRE(threw, "the spec decoder should refuse a private-framing frame");
}

// A malformed frame is not a frame.
TEST(test_msgpack_codec_rejects_malformed_frames) {
  bool threw = false;
  try {
    MsgPacker packer;
    packer.map_header(1);
    packer.str("NotAVariant");
    packer.map_header(0);
    (void)decode_msgpack(std::move(packer).take());
  } catch (const std::runtime_error&) {
    threw = true;
  }
  REQUIRE(threw, "an unknown envelope tag should be rejected");

  // A positional encoder round-trips every VALUE correctly and is still
  // non-conforming (§ Frame codecs). codec.hpp ships exactly that form, so the
  // decoder has to refuse it rather than read it as a frame.
  threw = false;
  try {
    (void)decode_msgpack(encode_positional(ipc_outbox_ack(41)));
  } catch (const std::runtime_error&) {
    threw = true;
  }
  REQUIRE(threw, "a positional array frame should be rejected");

  // MessagePack `bin` in a byte-payload position: the reference decoder rejects
  // it, so accepting it would put lazily-cpp outside the wire it claims.
  threw = false;
  try {
    MsgPacker packer;
    packer.map_header(1);
    packer.str("CrdtSync");
    packer.map_header(2);
    packer.str("frontier");
    packer.array_header(0);
    packer.str("ops");
    packer.array_header(1);
    packer.map_header(4);
    packer.str("node");
    packer.i64(1);
    packer.str("key");
    packer.nil();
    packer.str("stamp");
    packer.map_header(3);
    packer.str("wall_time");
    packer.i64(5);
    packer.str("logical");
    packer.i64(0);
    packer.str("peer");
    packer.i64(1);
    packer.str("state");
    packer.map_header(1);
    packer.str("Inline");
    packer.bin(std::vector<uint8_t>{1, 2, 3});
    (void)decode_msgpack(std::move(packer).take());
  } catch (const std::runtime_error&) {
    threw = true;
  }
  REQUIRE(threw, "a msgpack `bin` byte payload should be rejected");

  threw = false;
  try {
    std::vector<uint8_t> truncated = encode_msgpack(ipc_outbox_ack(41));
    truncated.pop_back();
    (void)decode_msgpack(truncated);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  REQUIRE(threw, "a truncated frame should be rejected");

  threw = false;
  try {
    std::vector<uint8_t> trailing = encode_msgpack(ipc_outbox_ack(41));
    trailing.push_back(0xc0);
    (void)decode_msgpack(trailing);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  REQUIRE(threw, "trailing bytes after a frame should be rejected");
}

int main() {
  // Every TEST above runs during static initialisation, so both fixtures'
  // replays are finished here. Prose verification is FIXTURE-scoped rather than
  // block-scoped precisely for this shape: the msgpack `note` is discharged by
  // `encoded_body_field_names`, asserted in a different test from the
  // `assertions` block that declared the paragraph.
  lazily_test::verify_prose(kFixtureId);
  lazily_test::verify_prose(kMsgpackFixtureId);
  REQUIRE_FIXTURES_LOADED(2);
  return test_count == test_passed ? 0 : 1;
}
