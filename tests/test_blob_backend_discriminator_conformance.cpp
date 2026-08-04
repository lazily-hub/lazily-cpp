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
// FIXTURE v2 (14 scenarios, seven wire shapes x two codecs) adds four facts v1
// declared or implied without carrying, and this runner holds each one:
//
//   * `in_process` — the THIRD declared backend. A binding that knows only
//     {shm, arrow} refuses it, NAMING the token, and passes every v1 scenario
//     while implementing a smaller enum than the clause declares. The guard is
//     not a scenario count: it is the SET DIFFERENCE asserted under
//     `assertions.backends` below — every backend that list declares must appear
//     as the `decoded_backend` of some accept scenario. It sits on `backends`
//     rather than on the `backend_form_vocabulary` paragraph that states it in
//     English, because a check hung off a paragraph pins the paragraph
//     (`#lzprosekeyconvention`).
//   * an explicit `null` — the ABSENT form (`#lzkeynullstrict`), not a
//     present-unknown one. It decodes as `shm` and does NOT survive a round
//     trip. Those frames are deliberately schema-INVALID: the enum in
//     `schemas/defs.json` binds the ENCODER, and the decoder's leniency is the
//     separate fact under test.
//   * a NON-STRING `backend` — refused, and refused through the family callers
//     guard a decode with. In C++ that means `std::runtime_error` and
//     specifically NOT `std::invalid_argument`, which derives from
//     `std::logic_error` and so escapes `catch (const std::runtime_error&)`.
//     That is the same hierarchy trap `#lzspecdecoderbound` pinned when a
//     `std::out_of_range` from `std::stoll` walked past every decode guard.
//     `expect.rejection_is_decode_error` is the assertion that pins it, and it
//     is a DIFFERENT fact from `expect.rejected`: an `invalid_argument` still
//     refuses the frame, it just refuses it past the handler.
//   * `expect.epoch` is GONE, split into `frame_epoch` (9, the Delta envelope's)
//     and `blob_epoch` (5, the descriptor's). v1 carried 9 in both, so a runner
//     reading either satisfied the one key. This runner asserts each against its
//     own source and REFUSES a fixture that reintroduces the merged key.
//
// The wire is carried as RAW TEXT / HEX in the fixture and decoded from that
// form. `schemas/defs.json` closes `backend` to an enum, so the reject frames
// are schema-INVALID by design and cannot be carried as parsed objects; a runner
// that re-serialized a parsed scenario body would be testing its own writer.
//
// Both codecs are replayed. In this binding they are NOT independent
// implementations — `decode_msgpack` bridges MessagePack into the same JsonValue
// DOM `decode_json` produces and then calls the one `json_to_ipc_message`, so
// the ShmBlobRef reader is shared and the msgpack half of a scenario pair yields
// ONE discriminator verdict, not a second independent one. That is stated rather
// than assumed, and it is what `assertions.anti_vacuity` asks a binding with a
// shared decode path to record: a fully green run here must not be read as two
// implementations agreeing. It is still not vacuous — the codec.hpp PRIVATE
// framing has its own hand-written ShmBlobRef reader (the divergence that let an
// invalid NodeKey through msgpack while json refused it), the msgpack scenarios
// prove the MessagePack->DOM bridge preserves (and drops) the field, and the
// re-encode half goes through each codec's OWN writer.
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
#include <iostream>
#include <set>
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

// The scenario's wire, in the codec-neutral DOM both decoders feed. Used to read
// the frame back SCHEMA-LESSLY, which the typed ShmBlobRef cannot do: it cannot
// tell "field omitted" from "field written as shm", nor "explicit null" from
// either.
static JsonValue wire_dom(const lazily_test::Json& scenario) {
  if (scenario_codec(scenario) == "json")
    return json_parse(lazily_test::json_string(lazily_test::json_member(scenario, "wire_json")));
  return msgpack_to_json(hex_to_bytes(
      lazily_test::json_string(lazily_test::json_member(scenario, "wire_msgpack_hex"))));
}

// Walk to the SharedBlob descriptor of a frame held schema-lessly. Shared by the
// wire reader and the re-encode reader so both ask the same question of the same
// shape.
static const JsonValue& dom_blob(const JsonValue& frame, const std::string& what) {
  const JsonValue* body = frame.find("Delta");
  REQUIRE(body != nullptr, what + " carries a Delta envelope");
  const JsonValue* ops = body->find("ops");
  REQUIRE(ops != nullptr && !ops->array.empty(), what + " carries its ops");
  const JsonValue* slot = ops->array.at(0).find("SlotValue");
  REQUIRE(slot != nullptr, what + " op is externally tagged SlotValue");
  const JsonValue* payload = slot->find("payload");
  REQUIRE(payload != nullptr, what + " SlotValue carries a payload");
  const JsonValue* blob = payload->find("SharedBlob");
  REQUIRE(blob != nullptr, what + " payload is externally tagged SharedBlob");
  return *blob;
}

// The shape `backend` arrives in, read from the SCENARIO'S OWN BYTES rather than
// from its `backend_form` label. That is what makes `backend_form` (and, through
// it, `rejection_kind`) checkable at all: the fixture's label and the fixture's
// wire are two claims, and this runner is only entitled to trust them once it has
// seen them agree. It also proves the MessagePack->DOM bridge preserves the
// distinctions the clause turns on — an absent entry, a nil, and an integer are
// three different things and a bridge that flattened any pair of them would pass
// every typed assertion below.
static std::string wire_backend_form(const lazily_test::Json& scenario) {
  const JsonValue frame = wire_dom(scenario);
  const JsonValue* field = dom_blob(frame, "scenario wire").find("backend");
  if (field == nullptr) return "omitted";
  if (field->is_null()) return "null";
  if (!field->is_string()) return "non_string";
  return field->as_string();
}

// Decode STRICTLY from the raw wire the fixture carries. Nothing is re-encoded
// on the way in.
//
// `decoders_entered` is booked INSIDE the dispatch arm, on a decoder call that
// really happened — returning a frame or refusing one, both of which this
// fixture's scenarios declare as outcomes. `assertions.codecs` is compared
// against it, so it can no longer be satisfied by counting the scenarios' own
// `codec` labels, which is the fixture describing itself (`#lznullformblind`).
static IpcMessage decode_scenario(const lazily_test::Json& scenario,
                                  lazily_test::AssertionKeys& expect,
                                  std::set<std::string>& decoders_entered) {
  const std::string codec = scenario_codec(scenario);
  try {
    IpcMessage message;
    if (codec == "json") {
      const std::string raw =
          lazily_test::json_string(lazily_test::json_member(scenario, "wire_json"));
      expect.assert_key("wire_input_fnv1a64", lazily_test::fnv1a64_hex(raw));
      message = decode_json(raw);
    } else {
      const std::vector<uint8_t> raw = hex_to_bytes(
          lazily_test::json_string(lazily_test::json_member(scenario, "wire_msgpack_hex")));
      expect.assert_key("wire_input_fnv1a64", lazily_test::fnv1a64_hex(raw));
      message = decode_msgpack(raw);
    }
    decoders_entered.insert(codec);
    return message;
  } catch (...) {
    // A refusal is the decoder RUNNING and saying no — the conforming outcome
    // for the two reject forms — so the arm is booked here too, and the
    // exception continues to the caller that asserts on it.
    decoders_entered.insert(codec);
    throw;
  }
}

// What an accept scenario yields, with the two epochs kept APART. `frame_epoch`
// is the Delta envelope's (it orders deltas) and `blob_epoch` is the ShmBlobRef
// descriptor's (the arena incarnation the blob was written into). v1 carried 9
// in both and one `expect.epoch`, so reading the wrong one was invisible.
struct DecodedScenario {
  const ShmBlobRef* blob = nullptr;
  NodeId node = 0;
  Epoch frame_epoch = 0;
};

// Every intermediate step is checked rather than assumed, so a frame that decoded
// into some other shape fails here instead of silently providing a
// default-constructed ShmBlobRef whose `backend` is `Shm` — which would satisfy
// the omitted/null/shm scenarios without a decode.
static DecodedScenario decoded_scenario(const lazily_test::Json& scenario,
                                        const IpcMessage& message) {
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
  DecodedScenario out;
  out.blob = &blob->blob;
  out.node = op->node;
  out.frame_epoch = envelope->value.epoch;
  return out;
}

// Re-encode under the scenario's OWN codec and read the descriptor's field set
// back schema-lessly.
static JsonValue reencoded_frame(const lazily_test::Json& scenario, const IpcMessage& message) {
  if (scenario_codec(scenario) == "msgpack") {
    // Through the msgpack ENCODER specifically. Asserting the json output for
    // both codecs would miss an encoder that writes the field on one wire only
    // — the `#lzmsgpackparity` class of defect.
    return msgpack_to_json(encode_msgpack(message));
  }
  return json_parse(encode_json(message));
}

static bool contains(const std::set<std::string>& set, const std::string& value) {
  return set.count(value) != 0;
}

static void test_blob_backend_discriminator_is_replayed() {
  const auto fx = lazily_test::parse_json(fixture_text());
  REQUIRE(lazily_test::json_u64(lazily_test::json_member(*fx, "protocol_version")) == 1,
          "protocol_version");
  REQUIRE(lazily_test::json_string(lazily_test::json_member(*fx, "kind")) ==
              "BlobBackendDiscriminator",
          "kind");

  const auto& scenarios = lazily_test::json_array(lazily_test::json_member(*fx, "scenarios"));

  // Anti-vacuity ledger. The counters are the three controls the fixture names;
  // the SETS are what the vocabulary-completeness assertion needs, and no count
  // substitutes for them — v1 shipped a complete-looking eight scenarios while
  // one declared backend never appeared on any wire.
  std::size_t replayed = 0;
  std::size_t accepted = 0;
  std::size_t rejected = 0;
  std::size_t non_shm_decoded = 0;       // only `arrow` and `in_process` can move this
  std::size_t backend_field_written = 0; // only `arrow` and `in_process` may move this
  std::size_t null_form_replayed = 0;
  std::size_t non_string_form_replayed = 0;
  std::size_t epochs_differed = 0;
  // Booked inside `decode_scenario`'s dispatch arm, never from the scenarios'
  // own `codec` labels (`#lznullformblind`).
  std::set<std::string> decoders_entered;
  std::set<std::string> forms_seen;
  std::set<std::string> rejection_kinds_seen;
  std::set<std::string> decoded_backends; // the vocabulary this run actually PROVED
  // The outcome each frame REACHED, not the one its scenario declared. A
  // scenario labelled `reject` whose frame decoded anyway records nothing here,
  // so the vocabulary cannot be satisfied by the labels alone
  // (`#lznullformblind`).
  std::set<std::string> outcomes_reached;

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

    // The label and the bytes must agree before either is trusted.
    const std::string form =
        lazily_test::json_string(lazily_test::json_member(scenario, "backend_form"));
    const std::string on_wire = wire_backend_form(scenario);
    REQUIRE(form == on_wire, id + ": scenario declares backend_form '" + form +
                                 "' but its own wire carries '" + on_wire +
                                 "' — the label and the bytes disagree");
    // The form read off the RAW BYTES, not the scenario's `backend_form` label.
    // The two are pinned equal one line above, so this changes no verdict today
    // — it changes which side the vocabulary assertion is rooted in
    // (`#lznullformblind`).
    forms_seen.insert(on_wire);

    lazily_test::AssertionKeys expect(std::string(kFixtureId) + " scenarios[" + id + "].expect",
                                      lazily_test::json_member(scenario, "expect"));

    // v2 REMOVED `expect.epoch` rather than redefining it, precisely so a runner
    // still reading it fails loudly instead of silently reading the other epoch.
    // This is that failure, made explicit rather than left to the
    // consumed-but-not-asserted path.
    REQUIRE(expect.find("epoch") == nullptr,
            id + ": `expect.epoch` is ambiguous between the frame and the descriptor and was "
                 "removed in fixture v2 — assert `frame_epoch` and `blob_epoch` separately");

    if (outcome == "reject") {
      ++rejected;
      std::string message;
      bool threw_any = false;
      bool threw_runtime_error = false;
      bool threw_logic_error = false;
      try {
        (void)decode_scenario(scenario, expect, decoders_entered);
      } catch (const std::logic_error& e) {
        // Caught FIRST and reported separately. `std::invalid_argument` and
        // `std::out_of_range` are logic_errors, so they escape the
        // `catch (const std::runtime_error&)` every decode caller uses — the
        // regression `#lzspecdecoderbound` pinned for NodeId. A refusal of that
        // family is not a refusal callers see.
        threw_any = true;
        threw_logic_error = true;
        message = e.what();
      } catch (const std::runtime_error& e) {
        threw_any = true;
        threw_runtime_error = true;
        message = e.what();
      }

      // Two DIFFERENT facts. `rejected` asks whether the frame was refused at
      // all; `rejection_is_decode_error` asks whether the refusal arrived
      // through the family callers guard a decode with. A `std::invalid_argument`
      // satisfies the first and fails the second, which is the whole point of
      // the second key.
      expect.assert_key("rejected", threw_any);
      if (threw_any) outcomes_reached.insert("reject");
      expect.assert_key_with("rejection_is_decode_error", [&](const lazily_test::Json& want) {
        if (!lazily_test::json_bool(want)) return !threw_runtime_error;
        REQUIRE(!threw_logic_error,
                id + ": refusal escaped as a std::logic_error (" + message +
                    "), outside the std::runtime_error family callers guard a decode with");
        return threw_runtime_error;
      });

      // Which of the two refusals this is — derived from the scenario's own
      // bytes, not taken on the fixture's word.
      const std::string kind_from_wire = (on_wire == "non_string") ? "non_string" : "unknown_token";
      if (kind_from_wire == "unknown_token") {
        bool speaks_it = true;
        try {
          (void)blob_backend_kind_from_str(on_wire);
        } catch (const std::runtime_error&) {
          speaks_it = false;
        }
        REQUIRE(!speaks_it, id + ": a reject scenario carries the token '" + on_wire +
                                "', which this decoder speaks — it cannot be an unknown token");
      }
      std::string kind;
      expect.assert_key_with("rejection_kind", [&](const lazily_test::Json& want) {
        kind = lazily_test::json_string(want);
        return kind == kind_from_wire;
      });
      // The kind derived from the scenario's own BYTES, not the fixture's
      // `rejection_kind` value that `kind` holds. The two are pinned equal by the
      // assertion just above; the vocabulary must be rooted in the wire
      // (`#lznullformblind`).
      rejection_kinds_seen.insert(kind_from_wire);
      if (kind == "non_string") ++non_string_form_replayed;

      // The refusal must be FOR THE STATED REASON. A decoder that refused
      // because it mis-parsed `checksum` satisfies a bare is-error assertion
      // while implementing none of the clause. There is no token to name on the
      // non-string form, and requiring the field name there would pin a message
      // format no codec's native type error carries.
      const bool names_token = expect.assert_key_with_if_present(
          "error_names_token", [&](const lazily_test::Json& want) {
            const std::string token = lazily_test::json_string(want);
            return threw_runtime_error && token == on_wire &&
                   message.find(token) != std::string::npos;
          });
      REQUIRE(names_token == (kind == "unknown_token"),
              id +
                  ": `error_names_token` is carried by exactly the unknown-token refusals; "
                  "kind='" +
                  kind + "'");
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
      message = decode_scenario(scenario, expect, decoders_entered);
    } catch (const std::exception& e) {
      REQUIRE(false, id + ": accept scenario was refused: " + e.what());
    }
    const DecodedScenario decoded = decoded_scenario(scenario, message);
    outcomes_reached.insert("accept");
    const ShmBlobRef& blob = *decoded.blob;
    if (blob.backend != BlobBackendKind::Shm) ++non_shm_decoded;

    expect.assert_key_with("decoded_backend", [&](const lazily_test::Json& want) {
      const std::string got = blob_backend_kind_str(blob.backend);
      decoded_backends.insert(got);
      return lazily_test::json_string(want) == got;
    });
    expect.assert_key("node", static_cast<int64_t>(decoded.node));
    expect.assert_key("offset", blob.offset);
    expect.assert_key("len", blob.len);
    expect.assert_key("generation", blob.generation);
    // The two epochs, each against ITS OWN source. Reading the frame's where the
    // descriptor's is expected is the defect v1 could not express.
    expect.assert_key("frame_epoch", static_cast<int64_t>(decoded.frame_epoch));
    expect.assert_key("blob_epoch", blob.epoch);
    if (static_cast<int64_t>(decoded.frame_epoch) != blob.epoch) ++epochs_differed;
    expect.assert_key("checksum", blob.checksum);

    // The encoder half: `backend` is OMITTED when the value is `Shm`, so a
    // descriptor that predates the field round-trips byte-identically, and it is
    // WRITTEN when it is not. Presence is STRICT — a re-encoded explicit null
    // would be a `backend` entry on the wire, and the null scenarios say the
    // re-encoded frame carries none.
    JsonValue reencoded;
    try {
      reencoded = reencoded_frame(scenario, message);
    } catch (const std::exception& e) {
      REQUIRE(false, id + ": re-encode under its own codec failed: " + e.what());
    }
    const JsonValue* field = dom_blob(reencoded, "re-encoded frame").find("backend");
    const bool present = field != nullptr;
    if (present) {
      REQUIRE(field->is_string(),
              id + ": a re-encoded `backend` entry must be a token, not " + json_write(*field));
      ++backend_field_written;
    }
    expect.assert_key("reencoded_backend_field_present", present);

    if (form == "null") {
      ++null_form_replayed;
      // The null form's whole claim, stated where it is easy to read: it is the
      // ABSENT form, so it decodes as `shm` AND does not survive the round trip.
      REQUIRE(blob.backend == BlobBackendKind::Shm,
              id + ": an explicit null `backend` is the ABSENT form and decodes as `shm` "
                   "(#lzkeynullstrict)");
      REQUIRE(!present, id + ": the null does not survive a round trip — a conforming encoder "
                             "omits `backend` when the value is `shm`");
    }
    expect.finish();
  }

  // The assertion block is evaluated AFTER the replay, because the assertions it
  // carries are about the run: `backend_form_vocabulary` is a set difference
  // against what the decode actually produced, and a count could never reach it.
  {
    lazily_test::AssertionKeys block(std::string(kFixtureId) + " assertions",
                                     lazily_test::json_member(*fx, "assertions"));
    block.assert_key("required_of_binding", std::string("MUST"));
    block.assert_key("scenario_count", static_cast<int64_t>(replayed));
    block.assert_key_with("codecs", [&](const lazily_test::Json& want) {
      const auto& list = lazily_test::json_array(want);
      if (list.size() != 2 || lazily_test::json_string(*list[0]) != "json" ||
          lazily_test::json_string(*list[1]) != "msgpack")
        return false;
      // Both decoders were actually ENTERED, so a runner that quietly skipped one
      // wire cannot satisfy the key by naming it — and the set is built in the
      // dispatch arm rather than from the scenarios' `codec` labels, so a runner
      // that decodes nothing cannot satisfy it either (`#lznullformblind`).
      return decoders_entered.size() == 2 && contains(decoders_entered, "json") &&
             contains(decoders_entered, "msgpack");
    });

    // The enum this binding closes `backend` to, AND the vocabulary guard.
    // If the corpus grows a fourth backend, this fails here rather than in a
    // reject scenario, which is the signal that a spec change (not a corrupt
    // producer) arrived.
    //
    // `arrow` proves the discriminator is READ; the set difference below proves
    // the enum is COMPLETE. It is the assertion that would have caught v1 —
    // where `in_process` was declared in `assertions.backends`, carried by no
    // scenario, and a binding that refused the token (conformingly, by the
    // letter of the clause) passed all eight. It lives on `backends`, whose
    // value is the list it compares against; `backend_form_vocabulary` only
    // states the rule in English, and a tally compared against a paragraph
    // would redden on a copy-edit and stay green on a regression.
    std::set<std::string> declared_backends;
    block.assert_key_with("backends", [&](const lazily_test::Json& want) {
      const auto& list = lazily_test::json_array(want);
      if (list.size() != 3) return false;
      const char* const expected[] = {"shm", "arrow", "in_process"};
      for (std::size_t i = 0; i < list.size(); ++i) {
        const std::string name = lazily_test::json_string(*list[i]);
        if (name != expected[i]) return false;
        // Every named backend is a token this decoder actually speaks.
        if (std::string(blob_backend_kind_str(blob_backend_kind_from_str(name))) != name)
          return false;
        declared_backends.insert(name);
      }
      for (const auto& backend : declared_backends) {
        if (!contains(decoded_backends, backend)) {
          std::cout << "FAIL: backend '" << backend
                    << "' is declared in assertions.backends but no accept scenario decoded to "
                       "it — the vocabulary is incomplete (this is the v1 hole)"
                    << std::endl;
          return false;
        }
      }
      return true;
    });

    block.assert_key_with("backend_forms", [&](const lazily_test::Json& want) {
      const auto& list = lazily_test::json_array(want);
      const char* const expected[] = {"omitted", "shm",        "arrow", "in_process",
                                      "null",    "non_string", "rdma"};
      if (list.size() != sizeof(expected) / sizeof(expected[0])) return false;
      std::set<std::string> declared;
      for (std::size_t i = 0; i < list.size(); ++i) {
        if (lazily_test::json_string(*list[i]) != expected[i]) return false;
        declared.insert(expected[i]);
      }
      // Both directions: every declared form was carried by a scenario whose own
      // bytes this runner read back, and no scenario carried a form the block
      // does not declare.
      return declared == forms_seen;
    });

    block.assert_key_with("rejection_kinds", [&](const lazily_test::Json& want) {
      const auto& list = lazily_test::json_array(want);
      if (list.size() != 2 || lazily_test::json_string(*list[0]) != "unknown_token" ||
          lazily_test::json_string(*list[1]) != "non_string")
        return false;
      return rejection_kinds_seen ==
             std::set<std::string>{std::string("unknown_token"), std::string("non_string")};
    });

    // `#lznullformblind`. This was a comparison against two runner-side string
    // literals and nothing else — a lambda that captured nothing, which is the
    // tell. Delete the replay loop entirely and it stayed green, which is the
    // vacuity `anti_vacuity` two lines below exists to name. It now closes over
    // the outcomes frames actually REACHED, in both directions: no declared
    // outcome went unreached, and no frame reached an outcome the block does not
    // declare. A run where every reject scenario decoded anyway records only
    // `accept` and fails here.
    block.assert_key_with("outcomes", [&](const lazily_test::Json& want) {
      const auto& list = lazily_test::json_array(want);
      if (list.size() != 2 || lazily_test::json_string(*list[0]) != "accept" ||
          lazily_test::json_string(*list[1]) != "reject")
        return false;
      std::set<std::string> declared;
      for (const auto& element : list)
        declared.insert(lazily_test::json_string(*element));
      return declared == outcomes_reached;
    });

    // The nine paragraphs the corpus declares in `assertions.prose`. Each is
    // DISCHARGED by naming the executable keys this run really asserted, never
    // asserted and never excused (`#lzprosekeyconvention`). Four of them —
    // `backend_form_vocabulary`, `null_form`, `non_string_form`,
    // `epoch_disambiguation` — used to be compared against tallies from this
    // run, which passed on any non-empty paragraph: that pinned wording, not
    // behaviour. The tallies survive as run controls below; the obligations now
    // point at the keys that carry them.
    block.prose_key("clause", {"decoded_backend", "rejected", "rejection_is_decode_error",
                               "error_names_token", "reencoded_backend_field_present"});
    // Executable proof that the exact raw text / decoded-hex byte buffer reaches
    // the library decoder rather than a reconstructed proxy.
    block.prose_key("wire_encoding", {"wire_input_fnv1a64"});
    block.prose_key("backend_form_vocabulary", {"backends", "backend_forms", "decoded_backend"});
    block.prose_key("reject_obligation",
                    {"error_names_token", "rejection_is_decode_error", "rejection_kind"});
    // Null is the ABSENT form: it decodes as `shm` (`decoded_backend`) and does
    // not survive the round trip (`reencoded_backend_field_present`), on a form
    // `backend_forms` proves was carried.
    block.prose_key("null_form",
                    {"decoded_backend", "reencoded_backend_field_present", "backend_forms"});
    // A present non-string is refused, and the refusal arrives through the
    // family every caller guards a decode with.
    block.prose_key("non_string_form", {"rejected", "rejection_is_decode_error", "rejection_kind"});
    // The two epochs are separate facts, asserted against separate sources.
    block.prose_key("epoch_disambiguation", {"frame_epoch", "blob_epoch"});
    // The controls, in order: a real decode and a read discriminator
    // (`decoded_backend`), the encoder half (`reencoded_backend_field_present`),
    // a complete vocabulary (`backends`), a full replay (`scenario_count`), and
    // — added by the `#lznullformblind` sweep — BOTH verdicts really reached
    // (`outcomes`), which until now was compared against two runner-side string
    // literals and so was the one name in this list that a runner decoding
    // nothing could still satisfy.
    block.prose_key("anti_vacuity", {"decoded_backend", "reencoded_backend_field_present",
                                     "backends", "scenario_count", "outcomes"});
    // PROXY. `theorem` names a Lean proof in another repository
    // (lazily-formal / docs/zero-copy-transport.md); a run here can only prove
    // its CONSEQUENCE. These three are that consequence: an unknown kind is
    // refused rather than normalized, so a decoded backend is always the one the
    // wire carried and no descriptor is ever routed to another backend's table.
    block.prose_key("theorem", {"decoded_backend", "rejected", "rejection_is_decode_error"});

    block.finish();
  }

  // Verifies every discharge above against what this fixture's run asserted.
  // The ledger's own teardown fails a run that omits this call.
  lazily_test::verify_prose(kFixtureId);

  REQUIRE(replayed == 14, "seven backend forms x two codecs");
  REQUIRE(accepted == 10, "omitted, explicit shm, arrow, in_process and null, on both codecs");
  REQUIRE(rejected == 4, "the unknown token and the non-string, on both codecs");
  // Control (2) from the fixture: a decoder that hardcodes `Shm` and ignores the
  // field passes every omitted/null/shm scenario. Only `arrow` and `in_process`
  // can move this counter.
  REQUIRE(non_shm_decoded == 4,
          "only the `arrow` and `in_process` scenarios decode to a non-shm backend; a decoder "
          "that hardcodes `Shm` satisfies the omitted, null and explicit-shm cases trivially");
  // Control (3): an encoder that echoes the received token back out writes the
  // field on the explicit-shm and null scenarios too.
  REQUIRE(backend_field_written == 4,
          "only the `arrow` and `in_process` scenarios re-encode a `backend` field; an encoder "
          "that echoes what it received writes it on the explicit-shm and null scenarios as well");
  // Control (4): the vocabulary is complete. Asserted as a set difference under
  // `backends` above; restated here so the count and the set are visibly
  // different claims.
  REQUIRE(decoded_backends.size() == 3,
          "all three declared backends appear as a decoded_backend; a scenario count cannot "
          "reach this fact");
  // The three ledgers the `null_form`, `non_string_form` and
  // `epoch_disambiguation` paragraphs used to be compared against. They are real
  // evidence — that the scenarios stating each rule were REACHED — so they stay,
  // as controls over the run. What changed is what they are compared to: a
  // number the runner knows, not a paragraph whose only property under test was
  // being non-empty.
  REQUIRE(null_form_replayed == 2,
          "both null-form scenarios were reached; each decoded as `shm` and re-encoded without a "
          "`backend` entry at its own call site");
  REQUIRE(non_string_form_replayed == 2,
          "both non-string scenarios were reached and classified from their own bytes");
  REQUIRE(epochs_differed == accepted,
          "every accept scenario carries a frame epoch and a descriptor epoch that DIFFER, so a "
          "runner reading one where the other belongs is now visible");
}

int main() {
  test_blob_backend_discriminator_is_replayed();
  REQUIRE_FIXTURES_LOADED(1);
  return 0;
}
