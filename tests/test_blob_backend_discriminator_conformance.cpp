// Blob-backend discriminator strictness on decode (`#lzblobbackendstrict`).
//
// `ShmBlobRef.backend` is optional and its OPTIONALITY is the forward-compat
// channel — it carries every descriptor minted before the field existed, so an
// omitted `backend` MUST decode as `shm`. A PRESENT token outside
// {shm, arrow, in_process} is a different fact and gets the opposite answer: the
// frame is REFUSED and the error names the token.
//
// lazily-cpp normalized an unknown token to `Shm` and documented the
// normalization as forward compatibility, on the argument that the
// generation/epoch/checksum verification in `ShmBackend::read_view` /
// `BlobArena::read_view` would turn a misrouted descriptor into an empty view
// rather than into wrong bytes. That argument inverts `resolve_wrong_backend`
// (lazily-spec/docs/zero-copy-transport.md): a descriptor of one kind never
// resolves against a different backend's table BECAUSE receivers route by kind,
// so reading an unknown kind as `Shm` IS the misroute the theorem excludes, and
// the verification only probabilistically repairs it. This runner is what holds
// the corrected behaviour.
//
// The wire is carried as RAW TEXT / HEX in the fixture and decoded from that
// form. `schemas/defs.json` closes `backend` to an enum, so the reject frames
// are schema-INVALID by design and cannot be carried as parsed objects; a runner
// that re-serialized a parsed scenario body would be testing its own writer.
//
// Both codecs are replayed. In this binding they are NOT independent
// implementations — `decode_msgpack` bridges MessagePack into the same JsonValue
// DOM `decode_json` produces and then calls the one `json_to_ipc_message`, so
// the ShmBlobRef reader is shared. That is stated rather than assumed: the
// codec.hpp PRIVATE framing has its own hand-written ShmBlobRef reader (the
// divergence that let an invalid NodeKey through msgpack while json refused it),
// and this runner drives the SPEC wires, where the sharing is deliberate. The
// msgpack scenarios still prove the MessagePack->DOM bridge preserves the field,
// which is a distinct failure from the DOM reader's verdict.
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
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

using namespace lazily;

static const char* const kFixtureArea = "codec";
static const char* const kFixtureName = "blob_backend_discriminator.json";
static const char* const kFixtureId = "codec/blob_backend_discriminator.json";

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

static std::string scenario_codec(const lazily_test::Json& scenario) {
  const std::string codec = lazily_test::json_string(lazily_test::json_member(scenario, "codec"));
  // Fail closed (#lzscenariobodyskip): a codec token this runner does not speak
  // must stop the run, not fall through to one it does.
  REQUIRE(codec == "json" || codec == "msgpack", "scenario names an unknown codec: " + codec);
  return codec;
}

// Decode STRICTLY from the raw wire the fixture carries. Nothing is re-encoded
// on the way in.
static IpcMessage decode_scenario(const lazily_test::Json& scenario) {
  if (scenario_codec(scenario) == "json")
    return decode_json(lazily_test::json_string(lazily_test::json_member(scenario, "wire_json")));
  return decode_msgpack(hex_to_bytes(
      lazily_test::json_string(lazily_test::json_member(scenario, "wire_msgpack_hex"))));
}

// The decoded `SharedBlob` descriptor carried by the fixture's single
// `SlotValue` op. Every intermediate step is checked rather than assumed, so a
// frame that decoded into some other shape fails here instead of silently
// providing a default-constructed ShmBlobRef whose `backend` is `Shm` — which
// would satisfy the omitted/shm scenarios without a decode.
static const ShmBlobRef& decoded_blob(const lazily_test::Json& scenario, const IpcMessage& message,
                                      NodeId& node_out) {
  const std::string variant =
      lazily_test::json_string(lazily_test::json_member(scenario, "variant"));
  REQUIRE(variant == "Delta", "fixture declares the Delta variant, got: " + variant);
  const auto* envelope = std::get_if<IpcMessageDelta>(&message);
  REQUIRE(envelope != nullptr, "decoded frame is a Delta envelope");
  REQUIRE(envelope->value.ops.size() == 1, "the scenario frame carries exactly one op");
  const auto* op = std::get_if<DeltaOpSlotValue>(&envelope->value.ops.front());
  REQUIRE(op != nullptr, "the scenario frame's op is a SlotValue");
  const auto* blob = std::get_if<IpcValueSharedBlob>(&op->payload);
  REQUIRE(blob != nullptr, "the SlotValue payload is a SharedBlob descriptor");
  node_out = op->node;
  return blob->blob;
}

// Re-encode under the scenario's OWN codec and read the descriptor's field set
// back SCHEMA-LESSLY. The typed `BlobBackendKind` cannot tell "field omitted"
// from "field written as shm", which is the encoder half of the clause.
static const JsonValue& reencoded_blob(const lazily_test::Json& scenario, const IpcMessage& message,
                                       JsonValue& owner) {
  if (scenario_codec(scenario) == "msgpack") {
    // Through the msgpack ENCODER specifically. Asserting the json output for
    // both codecs would miss an encoder that writes the field on one wire only
    // — the `#lzmsgpackparity` class of defect.
    owner = msgpack_to_json(encode_msgpack(message));
  } else {
    owner = json_parse(encode_json(message));
  }
  const JsonValue* body = owner.find("Delta");
  REQUIRE(body != nullptr, "re-encoded frame carries a Delta envelope");
  const JsonValue* ops = body->find("ops");
  REQUIRE(ops != nullptr && !ops->array.empty(), "re-encoded frame carries its ops");
  const JsonValue* slot = ops->array.at(0).find("SlotValue");
  REQUIRE(slot != nullptr, "re-encoded op is externally tagged SlotValue");
  const JsonValue* payload = slot->find("payload");
  REQUIRE(payload != nullptr, "re-encoded SlotValue carries a payload");
  const JsonValue* blob = payload->find("SharedBlob");
  REQUIRE(blob != nullptr, "re-encoded payload is externally tagged SharedBlob");
  return *blob;
}

static void test_blob_backend_discriminator_is_replayed() {
  const auto fx = lazily_test::parse_json(fixture_text());
  REQUIRE(lazily_test::json_u64(lazily_test::json_member(*fx, "protocol_version")) == 1,
          "protocol_version");
  REQUIRE(lazily_test::json_string(lazily_test::json_member(*fx, "kind")) ==
              "BlobBackendDiscriminator",
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
    // The enum this binding closes `backend` to. If the corpus grows a fourth
    // backend, this fails here rather than in a reject scenario, which is the
    // signal that a spec change (not a corrupt producer) arrived.
    block.assert_key_with("backends", [](const lazily_test::Json& want) {
      const auto& list = lazily_test::json_array(want);
      if (list.size() != 3) return false;
      const char* const expected[] = {"shm", "arrow", "in_process"};
      for (std::size_t i = 0; i < list.size(); ++i) {
        if (lazily_test::json_string(*list[i]) != expected[i]) return false;
        // Every named backend is a token this decoder actually speaks.
        if (std::string(blob_backend_kind_str(blob_backend_kind_from_str(
                lazily_test::json_string(*list[i])))) != lazily_test::json_string(*list[i]))
          return false;
      }
      return true;
    });
    block.assert_key_with("outcomes", [](const lazily_test::Json& want) {
      const auto& list = lazily_test::json_array(want);
      return list.size() == 2 && lazily_test::json_string(*list[0]) == "accept" &&
             lazily_test::json_string(*list[1]) == "reject";
    });
    block.excuse_keys(
        {"clause", "wire_encoding", "reject_obligation", "anti_vacuity", "theorem", "generator"},
        "prose: it states WHY the fixture is shaped this way; the behaviour it "
        "describes is asserted by the per-scenario decode, refusal and re-encode "
        "below");
    block.finish();
  }

  // Anti-vacuity ledger, three counters for the three ways to pass without
  // implementing the clause (the fixture's own `anti_vacuity` note).
  std::size_t replayed = 0;
  std::size_t accepted = 0;
  std::size_t rejected = 0;
  std::size_t non_shm_decoded = 0;       // only `arrow` can produce this
  std::size_t backend_field_written = 0; // only `arrow` may produce this

  for (const auto& sv : lazily_test::scenario_views(kFixtureId, scenarios)) {
    // Rung 4 books on the PAYLOAD handoff (#lzscenariobodyskip), so a body that
    // stops short of replaying stops being booked.
    const auto& scenario = sv.replay();
    const std::string id = sv.id();
    ++replayed;

    const std::string outcome =
        lazily_test::json_string(lazily_test::json_member(scenario, "outcome"));
    REQUIRE(outcome == "accept" || outcome == "reject",
            "scenario names an unknown outcome: " + outcome);

    lazily_test::AssertionKeys expect(std::string(kFixtureId) + " scenarios[" + id + "].expect",
                                      lazily_test::json_member(scenario, "expect"));

    if (outcome == "reject") {
      ++rejected;
      std::string message;
      bool threw_runtime_error = false;
      bool threw_logic_error = false;
      try {
        (void)decode_scenario(scenario);
      } catch (const std::logic_error& e) {
        // Caught FIRST and reported separately. `std::invalid_argument` and
        // `std::out_of_range` are logic_errors, so they escape the
        // `catch (const std::runtime_error&)` every decode caller uses — the
        // regression `#lzspecdecoderbound` pinned for NodeId. A refusal of that
        // family is not a refusal callers see.
        threw_logic_error = true;
        message = e.what();
      } catch (const std::runtime_error& e) {
        threw_runtime_error = true;
        message = e.what();
      }
      REQUIRE(!threw_logic_error,
              id + ": refusal escaped as a std::logic_error, outside the std::runtime_error "
                   "callers guard a decode with");
      expect.assert_key("rejected", threw_runtime_error);
      // The refusal must be FOR THE STATED REASON. A decoder that refused
      // because it mis-parsed `checksum` satisfies a bare is-error assertion
      // while implementing none of the clause.
      expect.assert_key_with("error_names_token", [&](const lazily_test::Json& want) {
        const std::string token = lazily_test::json_string(want);
        return threw_runtime_error && message.find(token) != std::string::npos;
      });
      expect.finish();
      continue;
    }

    ++accepted;
    // An accept scenario that throws is a FAILURE OF THIS SCENARIO, not an
    // unhandled exception out of `main`. Without this the process terminates
    // with a bare `what()` and the id never reaches the log, so a run that
    // reddened could not be attributed to the frame that reddened it.
    IpcMessage message;
    try {
      message = decode_scenario(scenario);
    } catch (const std::exception& e) {
      REQUIRE(false, id + ": accept scenario was refused: " + e.what());
    }
    NodeId node = 0;
    const ShmBlobRef& blob = decoded_blob(scenario, message, node);
    if (blob.backend != BlobBackendKind::Shm) ++non_shm_decoded;

    expect.assert_key_with("decoded_backend", [&](const lazily_test::Json& want) {
      return lazily_test::json_string(want) == blob_backend_kind_str(blob.backend);
    });
    expect.assert_key("node", static_cast<int64_t>(node));
    expect.assert_key("offset", blob.offset);
    expect.assert_key("len", blob.len);
    expect.assert_key("generation", blob.generation);
    expect.assert_key("epoch", blob.epoch);
    expect.assert_key("checksum", blob.checksum);

    // The encoder half: `backend` is OMITTED when the value is `Shm`, so a
    // descriptor that predates the field round-trips byte-identically, and it is
    // WRITTEN when it is not.
    JsonValue owner;
    const JsonValue* encoded_ptr = nullptr;
    try {
      encoded_ptr = &reencoded_blob(scenario, message, owner);
    } catch (const std::exception& e) {
      REQUIRE(false, id + ": re-encode under its own codec failed: " + e.what());
    }
    REQUIRE(encoded_ptr != nullptr, id + ": re-encode produced no descriptor");
    const JsonValue& encoded = *encoded_ptr;
    const JsonValue* field = encoded.find("backend");
    const bool present = field != nullptr && !field->is_null();
    if (present) ++backend_field_written;
    expect.assert_key("reencoded_backend_field_present", present);
    expect.finish();
  }

  REQUIRE(replayed == 8, "four backend forms x two codecs");
  REQUIRE(accepted == 6, "omitted, explicit shm and arrow, on both codecs");
  REQUIRE(rejected == 2, "the unknown token, on both codecs");
  // Control (2) from the fixture: a decoder that hardcodes `Shm` and ignores the
  // field passes every omitted/shm scenario. Only `arrow` can move this counter,
  // and the fixture carries exactly one arrow scenario per codec.
  REQUIRE(non_shm_decoded == 2,
          "only the `arrow` scenarios decode to a non-shm backend; a decoder that hardcodes "
          "`Shm` satisfies the omitted and explicit-shm cases trivially");
  // Control (3): an encoder that echoes the received token back out writes the
  // field on the explicit-shm scenarios too.
  REQUIRE(backend_field_written == 2,
          "only the `arrow` scenarios re-encode a `backend` field; an encoder that echoes what "
          "it received writes it on the explicit-shm scenarios as well");
}

int main() {
  test_blob_backend_discriminator_is_replayed();
  REQUIRE_FIXTURES_LOADED(1);
  return 0;
}
