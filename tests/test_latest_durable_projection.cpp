#include <lazily/latest_durable_projection.hpp>

#include <cstdint>
#include <iostream>
#include <string>

#include "test_assertion_keys.hpp"
#include "test_json.hpp"
#include "test_require.hpp"
#include "test_spec_fixture.hpp"

using namespace lazily;
using lazily_test::AssertionKeys;
using lazily_test::Json;

namespace {

constexpr const char* kFixture = "egress/latest_durable_projection.json";

std::string upsert_name(LatestDurableUpsertKind kind) {
  switch (kind) {
  case LatestDurableUpsertKind::Accepted:
    return "accepted";
  case LatestDurableUpsertKind::Unchanged:
    return "unchanged";
  case LatestDurableUpsertKind::AlreadyDurable:
    return "already_durable";
  case LatestDurableUpsertKind::StaleEpoch:
    return "stale_epoch";
  case LatestDurableUpsertKind::EpochConflict:
    return "epoch_conflict";
  }
  return "";
}

std::string claim_name(LatestDurableClaimKind kind) {
  switch (kind) {
  case LatestDurableClaimKind::Claimed:
    return "claimed";
  case LatestDurableClaimKind::Empty:
    return "empty";
  case LatestDurableClaimKind::Busy:
    return "busy";
  case LatestDurableClaimKind::StaleGeneration:
    return "stale_generation";
  }
  return "";
}

std::string ack_name(LatestDurableAckKind kind) {
  switch (kind) {
  case LatestDurableAckKind::Advanced:
    return "advanced";
  case LatestDurableAckKind::Unchanged:
    return "unchanged";
  case LatestDurableAckKind::UnknownEpoch:
    return "unknown_epoch";
  case LatestDurableAckKind::StaleGeneration:
    return "stale_generation";
  }
  return "";
}

std::string failure_name(LatestDurableFailureKind kind) {
  switch (kind) {
  case LatestDurableFailureKind::Pending:
    return "pending";
  case LatestDurableFailureKind::Superseded:
    return "superseded";
  case LatestDurableFailureKind::UnknownEpoch:
    return "unknown_epoch";
  case LatestDurableFailureKind::StaleGeneration:
    return "stale_generation";
  }
  return "";
}

std::string reconnect_name(LatestDurableReconnectKind kind) {
  switch (kind) {
  case LatestDurableReconnectKind::Advanced:
    return "advanced";
  case LatestDurableReconnectKind::Unchanged:
    return "unchanged";
  case LatestDurableReconnectKind::StaleGeneration:
    return "stale_generation";
  }
  return "";
}

template <typename Envelope> void assert_envelope(AssertionKeys& expected, const Envelope& actual) {
  expected.assert_key("generation", actual.generation);
  expected.assert_key("key", actual.key);
  expected.assert_key("epoch", actual.epoch);
  expected.assert_key("value", actual.value);
}

template <typename Core>
void assert_return(Core& core, const Json& op, const Json& expected, const std::string& where) {
  AssertionKeys returns(where + " returns", expected);
  const std::string type = op.find("type")->as_str();
  const std::string key = op.find("key") ? op.find("key")->as_str() : "";
  const auto epoch = op.find("epoch") ? static_cast<std::uint64_t>(op.find("epoch")->as_int()) : 0;
  const auto generation =
      op.find("generation") ? static_cast<std::uint64_t>(op.find("generation")->as_int()) : 0;

  if (type == "upsert_desired") {
    const auto result = core.upsert_desired(key, epoch, op.find("value")->as_str());
    returns.assert_key("upsert", upsert_name(result.kind));
    if (result.kind == LatestDurableUpsertKind::AlreadyDurable)
      returns.assert_key("durable_through", *result.current);
    if (result.kind == LatestDurableUpsertKind::StaleEpoch)
      returns.assert_key("current", *result.current);
  } else if (type == "claim") {
    const auto result = core.claim(key, generation);
    returns.assert_key("claim", claim_name(result.kind));
    if (result.envelope)
      returns.with_sub("envelope", [&](AssertionKeys& envelope) {
        assert_envelope(envelope, *result.envelope);
      });
    if (result.kind == LatestDurableClaimKind::StaleGeneration)
      returns.assert_key("current", *result.current);
  } else if (type == "ack_applied") {
    const auto result = core.ack_applied(key, generation, epoch);
    returns.assert_key("ack", ack_name(result.kind));
    if (result.durable_through) returns.assert_key("durable_through", *result.durable_through);
    if (result.kind == LatestDurableAckKind::StaleGeneration)
      returns.assert_key("current", *result.current);
  } else if (type == "fail_retryable") {
    const auto result = core.fail_retryable(key, generation, epoch);
    returns.assert_key("failure", failure_name(result.kind));
    if (result.kind == LatestDurableFailureKind::StaleGeneration)
      returns.assert_key("current", *result.current);
  } else if (type == "reconnect") {
    const auto result = core.reconnect(generation);
    returns.assert_key("reconnect", reconnect_name(result.kind));
    if (result.kind == LatestDurableReconnectKind::Advanced) {
      returns.assert_key("generation", result.generation);
      returns.assert_key("requeued", result.requeued);
      returns.assert_key("superseded", result.superseded);
    } else if (result.kind == LatestDurableReconnectKind::StaleGeneration) {
      returns.assert_key("current", result.generation);
    }
  } else {
    REQUIRE(false, ("unknown latest-durable operation: " + type).c_str());
  }
}

bool equal_entries(const Json& expected,
                   const std::vector<LatestDurableEntry<std::string, std::string>>& actual) {
  if (!expected.is_array() || expected.array.size() != actual.size()) return false;
  for (std::size_t index = 0; index < actual.size(); ++index) {
    const Json& want = *expected.array[index];
    const auto& got = actual[index];
    if (!want.is_object() || want.object.size() != 4 || want.find("key")->as_str() != got.key)
      return false;
    const Json& desired = *want.find("desired");
    if (desired.is_null() != !got.desired) return false;
    if (got.desired &&
        (desired.object.size() != 2 ||
         static_cast<std::uint64_t>(desired.find("epoch")->as_int()) != got.desired->epoch ||
         desired.find("value")->as_str() != got.desired->value))
      return false;
    const Json& inflight = *want.find("inflight");
    if (inflight.is_null() != !got.inflight) return false;
    if (got.inflight &&
        (inflight.object.size() != 4 ||
         static_cast<std::uint64_t>(inflight.find("generation")->as_int()) !=
             got.inflight->generation ||
         inflight.find("key")->as_str() != got.inflight->key ||
         static_cast<std::uint64_t>(inflight.find("epoch")->as_int()) != got.inflight->epoch ||
         inflight.find("value")->as_str() != got.inflight->value))
      return false;
    const Json& durable = *want.find("durable_through");
    if (durable.is_null() != !got.durable_through) return false;
    if (got.durable_through && static_cast<std::uint64_t>(durable.as_int()) != *got.durable_through)
      return false;
  }
  return true;
}

void replay_fixture() {
  const auto document = lazily_test::parse_json(
      lazily_test::spec_fixture_text("egress", "latest_durable_projection.json"));
  REQUIRE(document->find("kind")->as_str() == "LatestDurableProjection", "wrong fixture kind");
  REQUIRE(document->find("model")->as_str() == "LatestDurableProjectionCore",
          "wrong fixture model");
  const Json& scenarios = *document->find("scenarios");
  std::size_t step_count = 0;
  for (std::size_t scenario_index = 0; scenario_index < scenarios.array.size(); ++scenario_index) {
    const Json& scenario = *scenarios.array[scenario_index];
    const std::string id = lazily_test::record_scenario_at(kFixture, scenario, scenario_index);
    LatestDurableProjectionCore<std::string, std::string> core(
        static_cast<std::uint64_t>(scenario.find("generation")->as_int()));
    for (const auto& step_ptr : scenario.find("steps")->array) {
      const Json& step = *step_ptr;
      const std::string where =
          std::string(kFixture) + " " + id + " step " + std::to_string(step_count);
      assert_return(core, *step.find("op"), *step.find("returns"), where);
      AssertionKeys expected(where + " expected", *step.find("expected"));
      expected.assert_key("generation", core.generation());
      expected.assert_key_with(
          "entries", [&](const Json& entries) { return equal_entries(entries, core.snapshots()); });
      ++step_count;
    }
  }
  REQUIRE(step_count == 22, "canonical latest-durable trace grew; extend the replay");
}

void reactive_flavors() {
  Context context;
  LatestDurableProjection<std::string, std::string> single(context, 1);
  auto single_entry = single.entry_handle(context, "doc");
  REQUIRE(!context.get(single_entry).desired, "single projection starts empty");
  single.upsert_desired(context, "doc", 1, "A");
  REQUIRE(context.get(single_entry).desired->epoch == 1, "single projection invalidates");

  ThreadSafeContext thread_context;
  ThreadSafeLatestDurableProjection<std::string, std::string> thread_safe(thread_context, 1);
  auto thread_entry = thread_safe.entry_handle(thread_context, "doc");
  thread_safe.upsert_desired(thread_context, "doc", 1, "A");
  REQUIRE(thread_context.get(thread_entry).desired->epoch == 1,
          "thread-safe projection invalidates");

  AsyncContext async_context;
  AsyncLatestDurableProjection<std::string, std::string> asynchronous(async_context, 1);
  auto async_entry = asynchronous.entry_handle(async_context, "doc");
  asynchronous.upsert_desired(async_context, "doc", 1, "A");
  REQUIRE(async_context.context().get(async_entry).desired->epoch == 1,
          "async projection invalidates");
}

} // namespace

int main() {
  replay_fixture();
  reactive_flavors();
  REQUIRE_FIXTURES_LOADED(1);
  std::cout << "latest durable projection: OK\n";
}
