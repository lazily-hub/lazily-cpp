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

static IpcMessage decode_scenario(const lazily_test::Json& scenario) {
  const std::string codec = lazily_test::json_string(lazily_test::json_member(scenario, "codec"));
  if (codec == "json")
    return decode_json(lazily_test::json_string(lazily_test::json_member(scenario, "wire_json")));
  if (codec == "msgpack")
    return decode_msgpack(hex_to_bytes(
        lazily_test::json_string(lazily_test::json_member(scenario, "wire_msgpack_hex"))));
  REQUIRE(false, "scenario names an unknown codec");
  return IpcMessage{};
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
  const std::string field = lazily_test::json_string(lazily_test::json_member(scenario, "field"));
  // Fail closed (#lzscenariobodyskip). `field` fell through to the Delta arm for
  // ANY spelling that was not `snapshot`, so a renamed or misspelled field
  // silently re-encoded and asserted the node_add frame while the scenario was
  // still booked as replayed.
  REQUIRE(field == "snapshot" || field == "node_add", "unknown nodekey field in fixture: " + field);
  if (field == "snapshot") {
    const JsonValue* body = owner.find("Snapshot");
    REQUIRE(body != nullptr, "re-encoded frame carries a Snapshot envelope");
    return body->find("nodes")->array.at(0);
  }
  const JsonValue* body = owner.find("Delta");
  REQUIRE(body != nullptr, "re-encoded frame carries a Delta envelope");
  return *body->find("ops")->array.at(0).find("NodeAdd");
}

static std::optional<std::string> decoded_key(const lazily_test::Json& scenario,
                                              const IpcMessage& message) {
  const std::string field = lazily_test::json_string(lazily_test::json_member(scenario, "field"));
  // Fail closed (#lzscenariobodyskip) — see `reencoded_node`.
  REQUIRE(field == "snapshot" || field == "node_add", "unknown nodekey field in fixture: " + field);
  if (field == "snapshot") {
    const auto* envelope = std::get_if<IpcMessageSnapshot>(&message);
    REQUIRE(envelope != nullptr, "fixture declares the Snapshot variant");
    const auto& key = envelope->value.nodes.front().key;
    return key ? std::optional<std::string>(std::string(key->path())) : std::nullopt;
  }
  const auto* envelope = std::get_if<IpcMessageDelta>(&message);
  REQUIRE(envelope != nullptr, "fixture declares the Delta variant");
  const auto* op = std::get_if<DeltaOpNodeAdd>(&envelope->value.ops.front());
  REQUIRE(op != nullptr, "fixture declares a NodeAdd op");
  return op->key ? std::optional<std::string>(std::string(op->key->path())) : std::nullopt;
}

static void test_nodekey_null_leniency_is_replayed() {
  const auto fx = lazily_test::parse_json(fixture_text());
  REQUIRE(lazily_test::json_u64(lazily_test::json_member(*fx, "protocol_version")) == 1,
          "protocol_version");
  REQUIRE(lazily_test::json_string(lazily_test::json_member(*fx, "kind")) == "NodeKeyNullLeniency",
          "kind");

  const auto& scenarios = lazily_test::json_array(lazily_test::json_member(*fx, "scenarios"));

  {
    lazily_test::AssertionKeys block(std::string(kFixtureId) + " assertions",
                                     lazily_test::json_member(*fx, "assertions"));
    block.assert_key("required_of_binding", std::string("MUST"));
    block.assert_key("scenario_count", static_cast<int64_t>(scenarios.size()));
    block.assert_key_with("codecs", [](const lazily_test::Json& want) {
      const auto& list = lazily_test::json_array(want);
      return list.size() == 2 && lazily_test::json_string(*list[0]) == "json" &&
             lazily_test::json_string(*list[1]) == "msgpack";
    });
    block.assert_key_with("fields", [](const lazily_test::Json& want) {
      const auto& list = lazily_test::json_array(want);
      return list.size() == 2 && lazily_test::json_string(*list[0]) == "snapshot" &&
             lazily_test::json_string(*list[1]) == "node_add";
    });
    block.assert_key_with("key_forms", [](const lazily_test::Json& want) {
      const auto& list = lazily_test::json_array(want);
      return list.size() == 3 && lazily_test::json_string(*list[0]) == "omitted" &&
             lazily_test::json_string(*list[1]) == "null" &&
             lazily_test::json_string(*list[2]) == "present";
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
    // paragraph says so by name. `wire_encoding` is discharged by the keys that
    // can only be satisfied by decoding the scenario's own bytes: `key_forms`
    // pins the three wire shapes and `decoded_key` reads each back.
    block.prose_key("clause", {"decoded_key", "reencoded_key_field_present"});
    block.prose_key("wire_encoding", {"key_forms", "decoded_key"});
    block.prose_key("reencode_obligation", {"reencoded_key_field_present"});
    block.prose_key("anti_vacuity", {"decoded_key", "key_forms", "scenario_count"});
    block.excuse_key("generator", "names the corpus script that emits this fixture, not a fact "
                                  "about the frames under test");
    block.finish();
  }

  // Anti-vacuity in both directions. A runner that never decodes reports
  // "absent" for everything and satisfies all eight omitted/null scenarios; the
  // `present` count is what only a real decode can produce.
  std::size_t replayed = 0;
  std::size_t keys_decoded = 0;

  for (const auto& sv : lazily_test::scenario_views(kFixtureId, scenarios)) {
    // Rung 4 books on the PAYLOAD handoff (#lzscenariobodyskip), so a body
    // that stops short of replaying stops being booked.
    const auto& scenario = sv.replay();
    const std::string id = sv.id();
    ++replayed;

    lazily_test::AssertionKeys expect(std::string(kFixtureId) + " scenarios[" + id + "].expect",
                                      lazily_test::json_member(scenario, "expect"));

    const IpcMessage message = decode_scenario(scenario);
    const auto key = decoded_key(scenario, message);
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

  REQUIRE(replayed == 12, "two fields x three key forms x two codecs");
  REQUIRE(keys_decoded == 4,
          "only the `present` scenarios carry a key; a runner reporting absent for everything "
          "satisfies the null cases trivially");

  // Checks every discharge above against what this run asserted. The ledger's
  // teardown fails a run that omits this call.
  lazily_test::verify_prose(kFixtureId);
}

int main() {
  test_nodekey_null_leniency_is_replayed();
  REQUIRE_FIXTURES_LOADED(1);
  return 0;
}
