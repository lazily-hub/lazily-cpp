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
#include <set>
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

// `decoders_entered` is booked INSIDE the dispatch arms, on a decoder call that
// really happened — returning a message or refusing one, both of which are this
// fixture's conforming outcomes. `assertions.codecs` is compared against it, so
// it can no longer be satisfied by counting the scenarios' own `codec` labels,
// which is the fixture describing itself (`#lznullformblind`).
static DecodeResult decode_scenario(const lazily_test::Json& scenario,
                                    std::set<std::string>& decoders_entered) {
  const std::string codec = lazily_test::json_string(lazily_test::json_member(scenario, "codec"));
  REQUIRE(codec == "json" || codec == "msgpack", "scenario names an unknown codec: " + codec);
  try {
    if (codec == "json") {
      // The raw TEXT through the codec's own entry point, so the parse that
      // would round is inside the code under test.
      DecodeResult result{true, decode_json(lazily_test::json_string(
                                    lazily_test::json_member(scenario, "wire_json")))};
      decoders_entered.insert("json");
      return result;
    }
    DecodeResult result{true, decode_msgpack(hex_to_bytes(lazily_test::json_string(
                                  lazily_test::json_member(scenario, "wire_msgpack_hex"))))};
    decoders_entered.insert("msgpack");
    return result;
  } catch (const std::runtime_error&) {
    // Reachable only from inside an arm above, so the decoder DID run — it ran
    // and refused, which is the conforming outcome for an identifier outside
    // int64_t. The arm is booked on this path for that reason.
    decoders_entered.insert(codec);
    return {false, IpcMessage{}};
  }
}

static void test_nodeid_exact_range_is_replayed() {
  const auto fx = lazily_test::parse_json(fixture_text());
  REQUIRE(lazily_test::json_u64(lazily_test::json_member(*fx, "protocol_version")) == 1,
          "protocol_version");
  REQUIRE(lazily_test::json_string(lazily_test::json_member(*fx, "kind")) == "NodeIdExactRange",
          "kind");

  const auto& scenarios = lazily_test::json_array(lazily_test::json_member(*fx, "scenarios"));

  // Anti-vacuity. `exact_or_reject` is satisfied by a runner that decodes
  // nothing and calls everything refused — and lazily-cpp really does refuse
  // part of this corpus, so a broken runner resembles a working one. The two
  // counters, pinned at the end, are what separate them.
  std::size_t accepted = 0;
  std::size_t refused = 0;
  std::size_t replayed = 0;
  // Booked inside `decode_scenario`'s dispatch arms, never from the scenarios'
  // own `codec` labels (`#lznullformblind`).
  std::set<std::string> decoders_entered;
  // The verdict NAMES whose per-scenario comparison against the real decode
  // passed -- what `assertions.outcomes` is measured against.
  std::set<std::string> outcomes_validated;

  for (const auto& sv : lazily_test::scenario_views(kFixtureId, scenarios)) {
    // Rung 4 books on the PAYLOAD handoff (#lzscenariobodyskip), so a body
    // that stops short of replaying stops being booked.
    const auto& scenario = sv.replay();
    const std::string id = sv.id();
    ++replayed;

    lazily_test::AssertionKeys expect(std::string(kFixtureId) + " scenarios[" + id + "].expect",
                                      lazily_test::json_member(scenario, "expect"));

    const std::string decimal = lazily_test::json_string(
        lazily_test::json_member(lazily_test::json_member(scenario, "expect"), "node_id_decimal"));
    const uint64_t expected = std::stoull(decimal);
    const bool representable = expected <= kMaxExactNodeId;

    const DecodeResult result = decode_scenario(scenario, decoders_entered);

    // `outcome` is the corpus-wide statement of what a decoder may do, and it is
    // asserted against WHAT THIS DECODER DID (`#lznullformblind`).
    //
    // It used to be evaluated here as a constraint on the FIXTURE — `exact`
    // meant "the identifier fits in int64_t" — with both sides derived from the
    // scenario's own `node_id_decimal` and the comparison placed BEFORE the
    // decode ran. Nothing about the run could reach it: a decoder that refused
    // every frame, or accepted every frame, satisfied it identically. The
    // comparison is now after the decode and against `result.ok`.
    expect.assert_key_with("outcome", [&](const lazily_test::Json& want) {
      const std::string outcome = lazily_test::json_string(want);
      // `exact` admits ONE verdict: the frame decoded.
      if (outcome == "exact") {
        if (!result.ok) return false;
        outcomes_validated.insert(outcome);
        return true;
      }
      // `exact_or_reject` admits either, but not either arbitrarily — this
      // binding must accept exactly the identifiers it can represent.
      if (outcome != "exact_or_reject") return false;
      if (result.ok != representable) return false;
      outcomes_validated.insert(outcome);
      return true;
    });

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

  // The assertion block is evaluated AFTER the replay. `scenario_count` against
  // `scenarios.size()` would be the fixture compared to itself — green over a
  // runner that decodes nothing, the exact vacuity `assertions.anti_vacuity`
  // exists to name — so it is compared against the scenarios this run reached,
  // and `codecs` against the codecs it really drove.
  {
    lazily_test::AssertionKeys block(std::string(kFixtureId) + " assertions",
                                     lazily_test::json_member(*fx, "assertions"));
    block.assert_key("required_of_binding", std::string("MUST"));
    block.assert_key("scenario_count", static_cast<int64_t>(replayed));
    // Both directions against the DECODERS THIS RUN ENTERED. The previous
    // `codecs_seen.size() == 2` counted the scenarios' own `codec` labels, so it
    // was satisfied by a fixture carrying two of them and said nothing about
    // whether either decoder ran (`#lznullformblind`).
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
    // The three paragraphs the corpus declares in `assertions.prose`, each
    // DISCHARGED by naming the executable keys this fixture's run asserts
    // (`#lzprosekeyconvention`). The naming is checked: `verify_prose` below
    // refuses a name no block of this replay actually asserted.
    //
    // The clause is "represent it exactly or refuse": `outcome` is the
    // accept/refuse verdict, checked against the identifier's representability
    // rather than taken on the fixture's word, and `node_id_decimal` is the
    // decimal rendering that makes a neighbouring identifier visible.
    block.prose_key("clause", {"outcome", "node_id_decimal"});
    // PROXY. `wire_encoding` is a claim about how the CORPUS carries its bytes —
    // raw text and hex, and an expectation that is a decimal STRING — which no
    // assertion a run makes can observe directly. `node_id_decimal` is the
    // closest executable stand-in: it is the string expectation the paragraph
    // exists to require, and a runner comparing JSON numbers would have had the
    // fixture's own parser round 2^53+1 before any comparison happened.
    block.prose_key("wire_encoding", {"node_id_decimal", "outcome"});
    block.prose_key("anti_vacuity", {"node_id_decimal", "outcome", "scenario_count"});
    // `outcomes` is the corpus's glossary of verdict names. It used to be
    // EXCUSED as "not a fact about the frames under test" -- but it is: the names
    // it defines are exactly the verdicts this run reached, and the excuse let
    // the corpus retire one silently. Asserted in both directions against the
    // outcomes whose per-scenario comparison against `result.ok` really passed
    // (`#lznullformblind`).
    block.assert_key_with("outcomes", [&](const lazily_test::Json& want) {
      REQUIRE(want.is_object(), "assertions.outcomes is a glossary object");
      std::set<std::string> declared;
      for (const auto& kv : want.object)
        declared.insert(kv.first);
      return declared == outcomes_validated;
    });
    block.excuse_key("generator", "names the corpus script that emits this fixture, not a fact "
                                  "about the frames under test");
    block.finish();
  }

  // Checks every discharge above against what this run asserted. The ledger's
  // teardown fails a run that omits this call.
  lazily_test::verify_prose(kFixtureId);

  // Runner-side floors sit BELOW the fixture's own block: a floor above it
  // aborts on exactly the input `scenario_count` exists to catch
  // (`#lznullformblind`). Four scenarios (2^53-1 and 2^53+1, in both codecs) are
  // inside int64_t; the two at u64::MAX are not. Pinning both halves means a
  // parser that stopped refusing, and a decoder that stopped decoding, are each
  // a failure here.
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
