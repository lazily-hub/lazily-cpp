#include <lazily/durable_client.hpp>

#include "test_json.hpp"
#include "test_spec_fixture.hpp"

#include <string>

using namespace lazily;
using lazily_test::Json;

static DurableEnvelope envelope(const Json& value) {
  DurableEnvelope result;
  result.protocol_version =
      lazily_test::json_u64(lazily_test::json_member(value, "protocol_version"));
  result.message_id = lazily_test::json_string(lazily_test::json_member(value, "message_id"));
  result.schema_version = lazily_test::json_u64(lazily_test::json_member(value, "schema_version"));
  result.codec_version = lazily_test::json_u64(lazily_test::json_member(value, "codec_version"));
  for (const auto& byte : lazily_test::json_array(lazily_test::json_member(value, "payload")))
    result.payload.push_back(static_cast<std::uint8_t>(lazily_test::json_u64(*byte)));
  return result;
}

static std::string validation_name(EnvelopeValidation value) {
  switch (value) {
  case EnvelopeValidation::Accepted:
    return "accepted";
  case EnvelopeValidation::UnsupportedProtocolVersion:
    return "unsupported_protocol_version";
  case EnvelopeValidation::InvalidMessageId:
    return "invalid_message_id";
  case EnvelopeValidation::InvalidSchemaVersion:
    return "invalid_schema_version";
  case EnvelopeValidation::InvalidCodecVersion:
    return "invalid_codec_version";
  }
  return "unknown";
}

static std::string delivery_name(DeliveryClassification value) {
  switch (value) {
  case DeliveryClassification::First:
    return "first";
  case DeliveryClassification::Duplicate:
    return "duplicate";
  case DeliveryClassification::Conflict:
    return "conflict";
  }
  return "unknown";
}

static std::string projection_name(ProjectionApplyStatus value) {
  switch (value) {
  case ProjectionApplyStatus::Buffered:
    return "buffered";
  case ProjectionApplyStatus::Applied:
    return "applied";
  case ProjectionApplyStatus::Duplicate:
    return "duplicate";
  case ProjectionApplyStatus::IdentityConflict:
    return "conflict";
  case ProjectionApplyStatus::Invalid:
    return "invalid";
  }
  return "unknown";
}

static DurableProjectionUpdate<std::string> projection(std::uint64_t position,
                                                       std::string fingerprint) {
  return {{"orders", position, std::move(fingerprint), ProjectionCapability::CompleteHistory},
          position,
          "value"};
}

class FakeTransport final : public NatsDurableClientTransport {
public:
  BrokerPubAck publish(const std::string&, const DurableEnvelope&) override {
    return {"OWNER", 9, false};
  }
  void subscribe(const std::string&, Handler) override {}
};

int main() {
  const auto fixture =
      lazily_test::parse_json(lazily_test::spec_fixture_text("durable-client", "envelope_v1.json"));
  REQUIRE(!lazily_test::json_bool(lazily_test::json_member(*fixture, "owner_authority")),
          "durable client must not claim owner authority");
  const DurableTierDeclaration tiers;
  REQUIRE(tiers.core && tiers.client && !tiers.durable_host && !tiers.distributed_host &&
              !tiers.accelerated_host,
          "binding must advertise exactly Core + Client");

  for (const auto& item :
       lazily_test::json_array(lazily_test::json_member(*fixture, "envelope_vectors"))) {
    const auto actual = envelope(lazily_test::json_member(*item, "envelope"));
    lazily_test::AssertionKeys expected("durable-client/envelope_v1.json envelope expected",
                                        lazily_test::json_member(*item, "expected"));
    int decoded = 0;
    FakeTransport transport;
    DurableClient<int, std::string> client(
        transport, [](const int&) { return std::vector<std::uint8_t>{}; },
        [&decoded](const std::vector<std::uint8_t>&) {
          ++decoded;
          return projection(1, "source-1");
        });
    const auto observed = client.observe(actual);
    expected.assert_key("reason", validation_name(observed.validation));
    expected.assert_key("accepted", observed.validation == EnvelopeValidation::Accepted);
    expected.assert_key("payload_decoded", decoded > 0);
  }

  for (const auto& item :
       lazily_test::json_array(lazily_test::json_member(*fixture, "ordering_vectors"))) {
    DurableObservationOrder order;
    for (const auto& id :
         lazily_test::json_array(lazily_test::json_member(*item, "observed_message_ids")))
      order.observe({1, lazily_test::json_string(*id), 1, 1, {}});
    const auto& expected =
        lazily_test::json_array(lazily_test::json_member(*item, "expected_delivery_order"));
    REQUIRE(order.message_ids().size() == expected.size(), "delivery order size mismatch");
    for (std::size_t i = 0; i < expected.size(); ++i)
      REQUIRE(order.message_ids()[i] == lazily_test::json_string(*expected[i]),
              "delivery order mismatch");
    REQUIRE(order.owner_order_inferred() ==
                lazily_test::json_bool(lazily_test::json_member(*item, "owner_order_inferred")),
            "owner order inference mismatch");
  }

  for (const auto& item :
       lazily_test::json_array(lazily_test::json_member(*fixture, "projection_ordering_vectors"))) {
    DurableProjectionClient<std::string> client;
    const auto& positions =
        lazily_test::json_array(lazily_test::json_member(*item, "observed_source_positions"));
    const auto& expected = lazily_test::json_array(
        lazily_test::json_member(*item, "expected_delivery_classification"));
    for (std::size_t i = 0; i < positions.size(); ++i) {
      const auto position = lazily_test::json_u64(*positions[i]);
      REQUIRE(projection_name(
                  client.apply(projection(position, "source-" + std::to_string(position)))) ==
                  lazily_test::json_string(*expected[i]),
              "projection delivery classification mismatch");
    }
    const auto applied = client.applied_positions("orders");
    const auto& expected_applied =
        lazily_test::json_array(lazily_test::json_member(*item, "expected_applied_positions"));
    REQUIRE(applied.size() == expected_applied.size(), "applied projection size mismatch");
    for (std::size_t i = 0; i < applied.size(); ++i)
      REQUIRE(applied[i] == lazily_test::json_u64(*expected_applied[i]),
              "applied projection order mismatch");
    REQUIRE(!DurableProjectionClient<std::string>::may_authorize_transition(),
            "projection must remain advisory");
  }

  for (const auto& item :
       lazily_test::json_array(lazily_test::json_member(*fixture, "dedup_vectors"))) {
    DurableDeduplicator dedup;
    const auto& deliveries = lazily_test::json_array(lazily_test::json_member(*item, "deliveries"));
    const auto& expected =
        lazily_test::json_array(lazily_test::json_member(*item, "expected_classification"));
    for (std::size_t i = 0; i < deliveries.size(); ++i)
      REQUIRE(delivery_name(dedup.classify(envelope(*deliveries[i]))) ==
                  lazily_test::json_string(*expected[i]),
              "dedup classification mismatch");
  }

  for (const auto& item :
       lazily_test::json_array(lazily_test::json_member(*fixture, "receipt_vectors"))) {
    const auto& receipt = lazily_test::json_member(*item, "receipt");
    DurableHostReceipt actual{
        lazily_test::json_u64(lazily_test::json_member(receipt, "protocol_version")),
        lazily_test::json_string(lazily_test::json_member(receipt, "receipt_id")),
        lazily_test::json_string(lazily_test::json_member(receipt, "message_id")),
        DurableHostOutcome::Committed,
        lazily_test::json_u64(lazily_test::json_member(receipt, "owner_position"))};
    REQUIRE(!actual.receipt_id.empty() && !actual.transport_ack_equivalent,
            "durable receipt must remain distinct from PubAck");
  }

  for (const auto& item : lazily_test::json_array(
           lazily_test::json_member(*fixture, "projection_fingerprint_vectors"))) {
    const auto parse = [](const Json& value) {
      return ProjectionFingerprint{
          lazily_test::json_string(lazily_test::json_member(value, "projection_id")),
          lazily_test::json_u64(lazily_test::json_member(value, "source_position")),
          lazily_test::json_string(lazily_test::json_member(value, "fingerprint")),
          lazily_test::json_string(lazily_test::json_member(value, "completeness")) ==
                  "complete_history"
              ? ProjectionCapability::CompleteHistory
              : ProjectionCapability::LatestStateOnly};
    };
    const auto left = parse(lazily_test::json_member(*item, "left"));
    const auto right = parse(lazily_test::json_member(*item, "right"));
    lazily_test::AssertionKeys expected(
        "durable-client/envelope_v1.json projection fingerprint expected",
        lazily_test::json_member(*item, "expected"));
    expected.assert_key("same_source", left.source_position == right.source_position);
    expected.assert_key("same_fingerprint", left.fingerprint == right.fingerprint);
    expected.assert_key("same_completeness", left.completeness == right.completeness);
    expected.assert_key("equivalent", left.equivalent_to(right));
    static_assert(!ProjectionFingerprint::may_authorize_transition);
  }

  REQUIRE_FIXTURES_LOADED(1);
}
