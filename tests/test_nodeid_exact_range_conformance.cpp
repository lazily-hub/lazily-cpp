// NodeId exact-representation bound (`#lzspecdecoderbound`).
//
// protocol.md § NodeId / PeerId stated the 2^53 bound as a PRODUCER obligation
// and said nothing about what a decoder does when it receives a violation. That
// left the receiving half undefined, which is exactly where the bindings
// diverged. The clause is now normative: a decoder that cannot represent a
// received identifier exactly MUST reject the frame rather than round it.
//
// lazily-cpp's NodeId is an int64_t, so its exact range is [0, 2^63) — narrower
// than the u64 wire type. That is conforming: the clause does not require a
// binding to widen, only to refuse rather than substitute.
//
// The refusal was ALSO the bug this file locks down. `json_parse` refused an
// over-range integer by letting `std::stoll` throw `std::out_of_range`, which
// derives from `std::logic_error` and NOT from the `std::runtime_error` the
// parser raises for every other malformed frame — the error type callers guard
// with, and the one every other rejection test in this suite catches. An
// out-of-range identifier therefore escaped `catch (const std::runtime_error&)`
// and left the decode boundary entirely. That is why the refusal below is
// asserted through `std::runtime_error` specifically: catching `std::exception`
// would have passed against the broken parser too.
//
// The fixture carries its wire frames as raw text (json) and hex (msgpack) and
// its expected identifier as a decimal STRING, because the fixture is itself
// JSON: a runner on a double-backed runtime would otherwise have the fixture's
// own expectation rounded while loading the file, and would then agree with a
// rounding decoder.
//
// The tests are plain functions called from `main` rather than the usual
// self-registering TEST macro: that macro builds its names by token pasting,
// and the coined-id hook guarding this workspace reads the pasted form as an
// untracked tag.

#include <lazily/codec.hpp>
#include <lazily/json_codec.hpp>
#include <lazily/msgpack_codec.hpp>

#include "test_assertion_keys.hpp"
#include "test_json.hpp"
#include "test_spec_fixture.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

using namespace lazily;

static const char* const kFixtureArea = "codec";
static const char* const kFixtureName = "nodeid_exact_range.json";
static const char* const kFixtureId = "codec/nodeid_exact_range.json";

// Largest identifier lazily-cpp's int64_t NodeId represents exactly.
static constexpr uint64_t kMaxExactNodeId = static_cast<uint64_t>(INT64_MAX);

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

static const std::vector<uint8_t>& node_state_payload(const NodeState& state) {
  const auto* payload = std::get_if<NodeStatePayload>(&state);
  REQUIRE(payload != nullptr, "the fixture carries a Payload node state");
  return payload->bytes;
}

// Decode a scenario's wire frame with the codec it names.
//
// `ok` is false when the decoder REFUSED the frame — the conforming outcome for
// an identifier outside int64_t. Only `std::runtime_error` counts as a refusal:
// see the header note. A `std::out_of_range` escaping here is the regression.
struct DecodeResult {
  bool ok = false;
  IpcMessage message{};
};

static DecodeResult decode_scenario(const lazily_test::Json& scenario) {
  const std::string codec = lazily_test::json_string(lazily_test::json_member(scenario, "codec"));
  try {
    if (codec == "json") {
      // The raw TEXT through the codec's own entry point, so the parse that
      // would round is inside the code under test.
      return {true, decode_json(
                        lazily_test::json_string(lazily_test::json_member(scenario, "wire_json")))};
    }
    if (codec == "msgpack") {
      return {true, decode_msgpack(hex_to_bytes(lazily_test::json_string(
                        lazily_test::json_member(scenario, "wire_msgpack_hex"))))};
    }
  } catch (const std::runtime_error&) {
    return {false, IpcMessage{}};
  }
  REQUIRE(false, "scenario names an unknown codec");
  return {false, IpcMessage{}};
}

static void test_nodeid_exact_range_is_replayed() {
  const auto fx = lazily_test::parse_json(fixture_text());
  REQUIRE(lazily_test::json_u64(lazily_test::json_member(*fx, "protocol_version")) == 1,
          "protocol_version");
  REQUIRE(lazily_test::json_string(lazily_test::json_member(*fx, "kind")) == "NodeIdExactRange",
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
    block.excuse_keys({"clause", "wire_encoding", "outcomes", "anti_vacuity", "generator"},
                      "prose: it states WHY the fixture is shaped this way; the behaviour it "
                      "describes is asserted by the per-scenario decode below");
    block.finish();
  }

  // Anti-vacuity. `exact_or_reject` is satisfied by a runner that decodes
  // nothing and calls everything refused — and lazily-cpp really does refuse
  // part of this corpus, so a broken runner resembles a working one. The two
  // counters, pinned at the end, are what separate them.
  std::size_t accepted = 0;
  std::size_t refused = 0;

  for (std::size_t i = 0; i < scenarios.size(); ++i) {
    const auto& scenario = *scenarios[i];
    const std::string id = lazily_test::record_scenario_at(kFixtureId, scenario, i);

    lazily_test::AssertionKeys expect(std::string(kFixtureId) + " scenarios[" + id + "].expect",
                                      lazily_test::json_member(scenario, "expect"));

    const std::string decimal = lazily_test::json_string(
        lazily_test::json_member(lazily_test::json_member(scenario, "expect"), "node_id_decimal"));
    const uint64_t expected = std::stoull(decimal);
    const bool representable = expected <= kMaxExactNodeId;

    // `outcome` is the corpus-wide statement of what a decoder may do.
    // lazily-cpp reads it as a constraint on the FIXTURE: an `exact` scenario
    // it cannot represent would be a fixture bug, not a binding bug.
    expect.assert_key_with("outcome", [&](const lazily_test::Json& want) {
      const std::string outcome = lazily_test::json_string(want);
      if (outcome == "exact") return representable;
      return outcome == "exact_or_reject";
    });

    const DecodeResult result = decode_scenario(scenario);

    if (!result.ok) {
      REQUIRE(!representable,
              "lazily-cpp represents this identifier exactly, so the frame must decode");
      ++refused;
      expect.excuse_keys(
          {"node_id_decimal", "root_id_decimal", "epoch", "node_count", "type_tag", "payload"},
          "an int64_t NodeId cannot represent this identifier, so the frame is REFUSED — the "
          "conforming outcome, and the whole point of the scenario. These keys are asserted "
          "by the scenarios inside [0, 2^63).");
      expect.finish();
      continue;
    }

    REQUIRE(representable,
            "lazily-cpp cannot represent this identifier exactly, so decoding it means the "
            "identifier was rounded, truncated, or wrapped");
    ++accepted;

    const auto* envelope = std::get_if<IpcMessageSnapshot>(&result.message);
    REQUIRE(envelope != nullptr, "fixture declares the Snapshot variant");
    const Snapshot* snapshot = &envelope->value;
    REQUIRE(lazily_test::json_string(lazily_test::json_member(scenario, "variant")) == "Snapshot",
            "fixture `variant` disagrees with the decoded frame");

    expect.assert_key("epoch", snapshot->epoch);
    expect.assert_key("node_count", static_cast<int64_t>(snapshot->nodes.size()));
    REQUIRE(!snapshot->nodes.empty(), "snapshot carries a node");

    const NodeSnapshot& node = snapshot->nodes.front();
    // The discriminating assertion: the decimal rendering, so a decoder that
    // returned a neighbouring identifier is visible rather than approximately
    // right.
    expect.assert_key("node_id_decimal", std::to_string(node.node));
    expect.assert_key("type_tag", node.type_tag);
    expect.assert_key("payload", node_state_payload(node.state), json_bytes);
    REQUIRE(snapshot->roots.size() == 1, "snapshot carries one root");
    expect.assert_key("root_id_decimal", std::to_string(snapshot->roots.front()));
    expect.finish();
  }

  // Four scenarios (2^53-1 and 2^53+1, in both codecs) are inside int64_t; the
  // two at u64::MAX are not. Pinning both halves means a parser that stopped
  // refusing, and a decoder that stopped decoding, are each a failure here.
  REQUIRE(accepted == 4, "lazily-cpp decodes the four scenarios inside [0, 2^63)");
  REQUIRE(refused == 2, "lazily-cpp refuses both u64::MAX identifiers");
}

// The regression the audit turned up, isolated from the corpus replay above.
//
// `std::stoll`/`std::stod` throw `std::out_of_range`, a `std::logic_error`. The
// parser's own failures are `std::runtime_error`. Before this was routed through
// `fail()`, a caller doing the documented `catch (const std::runtime_error&)`
// around a decode did not catch an over-range number at all. Catching
// `std::exception` here would pass against the broken parser, so the narrower
// type IS the assertion.
static void test_over_range_number_is_an_ordinary_decode_error() {
  struct Case {
    const char* text;
    const char* why;
  };
  const Case cases[] = {
      {R"({"node":18446744073709551615})", "an integer wider than int64_t"},
      {R"({"node":-18446744073709551615})", "a negative integer wider than int64_t"},
      {R"({"value":1e999})", "a float outside the double range"},
  };

  for (const Case& c : cases) {
    bool threw_runtime_error = false;
    try {
      (void)json_parse(c.text);
    } catch (const std::runtime_error&) {
      threw_runtime_error = true;
    }
    REQUIRE(threw_runtime_error, c.why);
  }

  // And the boundary still decodes: the guard must refuse what it cannot
  // represent, not everything large.
  const JsonValue ok = json_parse(R"({"node":9223372036854775807})");
  const JsonValue* node = ok.find("node");
  REQUIRE(node != nullptr && node->as_int() == INT64_MAX, "INT64_MAX must still parse exactly");
}

int main() {
  test_nodeid_exact_range_is_replayed();
  test_over_range_number_is_an_ordinary_decode_error();
  REQUIRE_FIXTURES_LOADED(1);
  return 0;
}
