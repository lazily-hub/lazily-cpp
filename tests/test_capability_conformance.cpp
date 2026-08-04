// Canonical CapabilityHandshake negotiation replay.
//
// The fixture makes the endpoint advertisements executable: a positive common
// receive ceiling is retained as the smaller value, fragmentation is mutual
// support, and session_id is the shared graph identity while peer_id may differ.

#include <lazily/ipc.hpp>

#include "test_assertion_keys.hpp"
#include "test_json.hpp"
#include "test_spec_fixture.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

using namespace lazily;

static const char* const kFixtureArea = "codec";
static const char* const kFixtureName = "capability_handshake.json";
static const char* const kFixtureId = "codec/capability_handshake.json";

static CapabilityHandshake handshake_from_fixture(const lazily_test::Json& value) {
  CapabilityHandshake handshake;
  handshake.protocol_id = lazily_test::json_string(lazily_test::json_member(value, "protocol_id"));
  handshake.protocol_major_version = static_cast<int>(
      lazily_test::json_u64(lazily_test::json_member(value, "protocol_major_version")));
  const std::string codec = lazily_test::json_string(lazily_test::json_member(value, "codec"));
  const auto parsed_codec = codec_from_token(codec);
  REQUIRE(parsed_codec.has_value(),
          "fixture names a codec this binding does not support: " + codec);
  handshake.codec = *parsed_codec;
  handshake.max_frame_size = static_cast<int64_t>(
      lazily_test::json_u64(lazily_test::json_member(value, "max_frame_size")));
  handshake.fragmentation_supported =
      lazily_test::json_bool(lazily_test::json_member(value, "fragmentation_supported"));
  handshake.ordered_reliable =
      lazily_test::json_bool(lazily_test::json_member(value, "ordered_reliable"));
  handshake.peer_id =
      static_cast<PeerId>(lazily_test::json_u64(lazily_test::json_member(value, "peer_id")));
  handshake.session_id = lazily_test::json_string(lazily_test::json_member(value, "session_id"));
  for (const auto& feature : lazily_test::json_array(lazily_test::json_member(value, "features")))
    handshake.features.push_back(lazily_test::json_string(*feature));
  return handshake;
}

int main() {
  const std::string text = lazily_test::spec_fixture_text(kFixtureArea, kFixtureName);
  const lazily_test::JsonPtr fixture = lazily_test::parse_json(text);
  REQUIRE(lazily_test::json_u64(lazily_test::json_member(*fixture, "protocol_version")) == 1,
          "capability fixture protocol_version must be 1");
  REQUIRE(lazily_test::json_string(lazily_test::json_member(*fixture, "kind")) ==
              "CapabilityHandshake",
          "capability fixture kind mismatch");

  const auto& scenarios = lazily_test::json_array(lazily_test::json_member(*fixture, "scenarios"));
  for (std::size_t index = 0; index < scenarios.size(); ++index) {
    const lazily_test::Json& scenario = *scenarios[index];
    const std::string id = lazily_test::record_scenario_at(kFixtureId, scenario, index);
    const CapabilityHandshake local =
        handshake_from_fixture(lazily_test::json_member(scenario, "local"));
    const CapabilityHandshake remote =
        handshake_from_fixture(lazily_test::json_member(scenario, "remote"));
    const CapabilityNegotiation negotiation = local.negotiate(remote);

    lazily_test::AssertionKeys expected(std::string(kFixtureId) + " scenarios[" + id + "].expected",
                                        lazily_test::json_member(scenario, "expected"));
    expected.assert_key("compatible", negotiation.ok());
    if (negotiation.ok()) {
      REQUIRE(negotiation.capabilities.has_value(),
              id + ": successful negotiation retained no capabilities");
      expected.assert_key("negotiated_max_frame_size", negotiation.capabilities->max_frame_size);
      expected.assert_key("negotiated_fragmentation_supported",
                          negotiation.capabilities->fragmentation_supported);
    } else {
      REQUIRE(!negotiation.capabilities.has_value(),
              id + ": failed negotiation retained capabilities");
      expected.assert_key("field", negotiation.check.field);
    }
  }

  REQUIRE_FIXTURES_LOADED(1);
  return 0;
}
