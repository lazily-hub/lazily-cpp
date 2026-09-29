#include <lazily/sim_consumer_testkit.hpp>

#include "test_assertion_keys.hpp"
#include "test_json.hpp"
#include "test_require.hpp"
#include "test_spec_fixture.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace lazily;
using lazily_test::Json;

namespace {

constexpr const char* kFixture = "simulation/consumer_testkit.json";

std::int64_t json_i64(const Json& value) {
  REQUIRE(value.type == Json::Type::Number, "expected JSON integer");
  return std::stoll(value.number_token);
}

ReplayValue replay_value(const Json& value) {
  switch (value.type) {
  case Json::Type::Null:
    return ReplayValue::null();
  case Json::Type::Bool:
    return ReplayValue::boolean(value.boolean);
  case Json::Type::Number:
    return ReplayValue::integer(json_i64(value));
  case Json::Type::String:
    return ReplayValue::text(value.str);
  case Json::Type::Array: {
    std::vector<ReplayValue> items;
    for (const auto& item : value.array)
      items.push_back(replay_value(*item));
    return ReplayValue::seq(std::move(items));
  }
  case Json::Type::Object: {
    std::vector<std::pair<ReplayValue, ReplayValue>> entries;
    for (const auto& item : value.object)
      entries.emplace_back(ReplayValue::text(item.first), replay_value(*item.second));
    return ReplayValue::map(std::move(entries));
  }
  }
  throw std::runtime_error("unsupported JSON value");
}

SimConsumerAction action(const Json& value) {
  SimConsumerAction result;
  result.id = lazily_test::json_string(lazily_test::json_member(value, "id"));
  result.actor_id = lazily_test::json_string(lazily_test::json_member(value, "actor_id"));
  result.kind = lazily_test::json_string(lazily_test::json_member(value, "kind"));
  result.version = lazily_test::json_string(lazily_test::json_member(value, "version"));
  result.payload = replay_value(lazily_test::json_member(value, "payload"));
  if (const Json* cause = value.find("cause_id"))
    result.cause_id = lazily_test::json_string(*cause);
  return result;
}

SimConsumerAdapterKind adapter_kind(const std::string& value) {
  if (value == "in_memory") return SimConsumerAdapterKind::InMemory;
  if (value == "postgres") return SimConsumerAdapterKind::Postgres;
  if (value == "nats") return SimConsumerAdapterKind::Nats;
  if (value == "external_process") return SimConsumerAdapterKind::ExternalProcess;
  throw std::runtime_error("unknown adapter kind: " + value);
}

SimConsumerExternalPortKind external_port(const std::string& value) {
  if (value == "cli") return SimConsumerExternalPortKind::Cli;
  if (value == "filesystem") return SimConsumerExternalPortKind::Filesystem;
  if (value == "local_socket") return SimConsumerExternalPortKind::LocalSocket;
  if (value == "editor_replica") return SimConsumerExternalPortKind::EditorReplica;
  throw std::runtime_error("unknown external port: " + value);
}

std::string external_port_name(SimConsumerExternalPortKind value) {
  switch (value) {
  case SimConsumerExternalPortKind::Cli:
    return "cli";
  case SimConsumerExternalPortKind::Filesystem:
    return "filesystem";
  case SimConsumerExternalPortKind::LocalSocket:
    return "local_socket";
  case SimConsumerExternalPortKind::EditorReplica:
    return "editor_replica";
  case SimConsumerExternalPortKind::None:
    return "";
  }
  return "";
}

struct AdapterState {
  std::int64_t value = 0;
  int probes = 0;
  std::shared_ptr<SimConsumerWorldEvidence> world;
  std::vector<SimConsumerAction> history;
};

struct BuiltKit {
  std::unique_ptr<SimConsumerTestkit> testkit;
  std::map<std::string, std::shared_ptr<AdapterState>> states;
  std::shared_ptr<std::vector<std::int64_t>> baseline_values;
};

BuiltKit build_kit(const Json& scenario, const std::vector<SimConsumerPort>& ports) {
  BuiltKit built;
  built.baseline_values = std::make_shared<std::vector<std::int64_t>>();
  const std::string simulation_id =
      lazily_test::json_string(lazily_test::json_member(scenario, "simulation_adapter_id"));
  SimConsumerTestkitSpec spec;
  spec.simulation_adapter_id = simulation_id;
  for (const auto& required :
       lazily_test::json_array(lazily_test::json_member(scenario, "required_real_adapters")))
    spec.required_real_adapters.push_back(adapter_kind(lazily_test::json_string(*required)));
  for (const auto& required :
       lazily_test::json_array(lazily_test::json_member(scenario, "required_external_processes"))) {
    spec.required_external_processes.push_back(
        {lazily_test::json_string(lazily_test::json_member(*required, "adapter_id")),
         external_port(lazily_test::json_string(lazily_test::json_member(*required, "port")))});
  }

  for (const auto& node : lazily_test::json_array(lazily_test::json_member(scenario, "adapters"))) {
    SimConsumerAdapter adapter;
    adapter.id = lazily_test::json_string(lazily_test::json_member(*node, "id"));
    adapter.kind = adapter_kind(lazily_test::json_string(lazily_test::json_member(*node, "kind")));
    adapter.service_id = lazily_test::json_string(lazily_test::json_member(*node, "service_id"));
    adapter.reducer_id = lazily_test::json_string(lazily_test::json_member(*node, "reducer_id"));
    adapter.production_reducer_id =
        lazily_test::json_string(lazily_test::json_member(*node, "production_reducer_id"));
    adapter.protocol_id = lazily_test::json_string(lazily_test::json_member(*node, "protocol_id"));
    if (const Json* port = node->find("external_port"))
      adapter.external_port = external_port(lazily_test::json_string(*port));
    adapter.ports = ports;
    const std::string clock_stub =
        lazily_test::json_string(lazily_test::json_member(*node, "clock_stub"));
    for (auto& port : adapter.ports)
      port.stubbed = adapter.kind == SimConsumerAdapterKind::InMemory &&
                     port.id == "logical.clock" && clock_stub == "stubbed";

    const std::string execution_mode =
        lazily_test::json_string(lazily_test::json_member(*node, "execution_mode"));
    const std::string history_mode =
        lazily_test::json_string(lazily_test::json_member(*node, "history_mode"));
    const std::int64_t delta_bias = json_i64(lazily_test::json_member(*node, "delta_bias"));
    auto state = std::make_shared<AdapterState>();
    built.states.emplace(adapter.id, state);
    if (adapter.kind != SimConsumerAdapterKind::InMemory)
      adapter.probe = [state] { ++state->probes; };
    if (adapter.kind == SimConsumerAdapterKind::InMemory)
      adapter.world_evidence = [state] { return state->world; };
    adapter.reset = [state, kind = adapter.kind] {
      state->value = 0;
      state->history.clear();
      if (kind == SimConsumerAdapterKind::InMemory)
        state->world = std::make_shared<SimConsumerWorldEvidence>();
    };
    adapter.apply = [state, kind = adapter.kind, execution_mode, history_mode,
                     delta_bias](SimConsumerAction applied) {
      state->value += applied.payload.int_value() + delta_bias;
      if (kind == SimConsumerAdapterKind::InMemory) {
        if (execution_mode == "sim_world") state->world->record(applied.id);
      } else if (history_mode == "exact") {
        state->history.push_back(std::move(applied));
      }
    };
    adapter.observe = [state, baseline = built.baseline_values,
                       is_baseline = adapter.id == simulation_id] {
      if (is_baseline) baseline->push_back(state->value);
      return SimConsumerObservation{{"consumer.value", ReplayValue::integer(state->value)}};
    };
    if (adapter.kind != SimConsumerAdapterKind::InMemory)
      adapter.materialized_history = [state] { return state->history; };
    spec.adapters.push_back(std::move(adapter));
  }
  built.testkit = std::make_unique<SimConsumerTestkit>(std::move(spec));
  return built;
}

SimConsumerGeneratedScenario generated_scenario(const Json& root,
                                                const std::vector<SimConsumerAction>& actions) {
  SimConsumerGeneratedScenario scenario;
  scenario.generator_name = "consumer_simulation";
  scenario.generator_version = lazily_test::json_string(
      lazily_test::json_member(lazily_test::json_member(root, "generator"), "version"));
  scenario.seed_hex = lazily_test::json_string(lazily_test::json_member(root, "seed"));
  for (const auto& item : actions)
    scenario.actions.push_back({"apply", item});
  return scenario;
}

bool json_strings_equal(const Json& want, const std::vector<std::string>& actual) {
  const auto& array = lazily_test::json_array(want);
  if (array.size() != actual.size()) return false;
  for (std::size_t index = 0; index < array.size(); ++index)
    if (lazily_test::json_string(*array[index]) != actual[index]) return false;
  return true;
}

bool json_ints_equal(const Json& want, const std::vector<std::int64_t>& actual) {
  const auto& array = lazily_test::json_array(want);
  if (array.size() != actual.size()) return false;
  for (std::size_t index = 0; index < array.size(); ++index)
    if (json_i64(*array[index]) != actual[index]) return false;
  return true;
}

std::string observation_relation(const SimConsumerRunResult& result) {
  for (const auto& checkpoint : result.checkpoints) {
    if (checkpoint.observation_digests.empty()) return "diverged";
    const std::string first = checkpoint.observation_digests.begin()->second;
    for (const auto& digest : checkpoint.observation_digests)
      if (digest.second != first) return "diverged";
  }
  return "all_equal_at_every_checkpoint";
}

void replay_fixture() {
  const auto fixture = lazily_test::parse_json(
      lazily_test::spec_fixture_text("simulation", "consumer_testkit.json"));
  std::vector<SimConsumerAction> actions;
  for (const auto& item : lazily_test::json_array(lazily_test::json_member(*fixture, "actions")))
    actions.push_back(action(*item));
  std::vector<SimConsumerPort> ports;
  for (const auto& item : lazily_test::json_array(lazily_test::json_member(*fixture, "ports"))) {
    ports.push_back(
        {lazily_test::json_string(lazily_test::json_member(*item, "id")),
         lazily_test::json_string(lazily_test::json_member(*item, "kind")),
         lazily_test::json_string(lazily_test::json_member(*item, "determinism")) == "deterministic"
             ? SimConsumerPortDeterminism::Deterministic
             : SimConsumerPortDeterminism::Nondeterministic,
         false});
  }
  const auto& raw_scenarios =
      lazily_test::json_array(lazily_test::json_member(*fixture, "scenarios"));
  REQUIRE(raw_scenarios.size() == 5, "consumer testkit fixture must carry five scenarios");
  for (const auto& view : lazily_test::scenario_views(kFixture, raw_scenarios)) {
    const Json& scenario = view.replay();
    lazily_test::AssertionKeys expected(view.id() + " expected",
                                        lazily_test::json_member(scenario, "expected"));
    auto built = build_kit(scenario, ports);
    std::unique_ptr<SimConsumerRunResult> result;
    std::unique_ptr<SimConsumerDivergenceError> divergence;
    try {
      result = std::make_unique<SimConsumerRunResult>(
          built.testkit->run(generated_scenario(*fixture, actions)));
    } catch (const SimConsumerDivergenceError& error) {
      divergence = std::make_unique<SimConsumerDivergenceError>(error);
    }
    expected.assert_key("outcome", divergence ? divergence->kind : std::string("success"));
    if (result) {
      expected.assert_key_with("adapter_ids", [&](const Json& want) {
        return json_strings_equal(want, result->adapter_ids);
      });
      std::vector<std::int64_t> steps;
      std::vector<std::string> action_ids;
      for (const auto& checkpoint : result->checkpoints) {
        steps.push_back(static_cast<std::int64_t>(checkpoint.step));
        action_ids.push_back(checkpoint.action_id);
      }
      expected.assert_key_with("checkpoint_steps",
                               [&](const Json& want) { return json_ints_equal(want, steps); });
      expected.assert_key_with_if_present("checkpoint_action_ids", [&](const Json& want) {
        return json_strings_equal(want, action_ids);
      });
      expected.assert_key_with("checkpoint_values", [&](const Json& want) {
        return json_ints_equal(want, *built.baseline_values);
      });
      expected.assert_key_if_present("observation_relation", observation_relation(*result));
      expected.assert_key_if_present("materialized_history_relation",
                                     std::string("exact_prefix_at_every_checkpoint"));
      bool probes_once = true;
      for (const auto& entry : built.states)
        if (!entry.second->world && entry.second->probes != 1) probes_once = false;
      expected.assert_key_if_present("probe_relation", probes_once
                                                           ? std::string("every_real_adapter_once")
                                                           : std::string("probe_mismatch"));
      const SimConsumerAdapterEvidence* external = nullptr;
      for (const auto& evidence : result->adapter_evidence)
        if (evidence.kind == SimConsumerAdapterKind::ExternalProcess) external = &evidence;
      expected.assert_key_if_present("external_adapter_id", external ? external->adapter_id : "");
      expected.assert_key_if_present("external_port",
                                     external ? external_port_name(external->external_port) : "");
      expected.assert_key_if_present("external_protocol_id", external ? external->protocol_id : "");
      expected.assert_key_if_present("external_reducer_id", external ? external->reducer_id : "");
      expected.assert_key_if_present("external_production_reducer_id",
                                     external ? external->production_reducer_id : "");
    } else {
      REQUIRE(divergence != nullptr, "failed scenario must produce a divergence");
      expected.assert_key("step", static_cast<long long>(divergence->step));
      expected.assert_key("action_id", divergence->action_id);
      expected.assert_key("adapter_id", divergence->adapter_id);
      expected.assert_key_if_present("observation_id", divergence->observation_id);
      expected.assert_key_if_present("expected_prefix_length",
                                     static_cast<long long>(divergence->expected_prefix_length));
      expected.assert_key_if_present("actual_prefix_length",
                                     static_cast<long long>(divergence->actual_prefix_length));
    }
  }
}

struct Pair {
  std::vector<SimConsumerAdapter> adapters;
  std::shared_ptr<int> probes;
};

Pair valid_pair() {
  Pair pair;
  pair.probes = std::make_shared<int>(0);
  auto world = std::make_shared<SimConsumerWorldEvidence>();
  const std::vector<SimConsumerPort> ports = {
      {"state.store", "storage", SimConsumerPortDeterminism::Deterministic, false}};
  SimConsumerAdapter memory;
  memory.id = "memory";
  memory.kind = SimConsumerAdapterKind::InMemory;
  memory.production_reducer_id = "counter.reducer.v1";
  memory.protocol_id = "counter.protocol.v1";
  memory.reducer_id = "counter.reducer.v1";
  memory.ports = ports;
  memory.world_evidence = [world] { return world; };
  memory.reset = [] {};
  memory.apply = [world](SimConsumerAction item) { world->record(item.id); };
  memory.observe = [] {
    return SimConsumerObservation{{"consumer.value", ReplayValue::integer(0)}};
  };
  SimConsumerAdapter postgres;
  postgres.id = "postgres.integration";
  postgres.kind = SimConsumerAdapterKind::Postgres;
  postgres.production_reducer_id = "counter.reducer.v1";
  postgres.protocol_id = "counter.protocol.v1";
  postgres.reducer_id = "counter.reducer.v1";
  postgres.service_id = "postgres.service";
  postgres.ports = ports;
  postgres.probe = [probes = pair.probes] { ++*probes; };
  postgres.reset = [] {};
  postgres.apply = [](SimConsumerAction) {};
  postgres.observe = [] {
    return SimConsumerObservation{{"consumer.value", ReplayValue::integer(0)}};
  };
  postgres.materialized_history = [] { return std::vector<SimConsumerAction>{}; };
  pair.adapters = {std::move(memory), std::move(postgres)};
  return pair;
}

void rejection_units() {
  auto pair = valid_pair();
  bool rejected = false;
  try {
    SimConsumerTestkit invalid({"memory", {SimConsumerAdapterKind::Nats}, {}, pair.adapters});
    (void)invalid;
  } catch (const SimConsumerConformanceError&) {
    rejected = true;
  }
  REQUIRE(rejected && *pair.probes == 0, "constructor must reject before callbacks");

  pair = valid_pair();
  pair.adapters.front().ports.front().stubbed = true;
  rejected = false;
  try {
    SimConsumerTestkit invalid({"memory", {SimConsumerAdapterKind::Postgres}, {}, pair.adapters});
    (void)invalid;
  } catch (const SimConsumerConformanceError&) {
    rejected = true;
  }
  REQUIRE(rejected, "constructor must reject a deterministic stub");

  pair = valid_pair();
  SimConsumerTestkit testkit({"memory", {SimConsumerAdapterKind::Postgres}, {}, pair.adapters});
  SimConsumerGeneratedScenario scenario{
      "consumer_simulation",
      "1",
      "ABC",
      {{"apply",
        {"increment.0", "consumer", "counter.increment", "1", ReplayValue::integer(1), ""}}}};
  rejected = false;
  try {
    (void)testkit.run(scenario);
  } catch (const SimConsumerConformanceError&) {
    rejected = true;
  }
  REQUIRE(rejected, "run must reject an invalid seed");
  scenario.seed_hex = std::string(64, '0');
  scenario.actions.push_back(scenario.actions.front());
  rejected = false;
  try {
    (void)testkit.run(scenario);
  } catch (const SimConsumerConformanceError&) {
    rejected = true;
  }
  REQUIRE(rejected, "run must reject duplicate action ids");
}

} // namespace

int main() {
  replay_fixture();
  rejection_units();
  REQUIRE_FIXTURES_LOADED(1);
}
