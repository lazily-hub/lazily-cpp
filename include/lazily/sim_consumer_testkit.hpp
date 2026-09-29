#ifndef LAZILY_SIM_CONSUMER_TESTKIT_HPP
#define LAZILY_SIM_CONSUMER_TESTKIT_HPP

#include <lazily/replay.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lazily {

enum class SimConsumerAdapterKind { InMemory, Postgres, Nats, ExternalProcess };
enum class SimConsumerExternalPortKind { None, Cli, Filesystem, LocalSocket, EditorReplica };
enum class SimConsumerPortDeterminism { Deterministic, Nondeterministic };

struct SimConsumerPort {
  std::string id;
  std::string kind;
  SimConsumerPortDeterminism determinism = SimConsumerPortDeterminism::Deterministic;
  bool stubbed = false;
};

struct SimConsumerExternalProcessSelection {
  std::string adapter_id;
  SimConsumerExternalPortKind port = SimConsumerExternalPortKind::None;
};

struct SimConsumerAction {
  std::string id;
  std::string actor_id;
  std::string kind;
  std::string version;
  ReplayValue payload;
  std::string cause_id;
};

struct SimConsumerGeneratedAction {
  std::string command;
  SimConsumerAction action;
};

struct SimConsumerGeneratedScenario {
  std::string generator_name;
  std::string generator_version;
  std::string seed_hex;
  std::vector<SimConsumerGeneratedAction> actions;
  std::int64_t scenario_index = 0;
  std::vector<std::string> coverage_labels;
};

struct SimConsumerTraceEntry {
  std::string action_id;
  std::string kind;
};

// The intentionally narrow evidence interface for bindings without a scheduler.
class SimConsumerWorldEvidence {
public:
  SimConsumerWorldEvidence() : identity_(std::make_shared<const int>(0)) {}

  const std::shared_ptr<const void>& identity() const { return identity_; }
  std::uint64_t step_count() const { return step_count_; }
  const std::vector<SimConsumerTraceEntry>& trace_entries() const { return trace_entries_; }

  void record(const std::string& action_id, const std::string& kind = "action_applied") {
    if (!valid_id(action_id)) throw std::invalid_argument("trace action id must be stable");
    if (!valid_id(kind) || kind.rfind("action_", 0) != 0)
      throw std::invalid_argument("trace kind must start with action_");
    ++step_count_;
    trace_entries_.push_back({action_id, kind});
  }

private:
  static bool valid_id(const std::string& value) {
    if (value.empty() || value.size() > 128 || value.front() < 'a' || value.front() > 'z')
      return false;
    return std::all_of(value.begin() + 1, value.end(), [](char c) {
      return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == ':' ||
             c == '-';
    });
  }

  std::shared_ptr<const void> identity_;
  std::uint64_t step_count_ = 0;
  std::vector<SimConsumerTraceEntry> trace_entries_;
};

using SimConsumerObservation = std::map<std::string, ReplayValue>;

struct SimConsumerAdapter {
  std::string id;
  SimConsumerAdapterKind kind = SimConsumerAdapterKind::InMemory;
  std::string production_reducer_id;
  std::string protocol_id;
  std::string reducer_id;
  std::string service_id;
  SimConsumerExternalPortKind external_port = SimConsumerExternalPortKind::None;
  std::vector<SimConsumerPort> ports;
  std::function<void()> probe;
  std::function<std::shared_ptr<SimConsumerWorldEvidence>()> world_evidence;
  std::function<void()> reset;
  std::function<void(SimConsumerAction)> apply;
  std::function<SimConsumerObservation()> observe;
  std::function<std::vector<SimConsumerAction>()> materialized_history;
};

struct SimConsumerTestkitSpec {
  std::string simulation_adapter_id;
  std::vector<SimConsumerAdapterKind> required_real_adapters;
  std::vector<SimConsumerExternalProcessSelection> required_external_processes;
  std::vector<SimConsumerAdapter> adapters;
};

struct SimConsumerAdapterEvidence {
  std::string adapter_id;
  SimConsumerAdapterKind kind;
  std::string service_id;
  SimConsumerExternalPortKind external_port;
  std::string protocol_id;
  std::string reducer_id;
  std::string production_reducer_id;
};

struct SimConsumerCheckpoint {
  std::uint64_t step;
  std::string action_id;
  std::map<std::string, std::string> observation_digests;
};

struct SimConsumerRunResult {
  std::string scenario_digest;
  std::vector<std::string> adapter_ids;
  std::vector<SimConsumerAdapterEvidence> adapter_evidence;
  std::shared_ptr<const void> simulation_world_identity;
  std::uint64_t simulation_world_step_count = 0;
  std::vector<SimConsumerTraceEntry> simulation_trace_entries;
  std::vector<SimConsumerCheckpoint> checkpoints;
};

class SimConsumerConformanceError : public std::runtime_error {
public:
  explicit SimConsumerConformanceError(const std::string& what) : std::runtime_error(what) {}
};

class SimConsumerDivergenceError : public SimConsumerConformanceError {
public:
  SimConsumerDivergenceError(std::uint64_t step, std::string action_id,
                             std::string baseline_adapter_id, std::string adapter_id,
                             std::string kind, std::string observation_id = {},
                             std::int64_t expected_prefix_length = -1,
                             std::int64_t actual_prefix_length = -1)
      : SimConsumerConformanceError("consumer simulation divergence at step " +
                                    std::to_string(step) + " action '" + action_id + "' adapter '" +
                                    adapter_id + "': " + kind),
        step(step), action_id(std::move(action_id)),
        baseline_adapter_id(std::move(baseline_adapter_id)), adapter_id(std::move(adapter_id)),
        observation_id(std::move(observation_id)), kind(std::move(kind)),
        expected_prefix_length(expected_prefix_length), actual_prefix_length(actual_prefix_length) {
  }

  std::uint64_t step;
  std::string action_id;
  std::string baseline_adapter_id;
  std::string adapter_id;
  std::string observation_id;
  std::string kind;
  std::int64_t expected_prefix_length;
  std::int64_t actual_prefix_length;
};

namespace sim_consumer_detail {

[[noreturn]] inline void fail(const std::string& message) {
  throw SimConsumerConformanceError(message);
}

inline bool valid_id(const std::string& value) {
  if (value.empty() || value.size() > 128 || value.front() < 'a' || value.front() > 'z')
    return false;
  return std::all_of(value.begin() + 1, value.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == ':' ||
           c == '-';
  });
}

inline void require_id(const std::string& value, const std::string& name) {
  if (!valid_id(value)) fail(name + " must be a stable id");
}

inline bool is_real(SimConsumerAdapterKind kind) {
  return kind == SimConsumerAdapterKind::Postgres || kind == SimConsumerAdapterKind::Nats ||
         kind == SimConsumerAdapterKind::ExternalProcess;
}

inline bool valid_adapter_kind(SimConsumerAdapterKind kind) {
  return kind == SimConsumerAdapterKind::InMemory || kind == SimConsumerAdapterKind::Postgres ||
         kind == SimConsumerAdapterKind::Nats || kind == SimConsumerAdapterKind::ExternalProcess;
}

inline bool valid_determinism(SimConsumerPortDeterminism determinism) {
  return determinism == SimConsumerPortDeterminism::Deterministic ||
         determinism == SimConsumerPortDeterminism::Nondeterministic;
}

inline bool is_legacy_real(SimConsumerAdapterKind kind) {
  return kind == SimConsumerAdapterKind::Postgres || kind == SimConsumerAdapterKind::Nats;
}

inline bool valid_external_port(SimConsumerExternalPortKind port) {
  return port == SimConsumerExternalPortKind::Cli ||
         port == SimConsumerExternalPortKind::Filesystem ||
         port == SimConsumerExternalPortKind::LocalSocket ||
         port == SimConsumerExternalPortKind::EditorReplica;
}

inline const char* determinism_name(SimConsumerPortDeterminism value) {
  return value == SimConsumerPortDeterminism::Deterministic ? "deterministic" : "nondeterministic";
}

inline std::vector<std::string> port_contract(const std::vector<SimConsumerPort>& ports) {
  std::vector<std::string> result;
  result.reserve(ports.size());
  for (const auto& port : ports)
    result.push_back(port.id + '\0' + port.kind + '\0' + determinism_name(port.determinism));
  std::sort(result.begin(), result.end());
  return result;
}

inline ReplayValue action_value(const SimConsumerAction& action) {
  return ReplayValue::map({
      {ReplayValue::text("id"), ReplayValue::text(action.id)},
      {ReplayValue::text("actor_id"), ReplayValue::text(action.actor_id)},
      {ReplayValue::text("kind"), ReplayValue::text(action.kind)},
      {ReplayValue::text("version"), ReplayValue::text(action.version)},
      {ReplayValue::text("payload"), action.payload},
      {ReplayValue::text("cause_id"), ReplayValue::text(action.cause_id)},
  });
}

inline ReplayValue scenario_value(const SimConsumerGeneratedScenario& scenario) {
  std::vector<ReplayValue> actions;
  actions.reserve(scenario.actions.size());
  for (const auto& generated : scenario.actions) {
    actions.push_back(ReplayValue::map({
        {ReplayValue::text("command"), ReplayValue::text(generated.command)},
        {ReplayValue::text("action"), action_value(generated.action)},
    }));
  }
  std::vector<ReplayValue> labels;
  for (const auto& label : scenario.coverage_labels)
    labels.push_back(ReplayValue::text(label));
  return ReplayValue::map({
      {ReplayValue::text("generator_name"), ReplayValue::text(scenario.generator_name)},
      {ReplayValue::text("generator_version"), ReplayValue::text(scenario.generator_version)},
      {ReplayValue::text("scenario_index"), ReplayValue::integer(scenario.scenario_index)},
      {ReplayValue::text("seed_hex"), ReplayValue::text(scenario.seed_hex)},
      {ReplayValue::text("actions"), ReplayValue::seq(std::move(actions))},
      {ReplayValue::text("coverage_labels"), ReplayValue::seq(std::move(labels))},
  });
}

inline ReplayValue observation_value(const SimConsumerObservation& observation) {
  std::vector<std::pair<ReplayValue, ReplayValue>> entries;
  entries.reserve(observation.size());
  for (const auto& entry : observation)
    entries.emplace_back(ReplayValue::text(entry.first), entry.second);
  return ReplayValue::map(std::move(entries));
}

inline void validate_action(const SimConsumerAction& action) {
  require_id(action.id, "action id");
  require_id(action.actor_id, "action actor id");
  require_id(action.kind, "action kind");
  if (action.version.empty()) fail("action version must not be empty");
  if (!action.cause_id.empty()) require_id(action.cause_id, "action cause id");
  (void)canonical_bytes(action_value(action));
}

inline std::string validate_scenario(const SimConsumerGeneratedScenario& scenario) {
  require_id(scenario.generator_name, "scenario generator name");
  if (scenario.generator_version.empty()) fail("scenario generator version must not be empty");
  if (scenario.seed_hex.size() != 64 ||
      !std::all_of(scenario.seed_hex.begin(), scenario.seed_hex.end(),
                   [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
    fail("scenario seed must be 32 bytes of lowercase hexadecimal");
  if (scenario.actions.empty()) fail("scenario must contain at least one action");
  std::set<std::string> seen;
  for (const auto& generated : scenario.actions) {
    require_id(generated.command, "generator command");
    validate_action(generated.action);
    if (!seen.insert(generated.action.id).second)
      fail("scenario has duplicate action id '" + generated.action.id + "'");
    if (!generated.action.cause_id.empty() && seen.count(generated.action.cause_id) == 0)
      fail("scenario action has an unresolved cause");
  }
  return canonical_digest(scenario_value(scenario));
}

inline void validate_adapter(const SimConsumerAdapter& adapter) {
  if (!valid_adapter_kind(adapter.kind)) fail("adapter has an unknown kind");
  require_id(adapter.id, "adapter id");
  require_id(adapter.protocol_id, "adapter protocol id");
  require_id(adapter.reducer_id, "adapter reducer id");
  if (!adapter.reset || !adapter.apply || !adapter.observe)
    fail("adapter '" + adapter.id + "' needs reset, apply, and observe callbacks");
  if (adapter.ports.empty()) fail("adapter '" + adapter.id + "' needs narrow ports");
  std::set<std::string> seen_ports;
  for (const auto& port : adapter.ports) {
    require_id(port.id, "adapter port id");
    require_id(port.kind, "adapter port kind");
    if (!valid_determinism(port.determinism)) fail("adapter port has unknown determinism");
    if (!seen_ports.insert(port.id).second) fail("adapter has a duplicate port");
    if (port.stubbed && port.determinism != SimConsumerPortDeterminism::Nondeterministic)
      fail("adapter stubs a deterministic port");
    if (is_real(adapter.kind) && port.stubbed) fail("a real adapter cannot stub a port");
  }
  if (adapter.kind == SimConsumerAdapterKind::InMemory) {
    require_id(adapter.production_reducer_id, "in-memory production reducer id");
    if (!adapter.world_evidence) fail("in-memory adapter needs world evidence");
    if (!adapter.service_id.empty() || adapter.probe || adapter.materialized_history ||
        adapter.external_port != SimConsumerExternalPortKind::None)
      fail("in-memory adapter carries real-adapter evidence");
    return;
  }
  require_id(adapter.service_id, "real adapter service id");
  if (!adapter.probe || !adapter.materialized_history)
    fail("real adapter needs probe and materialized-history callbacks");
  if (adapter.world_evidence) fail("real adapter cannot expose world evidence");
  if (adapter.kind == SimConsumerAdapterKind::ExternalProcess) {
    if (!valid_external_port(adapter.external_port)) fail("external adapter needs a port");
    if (!adapter.production_reducer_id.empty())
      fail("external adapter cannot claim a production reducer");
  } else {
    require_id(adapter.production_reducer_id, "real adapter production reducer id");
    if (adapter.reducer_id != adapter.production_reducer_id)
      fail("real adapter reducer differs from its production reducer");
    if (adapter.external_port != SimConsumerExternalPortKind::None)
      fail("Postgres/NATS adapter cannot claim an external port");
  }
}

} // namespace sim_consumer_detail

class SimConsumerTestkit {
public:
  explicit SimConsumerTestkit(SimConsumerTestkitSpec spec) : adapters_(std::move(spec.adapters)) {
    using namespace sim_consumer_detail;
    require_id(spec.simulation_adapter_id, "simulation adapter id");
    if (spec.required_real_adapters.empty() && spec.required_external_processes.empty())
      fail("select at least one real adapter or external process");
    if (adapters_.size() < 2) fail("testkit needs in-memory and real adapters");

    std::set<SimConsumerAdapterKind> required;
    for (auto kind : spec.required_real_adapters) {
      if (!is_legacy_real(kind)) fail("required kind is not Postgres or NATS");
      if (!required.insert(kind).second) fail("duplicate required real kind");
    }
    std::map<std::string, SimConsumerExternalPortKind> required_external;
    for (const auto& selection : spec.required_external_processes) {
      require_id(selection.adapter_id, "external selection adapter id");
      if (!valid_external_port(selection.port)) fail("external selection needs a supported port");
      if (!required_external.emplace(selection.adapter_id, selection.port).second)
        fail("duplicate external selection");
    }

    std::sort(adapters_.begin(), adapters_.end(),
              [](const auto& left, const auto& right) { return left.id < right.id; });
    std::set<std::string> ids;
    std::set<SimConsumerAdapterKind> present;
    std::string protocol;
    std::string production_reducer;
    std::vector<std::string> ports;
    for (std::size_t index = 0; index < adapters_.size(); ++index) {
      const auto& adapter = adapters_[index];
      validate_adapter(adapter);
      if (!ids.insert(adapter.id).second) fail("duplicate adapter id '" + adapter.id + "'");
      present.insert(adapter.kind);
      if (is_legacy_real(adapter.kind) && required.count(adapter.kind) == 0)
        fail("real adapter was not explicitly selected");
      if (adapter.kind == SimConsumerAdapterKind::ExternalProcess) {
        const auto selected = required_external.find(adapter.id);
        if (selected == required_external.end()) fail("external adapter was not selected");
        if (selected->second != adapter.external_port) fail("external adapter selected wrong port");
      }
      if (required_external.count(adapter.id) != 0 &&
          adapter.kind != SimConsumerAdapterKind::ExternalProcess)
        fail("external selection refers to the wrong adapter kind");
      if (adapter.kind != SimConsumerAdapterKind::ExternalProcess) {
        if (production_reducer.empty()) production_reducer = adapter.production_reducer_id;
        if (production_reducer != adapter.production_reducer_id)
          fail("adapters do not share the production reducer");
      }
      if (protocol.empty()) protocol = adapter.protocol_id;
      if (protocol != adapter.protocol_id) fail("adapters do not share the protocol");
      const auto contract = port_contract(adapter.ports);
      if (ports.empty())
        ports = contract;
      else if (ports != contract)
        fail("adapters do not share the narrow-port contract");
      if (adapter.id == spec.simulation_adapter_id) baseline_index_ = index;
    }
    for (auto kind : required)
      if (present.count(kind) == 0) fail("required real adapter is missing");
    for (const auto& selection : required_external)
      if (ids.count(selection.first) == 0) fail("required external adapter is missing");
    if (baseline_index_ == adapters_.size()) fail("simulation adapter is missing");
    if (adapters_[baseline_index_].kind != SimConsumerAdapterKind::InMemory)
      fail("simulation adapter must be in-memory");
  }

  SimConsumerRunResult run(const SimConsumerGeneratedScenario& scenario) const {
    using namespace sim_consumer_detail;
    SimConsumerRunResult result;
    result.scenario_digest = validate_scenario(scenario);
    std::shared_ptr<SimConsumerWorldEvidence> reset_world;
    for (const auto& adapter : adapters_) {
      if (is_real(adapter.kind)) adapter.probe();
      adapter.reset();
      if (adapter.kind == SimConsumerAdapterKind::InMemory) {
        reset_world = adapter.world_evidence();
        if (!reset_world) fail("reset did not create simulation world evidence");
      } else {
        validate_history(adapter, {}, 0, "");
      }
      result.adapter_ids.push_back(adapter.id);
      result.adapter_evidence.push_back({adapter.id, adapter.kind, adapter.service_id,
                                         adapter.external_port, adapter.protocol_id,
                                         adapter.reducer_id, adapter.production_reducer_id});
    }

    for (std::size_t action_index = 0; action_index < scenario.actions.size(); ++action_index) {
      const auto& generated = scenario.actions[action_index];
      const auto step = static_cast<std::uint64_t>(action_index + 1);
      std::vector<SimConsumerObservation> observations(adapters_.size());
      SimConsumerCheckpoint checkpoint{step, generated.action.id, {}};
      for (std::size_t adapter_index = 0; adapter_index < adapters_.size(); ++adapter_index) {
        const auto& adapter = adapters_[adapter_index];
        std::shared_ptr<SimConsumerWorldEvidence> world;
        std::uint64_t steps_before = 0;
        std::size_t trace_before = 0;
        if (adapter.kind == SimConsumerAdapterKind::InMemory) {
          world = adapter.world_evidence();
          if (world != reset_world) throw bypass(step, generated.action.id, adapter.id);
          steps_before = world->step_count();
          trace_before = world->trace_entries().size();
        }
        adapter.apply(generated.action);
        if (world) {
          const auto current = adapter.world_evidence();
          bool traced = false;
          if (current == world && current->identity() == world->identity() &&
              current->step_count() > steps_before) {
            const auto& entries = current->trace_entries();
            for (std::size_t index = trace_before; index < entries.size(); ++index)
              if (entries[index].action_id == generated.action.id &&
                  entries[index].kind.rfind("action_", 0) == 0)
                traced = true;
          }
          if (!traced) throw bypass(step, generated.action.id, adapter.id);
        } else {
          std::vector<SimConsumerAction> expected;
          for (std::size_t index = 0; index <= action_index; ++index)
            expected.push_back(scenario.actions[index].action);
          validate_history(adapter, expected, step, generated.action.id);
        }
        observations[adapter_index] = adapter.observe();
        if (observations[adapter_index].empty()) fail("adapter returned no observations");
        for (const auto& entry : observations[adapter_index])
          require_id(entry.first, "observation id");
        checkpoint.observation_digests.emplace(
            adapter.id, canonical_digest(observation_value(observations[adapter_index])));
      }
      for (std::size_t adapter_index = 0; adapter_index < adapters_.size(); ++adapter_index)
        if (adapter_index != baseline_index_)
          compare_observations(step, generated.action.id, adapters_[adapter_index].id,
                               observations[baseline_index_], observations[adapter_index]);
      result.checkpoints.push_back(std::move(checkpoint));
    }

    const auto world = adapters_[baseline_index_].world_evidence();
    result.simulation_world_identity = world->identity();
    result.simulation_world_step_count = world->step_count();
    result.simulation_trace_entries = world->trace_entries();
    return result;
  }

private:
  SimConsumerDivergenceError bypass(std::uint64_t step, const std::string& action_id,
                                    const std::string& adapter_id) const {
    return {step, action_id, adapters_[baseline_index_].id, adapter_id, "simulation_world_bypass"};
  }

  void validate_history(const SimConsumerAdapter& adapter,
                        const std::vector<SimConsumerAction>& expected, std::uint64_t step,
                        const std::string& action_id) const {
    const auto history = adapter.materialized_history();
    if (history.size() != expected.size())
      throw SimConsumerDivergenceError(step, action_id, adapters_[baseline_index_].id, adapter.id,
                                       "materialized_history_mismatch", "",
                                       static_cast<std::int64_t>(expected.size()),
                                       static_cast<std::int64_t>(history.size()));
    for (std::size_t index = 0; index < expected.size(); ++index)
      if (canonical_bytes(sim_consumer_detail::action_value(history[index])) !=
          canonical_bytes(sim_consumer_detail::action_value(expected[index])))
        throw SimConsumerDivergenceError(step, action_id, adapters_[baseline_index_].id, adapter.id,
                                         "materialized_history_mismatch", "",
                                         static_cast<std::int64_t>(expected.size()),
                                         static_cast<std::int64_t>(history.size()));
  }

  void compare_observations(std::uint64_t step, const std::string& action_id,
                            const std::string& adapter_id, const SimConsumerObservation& baseline,
                            const SimConsumerObservation& actual) const {
    if (baseline.size() != actual.size())
      throw SimConsumerDivergenceError(step, action_id, adapters_[baseline_index_].id, adapter_id,
                                       "observation_count");
    for (const auto& entry : baseline) {
      const auto found = actual.find(entry.first);
      if (found == actual.end())
        throw SimConsumerDivergenceError(step, action_id, adapters_[baseline_index_].id, adapter_id,
                                         "missing_observation", entry.first);
      if (canonical_bytes(entry.second) != canonical_bytes(found->second))
        throw SimConsumerDivergenceError(step, action_id, adapters_[baseline_index_].id, adapter_id,
                                         "observation_divergence", entry.first);
    }
  }

  std::vector<SimConsumerAdapter> adapters_;
  std::size_t baseline_index_ = adapters_.size();
};

} // namespace lazily

#endif // LAZILY_SIM_CONSUMER_TESTKIT_HPP
