// NodeKey null-leniency on decode (`#lzkeynullstrict`).
//
// protocol.md § NodeKey said a self-describing codec OMITS an absent `key`, and
// that a decoder seeing no `key` field treats it as absent. That settled the
// omitted form and left an explicit `key: null` undefined — and three bindings
// diverged there. The clause is now explicit: omit-when-absent binds the
// ENCODER, and a decoder MUST accept both forms as absent, refusing neither and
// constructing a key from neither.
//
// lazily-cpp was already lenient in both codecs — the msgpack reader tests for
// `nil` before reading a string, and the json path treats a null value as
// absent. This runner is what holds it there, and pins the other half: the
// encoder must still OMIT the field, because a decoder that reads null as
// absent and writes it straight back out has a correct decoded value and a
// non-conforming encoder.
//
// Plain functions called from `main` rather than the usual self-registering TEST
// macro: that macro builds its names by token pasting, and the coined-id hook
// guarding this workspace reads the pasted form as an untracked tag.

#include <lazily/codec.hpp>
#include <lazily/json_codec.hpp>
#include <lazily/msgpack_codec.hpp>

#include "test_assertion_keys.hpp"
#include "test_json.hpp"
#include "test_spec_fixture.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

using namespace lazily;

static const char* const kFixtureArea = "codec";
static const char* const kFixtureName = "nodekey_null_leniency.json";
static const char* const kFixtureId = "codec/nodekey_null_leniency.json";

static const std::string& fixture_text() {
  static const std::string text = lazily_test::spec_fixture_text(kFixtureArea, kFixtureName);
  return text;
}

static std::vector<uint8_t> hex_to_bytes(const std::string& hex) {
  REQUIRE(hex.size() % 2 == 0, "hex string should have an even length");
  std::vector<uint8_t> out;
  out.reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2)
    out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
  return out;
}

static std::vector<uint8_t> json_bytes(const lazily_test::Json& value) {
  std::vector<uint8_t> out;
  for (const auto& element : lazily_test::json_array(value))
    out.push_back(static_cast<uint8_t>(lazily_test::json_u64(*element)));
  return out;
}

// The codec DISPATCH. Which arm was entered is recorded here, inside the arm and
// after the decoder returned, rather than tallied afterwards from the scenario's
// `codec` label (`#lznullformblind`). `assertions.codecs` is the claim that both
// decoders ran; a set built from the label is a set built from the fixture, and
// it is green over a runner that decodes nothing.
static IpcMessage decode_scenario(const lazily_test::Json& scenario,
                                  lazily_test::AssertionKeys& expect,
                                  std::set<std::string>& decoders_entered) {
  const std::string codec = lazily_test::json_string(lazily_test::json_member(scenario, "codec"));
  if (codec == "json") {
    const std::string raw =
        lazily_test::json_string(lazily_test::json_member(scenario, "wire_json"));
    expect.assert_key("wire_input_fnv1a64", lazily_test::fnv1a64_hex(raw));
    IpcMessage message = decode_json(raw);
    decoders_entered.insert("json");
    return message;
  }
  if (codec == "msgpack") {
    const std::vector<uint8_t> raw = hex_to_bytes(
        lazily_test::json_string(lazily_test::json_member(scenario, "wire_msgpack_hex")));
    expect.assert_key("wire_input_fnv1a64", lazily_test::fnv1a64_hex(raw));
    IpcMessage message = decode_msgpack(raw);
    decoders_entered.insert("msgpack");
    return message;
  }
  REQUIRE(false, "scenario names an unknown codec");
  return IpcMessage{};
}

// Walk to the node the scenario's `field` names, in a frame held schema-lessly.
// Shared by the re-encode reader and the raw-wire reader below, so both ask the
// same question of the same shape.
static const JsonValue& frame_node(const JsonValue& frame, const lazily_test::Json& scenario,
                                   const std::string& what) {
  const std::string field = lazily_test::json_string(lazily_test::json_member(scenario, "field"));
  // Fail closed (#lzscenariobodyskip). `field` fell through to the Delta arm for
  // ANY spelling that was not `snapshot`, so a renamed or misspelled field
  // silently re-encoded and asserted the node_add frame while the scenario was
  // still booked as replayed.
  REQUIRE(field == "snapshot" || field == "node_add", "unknown nodekey field in fixture: " + field);
  if (field == "snapshot") {
    const JsonValue* body = frame.find("Snapshot");
    REQUIRE(body != nullptr, what + " carries a Snapshot envelope");
    return body->find("nodes")->array.at(0);
  }
  const JsonValue* body = frame.find("Delta");
  REQUIRE(body != nullptr, what + " carries a Delta envelope");
  return *body->find("ops")->array.at(0).find("NodeAdd");
}

// THE FORM ON THE WIRE, read BEFORE the decoder runs.
//
// Without this control the `omitted` and `null` families of this fixture are
// indistinguishable in a runner: every `expect` key is identical for the two,
// and the typed `std::optional<NodeKey>` collapses them the instant the value is
// decoded. Four `null` scenarios would then be four `omitted` ones wearing a
// different id, and `assertions.key_forms` would be satisfied by a list of
// literals. Reading the raw slot is what makes the two forms separate facts —
// the same control the sibling blob-backend runner already applies to `backend`.
static std::string wire_key_form(const lazily_test::Json& scenario) {
  const std::string codec = lazily_test::json_string(lazily_test::json_member(scenario, "codec"));
  REQUIRE(codec == "json" || codec == "msgpack", "scenario names an unknown codec: " + codec);
  const JsonValue frame =
      codec == "msgpack"
          ? msgpack_to_json(hex_to_bytes(
                lazily_test::json_string(lazily_test::json_member(scenario, "wire_msgpack_hex"))))
          : json_parse(lazily_test::json_string(lazily_test::json_member(scenario, "wire_json")));
  const JsonValue& node = frame_node(frame, scenario, "the scenario's own wire");
  const JsonValue* slot = node.find("key");
  if (slot == nullptr) return "omitted";
  if (slot->is_null()) return "null";
  return "present";
}

// A SECOND witness for the msgpack form, taken WITHOUT the decoder.
//
// `wire_key_form` above reads a msgpack frame through `msgpack_to_json` — this
// binding's own decoder. That makes the control and the thing it controls share
// a dependency: a decoder that lost the nil/absent distinction would classify
// every scenario the same way AND decode every scenario the same way, and the
// two would agree all the way to green. This one reads the bytes directly.
//
// `a3 6b 65 79` is the MessagePack fixstr header for a 3-byte string followed by
// `key`; the byte after it is that entry's value, and `c0` is nil. No match at
// all is the OMITTED form — the entry is not on the wire.
static std::string raw_msgpack_key_form(const std::string& hex) {
  const std::vector<uint8_t> bytes = hex_to_bytes(hex);
  const uint8_t marker[4] = {0xa3, 'k', 'e', 'y'};
  for (std::size_t i = 0; i + 4 < bytes.size(); ++i) {
    if (bytes[i] != marker[0] || bytes[i + 1] != marker[1] || bytes[i + 2] != marker[2] ||
        bytes[i + 3] != marker[3])
      continue;
    return bytes[i + 4] == 0xc0 ? "null" : "present";
  }
  return "omitted";
}

// Re-encode under the scenario's own codec and read the field set back
// SCHEMA-LESSLY. The typed `std::optional<NodeKey>` cannot tell "field absent"
// from "field present and null", which is the whole distinction under test.
static const JsonValue& reencoded_node(const lazily_test::Json& scenario, const IpcMessage& message,
                                       JsonValue& owner) {
  const std::string codec = lazily_test::json_string(lazily_test::json_member(scenario, "codec"));
  if (codec == "msgpack") {
    // Through the msgpack encoder specifically: the `#lzmsgpackparity` defect
    // was a msgpack encoder writing `key: null` while json omitted it, so
    // asserting the json output for both would miss exactly that class.
    owner = msgpack_to_json(encode_msgpack(message));
  } else {
    owner = json_parse(encode_json(message));
  }
  return frame_node(owner, scenario, "re-encoded frame");
}

// The field DISPATCH, recorded the same way and for the same reason as the codec
// one: `fields_decoded` is booked on the arm whose envelope the DECODED message
// really turned out to be, so `assertions.fields` cannot be satisfied by the
// scenario's `field` label alone (`#lznullformblind`).
static std::optional<std::string> decoded_key(const lazily_test::Json& scenario,
                                              const IpcMessage& message,
                                              std::set<std::string>& fields_decoded) {
  const std::string field = lazily_test::json_string(lazily_test::json_member(scenario, "field"));
  // Fail closed (#lzscenariobodyskip) — see `reencoded_node`.
  REQUIRE(field == "snapshot" || field == "node_add", "unknown nodekey field in fixture: " + field);
  if (field == "snapshot") {
    const auto* envelope = std::get_if<IpcMessageSnapshot>(&message);
    REQUIRE(envelope != nullptr, "fixture declares the Snapshot variant");
    fields_decoded.insert("snapshot");
    const auto& key = envelope->value.nodes.front().key;
    return key ? std::optional<std::string>(std::string(key->path())) : std::nullopt;
  }
  const auto* envelope = std::get_if<IpcMessageDelta>(&message);
  REQUIRE(envelope != nullptr, "fixture declares the Delta variant");
  const auto* op = std::get_if<DeltaOpNodeAdd>(&envelope->value.ops.front());
  REQUIRE(op != nullptr, "fixture declares a NodeAdd op");
  fields_decoded.insert("node_add");
  return op->key ? std::optional<std::string>(std::string(op->key->path())) : std::nullopt;
}

static void test_nodekey_null_leniency_is_replayed() {
  const auto fx = lazily_test::parse_json(fixture_text());
  REQUIRE(lazily_test::json_u64(lazily_test::json_member(*fx, "protocol_version")) == 1,
          "protocol_version");
  REQUIRE(lazily_test::json_string(lazily_test::json_member(*fx, "kind")) == "NodeKeyNullLeniency",
          "kind");

  const auto& scenarios = lazily_test::json_array(lazily_test::json_member(*fx, "scenarios"));

  // Anti-vacuity in both directions. A runner that never decodes reports
  // "absent" for everything and satisfies all eight omitted/null scenarios; the
  // `present` count is what only a real decode can produce.
  std::size_t replayed = 0;
  std::size_t keys_decoded = 0;
  std::set<std::string> forms_seen; // read off each scenario's own wire
  // Booked inside the dispatch arms of `decoded_key` / `decode_scenario`, on the
  // envelope and the decoder that were really reached — never from the
  // scenario's own `field` / `codec` labels, which are the fixture describing
  // itself (`#lznullformblind`).
  std::set<std::string> fields_decoded;
  std::set<std::string> decoders_entered;

  for (const auto& sv : lazily_test::scenario_views(kFixtureId, scenarios)) {
    // Rung 4 books on the PAYLOAD handoff (#lzscenariobodyskip), so a body
    // that stops short of replaying stops being booked.
    const auto& scenario = sv.replay();
    const std::string id = sv.id();
    ++replayed;

    // The label and the bytes must agree before either is trusted, and the
    // reading happens BEFORE the decode that would collapse omitted into null.
    const std::string form =
        lazily_test::json_string(lazily_test::json_member(scenario, "key_form"));
    const std::string on_wire = wire_key_form(scenario);
    REQUIRE(form == on_wire, id + ": scenario declares key_form '" + form +
                                 "' but its own wire carries '" + on_wire +
                                 "' — the label and the bytes disagree");
    // The msgpack half of `wire_key_form` runs through this binding's own
    // decoder, so a defect there would corrupt the control and the thing
    // controlled together. The raw-byte witness has no such dependency.
    if (lazily_test::json_string(lazily_test::json_member(scenario, "codec")) == "msgpack") {
      const std::string raw = raw_msgpack_key_form(
          lazily_test::json_string(lazily_test::json_member(scenario, "wire_msgpack_hex")));
      REQUIRE(raw == on_wire,
              id + ": the two wire witnesses disagree — msgpack_to_json says '" + on_wire +
                  "', the raw bytes say '" + raw +
                  "'. A decoder defect that moved both would be invisible, which is why "
                  "there are two");
    }
    forms_seen.insert(on_wire);

    lazily_test::AssertionKeys expect(std::string(kFixtureId) + " scenarios[" + id + "].expect",
                                      lazily_test::json_member(scenario, "expect"));

    const IpcMessage message = decode_scenario(scenario, expect, decoders_entered);
    const auto key = decoded_key(scenario, message, fields_decoded);
    if (key) ++keys_decoded;

    // The decode half: omitted and explicit-null must both arrive absent.
    expect.assert_key_with("decoded_key", [&](const lazily_test::Json& want) {
      if (want.is_null()) return !key.has_value();
      return key.has_value() && *key == lazily_test::json_string(want);
    });

    JsonValue owner;
    const JsonValue& node = reencoded_node(scenario, message, owner);
    // The encode half, which no assertion over the decoded value reaches.
    const JsonValue* encoded = node.find("key");
    expect.assert_key("reencoded_key_field_present", encoded != nullptr && !encoded->is_null());

    expect.assert_key("node", node.find("node")->as_int());
    expect.assert_key("type_tag", node.find("type_tag")->as_string());
    expect.assert_key_with("payload", [&](const lazily_test::Json& want) {
      const auto expected = json_bytes(want);
      const auto& actual = node.find("state")->find("Payload")->array;
      if (expected.size() != actual.size()) return false;
      for (std::size_t b = 0; b < expected.size(); ++b) {
        if (static_cast<int64_t>(expected[b]) != actual[b].as_int()) return false;
      }
      return true;
    });
    expect.assert_key("epoch", std::visit(
                                   [](const auto& body) -> int64_t {
                                     using T = std::decay_t<decltype(body)>;
                                     if constexpr (std::is_same_v<T, IpcMessageSnapshot>)
                                       return body.value.epoch;
                                     else if constexpr (std::is_same_v<T, IpcMessageDelta>)
                                       return body.value.epoch;
                                     else
                                       return -1;
                                   },
                                   message));
    expect.finish();
  }

  // The assertion block is evaluated AFTER the replay. `scenario_count` against
  // `scenarios.size()` would be the fixture compared to itself — green over a
  // runner that decodes nothing, which is precisely the vacuity
  // `assertions.anti_vacuity` exists to name — so it is compared against the
  // scenarios this run actually reached, and `key_forms` against the forms read
  // off their wires.
  {
    lazily_test::AssertionKeys block(std::string(kFixtureId) + " assertions",
                                     lazily_test::json_member(*fx, "assertions"));
    block.assert_key("required_of_binding", std::string("MUST"));
    block.assert_key("scenario_count", static_cast<int64_t>(replayed));
    // Both directions against the DECODERS THIS RUN ENTERED, not against a count
    // of the scenarios' own `codec` labels (`#lznullformblind`): the previous
    // `codecs_seen.size() == 2` was satisfied by a fixture carrying two labels
    // and said nothing about whether either decoder ran.
    block.assert_key_with("codecs", [&](const lazily_test::Json& want) {
      const auto& list = lazily_test::json_array(want);
      if (list.size() != 2 || lazily_test::json_string(*list[0]) != "json" ||
          lazily_test::json_string(*list[1]) != "msgpack")
        return false;
      std::set<std::string> declared;
      for (const auto& element : list)
        declared.insert(lazily_test::json_string(*element));
      return declared == decoders_entered;
    });
    // Likewise: booked on the envelope the decode really produced, so a fixture
    // that labels a frame `node_add` and carries a Snapshot cannot satisfy it.
    block.assert_key_with("fields", [&](const lazily_test::Json& want) {
      const auto& list = lazily_test::json_array(want);
      std::set<std::string> declared;
      for (const auto& element : list)
        declared.insert(lazily_test::json_string(*element));
      return list.size() == 2 && lazily_test::json_string(*list[0]) == "snapshot" &&
             lazily_test::json_string(*list[1]) == "node_add" && declared == fields_decoded;
    });
    // Both directions, against the RAW WIRE rather than a list of literals:
    // every declared form was carried by a scenario whose own bytes this runner
    // read back before decoding, and no scenario carried a form the block does
    // not declare. A literal comparison here is green over a runner that never
    // opens a frame, and it cannot see `null` collapsing into `omitted`.
    block.assert_key_with("key_forms", [&](const lazily_test::Json& want) {
      const auto& list = lazily_test::json_array(want);
      const char* const expected[] = {"omitted", "null", "present"};
      if (list.size() != 3) return false;
      std::set<std::string> declared;
      for (std::size_t i = 0; i < list.size(); ++i) {
        if (lazily_test::json_string(*list[i]) != expected[i]) return false;
        declared.insert(expected[i]);
      }
      return declared == forms_seen;
    });

    // The four paragraphs the corpus declares in `assertions.prose`, each
    // DISCHARGED by naming the executable keys this fixture's run asserts
    // (`#lzprosekeyconvention`). `verify_prose` below refuses a name no block of
    // this replay actually asserted, which is what makes the naming falsifiable
    // where the free-text reason it replaces was not.
    //
    // The clause has two halves and needs both keys: `decoded_key` is the
    // decoder accepting omitted and explicit-null alike and constructing a key
    // from neither, `reencoded_key_field_present` is the encoder still emitting
    // the OMITTED form. `reencode_obligation` is the second half alone — the
    // paragraph says so by name.
    block.prose_key("clause", {"decoded_key", "reencoded_key_field_present"});
    // Executable proof that the exact raw text / decoded-hex byte buffer reaches
    // the library decoder rather than a reconstructed proxy.
    block.prose_key("wire_encoding", {"wire_input_fnv1a64"});
    block.prose_key("reencode_obligation", {"reencoded_key_field_present"});
    block.prose_key("anti_vacuity", {"decoded_key", "key_forms", "scenario_count"});
    block.excuse_key("generator", "names the corpus script that emits this fixture, not a fact "
                                  "about the frames under test");
    block.finish();
  }

  // Checks every discharge above against what this run asserted. The ledger's
  // teardown fails a run that omits this call.
  lazily_test::verify_prose(kFixtureId);

  // The runner-side floors sit BELOW the fixture's own block, not above it
  // (`#lznullformblind`). A hand-maintained `replayed == 12` ahead of
  // `assertions.scenario_count` aborts on exactly the input that key exists to
  // catch, making the corpus assertion unreachable for the only run that would
  // falsify it. The exact count is the corpus's to state; what stays here is the
  // control the corpus does NOT carry.
  REQUIRE(replayed > 0, "the replay loop entered no scenario at all");
  REQUIRE(keys_decoded == 4,
          "only the `present` scenarios carry a key; a runner reporting absent for everything "
          "satisfies the null cases trivially");
}

int main() {
  test_nodekey_null_leniency_is_replayed();
  REQUIRE_FIXTURES_LOADED(1);
  return 0;
}
