// Canonical generic effect-CAS corpus replay (`#lzeffectcas`), plus the core
// invariants the corpus cannot name.
//
// Mirrors lazily-rs tests/effect_cas_conformance.rs and the `#[cfg(test)]` tail
// of src/effect_cas_core.rs. `EffectCasCore` is pure -- no graph, no clock, no
// I/O -- so there is exactly one flavour and the replay drives the core itself
// through its public API.
//
// Kept from reporting green while testing nothing by:
//   * every scenario is booked through `scenario_views(...).replay()`, so the
//     coverage guard compares declared against REPLAYED scenarios;
//   * every step's `returns` is compared as a whole object (key set and
//     values), and every scenario's `final` likewise, against values rendered
//     from what the LIBRARY returned and what its snapshot projects;
//   * the executed step count must equal the loaded step count, and an op this
//     runner does not know aborts rather than being skipped;
//   * `superseded` steps assert the fixture's own zero-mutation witness
//     (`backend_before == backend_after`).

#include <lazily/effect_cas_core.hpp>

#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "test_json.hpp"
#include "test_require.hpp"
#include "test_spec_fixture.hpp"

using lazily_test::Json;
using lazily_test::json_string;
using lazily_test::json_u64;
using lazily_test::JsonParser;
using lazily_test::JsonPtr;

using lazily::EffectAttemptIdentity;
using lazily::EffectCasApplied;
using lazily::EffectCasAppliedKind;
using lazily::EffectCasClaimKind;
using lazily::EffectCasFailure;
using lazily::EffectCasFailureKind;
using lazily::EffectCasReconnect;
using lazily::EffectCasReconnectKind;
using lazily::EffectCasUpsert;
using lazily::EffectCasUpsertKind;
using lazily::EffectIdentity;
using lazily::EffectIntent;
using lazily::EffectPlan;
using lazily::EffectTerminalKind;

namespace {

constexpr const char* kArea = "egress";
constexpr const char* kFixture = "effect_cas.json";

using Core =
    lazily::EffectCasCore<std::string, std::uint64_t, std::string, std::string, std::string>;

const Json& member(const Json& object, const std::string& key, const std::string& where) {
  const Json* found = object.find(key);
  REQUIRE(found != nullptr, where + ": missing JSON member `" + key + "`");
  return *found;
}

// ── Canonical rendering: a value rendered as text, objects as sorted maps. ──

using Flat = std::map<std::string, std::string>;

std::string str_text(const std::string& value) { return "\"" + value + "\""; }
std::string u64_text(std::uint64_t value) { return std::to_string(value); }
template <typename T> std::string opt_u64_text(const std::optional<T>& value) {
  return value ? u64_text(static_cast<std::uint64_t>(*value)) : "null";
}

std::string scalar_text(const Json& value, const std::string& where) {
  switch (value.type) {
  case Json::Type::Null:
    return "null";
  case Json::Type::Number:
    return u64_text(json_u64(value));
  case Json::Type::String:
    return str_text(json_string(value));
  default:
    REQUIRE(false, where + ": expected a scalar, got " + lazily_test::json_debug(value));
  }
  return {};
}

Flat flat_object(const Json& value, const std::string& where) {
  REQUIRE(value.is_object(), where + ": expected an object");
  Flat out;
  for (const auto& entry : value.object)
    out[entry.first] = scalar_text(*entry.second, where + "." + entry.first);
  return out;
}

void require_flat(const Flat& got, const Json& want, const std::string& where) {
  const Flat expected = flat_object(want, where);
  if (got == expected) return;
  std::string detail;
  for (const auto& entry : expected) {
    const auto it = got.find(entry.first);
    detail +=
        " " + entry.first + "=" + entry.second + "/" + (it == got.end() ? "<absent>" : it->second);
  }
  for (const auto& entry : got)
    if (expected.count(entry.first) == 0) detail += " +" + entry.first + "=" + entry.second;
  REQUIRE(false, where + ": disagreed (fixture/library):" + detail);
}

Flat render(const EffectCasUpsert& outcome) {
  switch (outcome.kind) {
  case EffectCasUpsertKind::Accepted:
    return {{"upsert", str_text("accepted")}};
  case EffectCasUpsertKind::Unchanged:
    return {{"upsert", str_text("unchanged")}};
  case EffectCasUpsertKind::AlreadyApplied:
    return {{"upsert", str_text("already_applied")},
            {"applied_through", u64_text(outcome.applied_through)}};
  case EffectCasUpsertKind::StaleEpoch:
    return {{"upsert", str_text("stale_epoch")}, {"current", u64_text(outcome.current)}};
  case EffectCasUpsertKind::EpochConflict:
    return {{"upsert", str_text("epoch_conflict")}};
  }
  return {};
}

Flat render(const EffectCasApplied& outcome) {
  switch (outcome.kind) {
  case EffectCasAppliedKind::StepApplied:
    return {{"applied", str_text("step_applied")}, {"next_step", u64_text(outcome.next_step)}};
  case EffectCasAppliedKind::Completed:
    return {{"applied", str_text("completed")},
            {"applied_through", u64_text(outcome.applied_through)}};
  case EffectCasAppliedKind::Superseded:
    return {{"applied", str_text("superseded")}};
  case EffectCasAppliedKind::Duplicate:
    return {{"applied", str_text("duplicate")}};
  case EffectCasAppliedKind::UnknownAttempt:
    return {{"applied", str_text("unknown_attempt")}};
  case EffectCasAppliedKind::StaleGeneration:
    return {{"applied", str_text("stale_generation")}, {"current", u64_text(outcome.current)}};
  }
  return {};
}

Flat render(const EffectCasFailure& outcome) {
  switch (outcome.kind) {
  case EffectCasFailureKind::Superseded:
    return {{"failure", str_text("superseded")}};
  case EffectCasFailureKind::Retrying:
    return {{"failure", str_text("retrying")}, {"retry_at", u64_text(outcome.retry_at)}};
  case EffectCasFailureKind::Rejected:
    return {{"failure", str_text("rejected")}};
  case EffectCasFailureKind::UnknownAttempt:
    return {{"failure", str_text("unknown_attempt")}};
  case EffectCasFailureKind::StaleGeneration:
    return {{"failure", str_text("stale_generation")}, {"current", u64_text(outcome.current)}};
  }
  return {};
}

Flat render(const EffectCasReconnect& outcome) {
  switch (outcome.kind) {
  case EffectCasReconnectKind::Advanced:
    return {{"reconnect", str_text("advanced")},
            {"generation", u64_text(outcome.generation)},
            {"requeued", u64_text(outcome.requeued)},
            {"superseded", u64_text(outcome.superseded)}};
  case EffectCasReconnectKind::Unchanged:
    return {{"reconnect", str_text("unchanged")}, {"generation", u64_text(outcome.generation)}};
  case EffectCasReconnectKind::StaleGeneration:
    return {{"reconnect", str_text("stale_generation")}, {"current", u64_text(outcome.current)}};
  }
  return {};
}

std::string terminal_text(EffectTerminalKind kind) {
  switch (kind) {
  case EffectTerminalKind::Applied:
    return str_text("applied");
  case EffectTerminalKind::Superseded:
    return str_text("superseded");
  case EffectTerminalKind::Rejected:
    return str_text("rejected");
  }
  return "null";
}

/// The scenario's `final` block, projected from the LIBRARY's snapshot.
std::map<std::string, Flat> final_state(const Core& core) {
  std::map<std::string, Flat> out;
  for (const auto& entry : core.snapshot().keys) {
    const auto& state = entry.second;
    Flat flat;
    flat["pending_identity"] =
        state.pending ? str_text(state.pending->intent.identity.str()) : "null";
    flat["inflight_identity"] =
        state.inflight ? str_text(state.inflight->identity.effect.str()) : "null";
    flat["applied_through"] = opt_u64_text(state.applied_through);
    flat["terminal"] = state.terminal ? terminal_text(state.terminal->kind) : "null";
    flat["receipt_count"] = u64_text(state.terminal ? state.terminal->receipts.size() : 0);
    flat["retry_at"] = state.retry ? u64_text(state.retry->retry_at) : std::string("null");
    out.emplace(entry.first, std::move(flat));
  }
  return out;
}

Flat run_step(Core& core, std::map<std::string, EffectAttemptIdentity>& attempts, const Json& step,
              const std::string& where) {
  const Json& op = member(step, "op", where);
  const std::string type = json_string(member(op, "type", where));
  const auto key = [&] { return json_string(member(op, "key", where)); };
  const auto attempt = [&]() -> EffectAttemptIdentity {
    const std::string label = json_string(member(op, "attempt", where));
    const auto it = attempts.find(label);
    REQUIRE(it != attempts.end(), where + ": no saved attempt `" + label + "`");
    return it->second;
  };
  REQUIRE(type == "claim" || step.find("save") == nullptr,
          where + ": only a claim can save an attempt label");

  if (type == "upsert") {
    std::vector<std::string> plan;
    for (const auto& payload : member(op, "plan", where).array)
      plan.push_back(json_string(*payload));
    EffectIntent<std::string, std::uint64_t, std::string> intent{
        EffectIdentity(json_string(member(op, "identity", where))),
        json_u64(member(op, "epoch", where)), key(), json_u64(member(op, "expected", where)),
        EffectPlan<std::string>(std::move(plan))};
    return render(core.upsert_desired(std::move(intent)).second);
  }
  if (type == "claim") {
    const auto outcome = core.claim(key(), json_u64(member(op, "generation", where)),
                                    json_u64(member(op, "now", where)))
                             .second;
    Flat flat;
    switch (outcome.kind) {
    case EffectCasClaimKind::Claimed: {
      const auto& a = *outcome.attempt;
      flat = {{"claim", str_text("claimed")},      {"identity", str_text(a.identity.effect.str())},
              {"step", u64_text(a.identity.step)}, {"attempt", u64_text(a.identity.attempt)},
              {"expected", u64_text(a.expected)},  {"payload", str_text(a.payload)}};
      break;
    }
    case EffectCasClaimKind::Empty:
      flat = {{"claim", str_text("empty")}};
      break;
    case EffectCasClaimKind::Busy:
      flat = {{"claim", str_text("busy")}};
      break;
    case EffectCasClaimKind::Waiting:
      flat = {{"claim", str_text("waiting")}, {"retry_at", u64_text(outcome.retry_at)}};
      break;
    case EffectCasClaimKind::StaleGeneration:
      flat = {{"claim", str_text("stale_generation")}, {"current", u64_text(outcome.current)}};
      break;
    }
    if (const Json* save = step.find("save")) {
      REQUIRE(outcome.kind == EffectCasClaimKind::Claimed, where + ": a saved claim must succeed");
      attempts.insert_or_assign(json_string(*save), outcome.attempt->identity);
    }
    return flat;
  }
  if (type == "applied")
    return render(core.record_applied(key(), attempt(), json_string(member(op, "receipt", where)),
                                      json_u64(member(op, "authority", where)))
                      .second);
  if (type == "superseded") {
    // The fixture's own zero-mutation witness for the backend.
    REQUIRE(json_u64(member(op, "backend_before", where)) ==
                json_u64(member(op, "backend_after", where)),
            where + ": a superseded CAS must leave the backend unmutated");
    return render(
        core.record_superseded(key(), attempt(), json_u64(member(op, "observed", where))).second);
  }
  if (type == "retryable")
    return render(core.record_retryable(key(), attempt(), json_string(member(op, "error", where)),
                                        json_u64(member(op, "retry_at", where)))
                      .second);
  if (type == "rejected")
    return render(
        core.record_rejected(key(), attempt(), json_string(member(op, "error", where))).second);
  if (type == "reconnect")
    return render(core.reconnect(json_u64(member(op, "generation", where))).second);
  REQUIRE(false, where + ": unknown effect-CAS op `" + type + "`");
  return {};
}

/// Replay the whole fixture. Returns {scenarios, steps} EXECUTED.
std::pair<std::size_t, std::size_t> replay_corpus() {
  const std::string text = lazily_test::spec_fixture_text(kArea, kFixture);
  JsonParser parser(text);
  const JsonPtr fixture = parser.parse();
  REQUIRE(fixture && fixture->is_object(), "effect_cas.json is not a JSON object");
  REQUIRE(json_string(member(*fixture, "kind", kFixture)) == "EffectCas", "fixture kind");
  REQUIRE(json_string(member(*fixture, "model", kFixture)) == "EffectCasCore", "fixture model");
  const auto& scenarios = member(*fixture, "scenarios", kFixture).array;
  REQUIRE(!scenarios.empty(), "effect_cas.json declares no scenarios -- a vacuous replay");

  std::size_t loaded_steps = 0;
  for (const auto& scenario : scenarios)
    loaded_steps += member(*scenario, "steps", kFixture).array.size();

  std::size_t scenario_count = 0;
  std::size_t step_count = 0;
  const std::string fixture_id = std::string(kArea) + "/" + kFixture;
  for (const auto& view : lazily_test::scenario_views(fixture_id, scenarios)) {
    const Json& scenario = view.replay();
    const std::string label = fixture_id + " " + view.id();
    Core core(json_u64(member(scenario, "generation", label)));
    std::map<std::string, EffectAttemptIdentity> attempts;
    const auto& steps = member(scenario, "steps", label).array;
    REQUIRE(!steps.empty(), label + ": a scenario with no steps is a vacuous replay");
    for (std::size_t index = 0; index < steps.size(); ++index) {
      const std::string where = label + " step " + std::to_string(index);
      const Flat got = run_step(core, attempts, *steps[index], where);
      require_flat(got, member(*steps[index], "returns", where), where + " returns");
      ++step_count;
    }
    const Json& want_final = member(scenario, "final", label);
    REQUIRE(want_final.is_object(), label + ": `final` must be an object");
    const auto got_final = final_state(core);
    std::set<std::string> want_keys;
    for (const auto& entry : want_final.object) {
      want_keys.insert(entry.first);
      const auto it = got_final.find(entry.first);
      REQUIRE(it != got_final.end(),
              label + ": final names key `" + entry.first + "` the core never saw");
      require_flat(it->second, *entry.second, label + " final." + entry.first);
    }
    std::set<std::string> got_keys;
    for (const auto& entry : got_final)
      got_keys.insert(entry.first);
    REQUIRE(got_keys == want_keys, label + ": the core holds keys the fixture's final omits");
    ++scenario_count;
  }
  REQUIRE(step_count == loaded_steps, "every loaded effect-CAS step must be executed");
  REQUIRE(scenario_count == scenarios.size(), "every effect-CAS scenario must be executed");
  return {scenario_count, step_count};
}

// ── Core invariants (lazily-rs src/effect_cas_core.rs #[cfg(test)]) ─────────

using SCore =
    lazily::EffectCasCore<std::string, std::uint64_t, std::string, std::string, std::string>;
using SIntent = EffectIntent<std::string, std::uint64_t, std::string>;

SIntent intent(const std::string& id, std::uint64_t epoch, const std::string& key,
               std::uint64_t expected, std::vector<std::string> steps) {
  return SIntent{EffectIdentity(id), epoch, key, expected,
                 EffectPlan<std::string>(std::move(steps))};
}

lazily::EffectAttempt<std::string, std::uint64_t, std::string, std::string>
must_claim(SCore& core, const std::string& key, std::uint64_t generation, std::uint64_t now) {
  auto outcome = core.claim(key, generation, now).second;
  REQUIRE(outcome.kind == EffectCasClaimKind::Claimed, "expected a claim");
  return *outcome.attempt;
}

void ordered_plan_advances_from_receipt_authority_and_dedupes_terminal_receipt() {
  SCore core(7);
  core.upsert_desired(intent("effect-1", 1, "doc", 10, {"canonical", "snapshot"}));
  const auto first = must_claim(core, "doc", 7, 0);
  REQUIRE(first.expected == 10, "first step compares against the intent's authority");
  REQUIRE(core.record_applied("doc", first.identity, "r1", 11).second.next_step == 1,
          "step 0 advances the cursor");
  const auto second = must_claim(core, "doc", 7, 0);
  REQUIRE(second.expected == 11 && second.payload == "snapshot",
          "step 1 compares against step 0's receipt authority");
  REQUIRE(second.prior_receipts.size() == 1, "step 1 carries step 0's receipt as lineage");
  const auto done = core.record_applied("doc", second.identity, "r2", 12).second;
  REQUIRE(done.kind == EffectCasAppliedKind::Completed && done.applied_through == 1, "completed");
  REQUIRE(core.record_applied("doc", second.identity, "r2", 12).second.kind ==
              EffectCasAppliedKind::Duplicate,
          "the exact terminal receipt is a duplicate");
}

void retry_uses_logical_clock_and_newer_desire_wins() {
  SCore core(1);
  core.upsert_desired(intent("old", 1, "doc", 1, {"write"}));
  const auto old = must_claim(core, "doc", 1, 0);
  const auto retrying = core.record_retryable("doc", old.identity, "busy", 9).second;
  REQUIRE(retrying.kind == EffectCasFailureKind::Retrying && retrying.retry_at == 9, "retrying");
  const auto waiting = core.claim("doc", 1, 8).second;
  REQUIRE(waiting.kind == EffectCasClaimKind::Waiting && waiting.retry_at == 9,
          "the logical clock gates the retry");
  REQUIRE(core.pending_keys(8).empty() && core.pending_keys(9).size() == 1,
          "pending_keys honours ready_at");
  core.upsert_desired(intent("new", 2, "doc", 2, {"write"}));
  REQUIRE(must_claim(core, "doc", 1, 8).identity.effect.str() == "new",
          "a newer desire replaces the waiting retry immediately");
}

void stale_attempts_and_authority_mismatch_never_advance_frontier() {
  SCore core(2);
  core.upsert_desired(intent("effect", 4, "doc", 8, {"write"}));
  const auto attempt = must_claim(core, "doc", 2, 0);
  REQUIRE(core.record_superseded("doc", attempt.identity, 9).second.kind ==
              EffectCasFailureKind::Superseded,
          "superseded");
  REQUIRE(!core.state("doc")->applied_through.has_value(), "no frontier after a mismatch");
  REQUIRE(core.record_applied("doc", attempt.identity, "late", 10).second.kind ==
              EffectCasAppliedKind::UnknownAttempt,
          "a late receipt for a finished attempt is unknown");
  REQUIRE(!core.state("doc")->applied_through.has_value(), "still no frontier");
}

void reconnect_requeues_same_identity_and_fences_old_generation() {
  SCore core(3);
  core.upsert_desired(intent("effect", 1, "doc", 1, {"write"}));
  const auto first = must_claim(core, "doc", 3, 0);
  const auto reconnect = core.reconnect(4).second;
  REQUIRE(reconnect.kind == EffectCasReconnectKind::Advanced && reconnect.requeued == 1 &&
              reconnect.superseded == 0,
          "reconnect requeues the in-flight attempt");
  const auto late = core.record_applied("doc", first.identity, "late", 2).second;
  REQUIRE(late.kind == EffectCasAppliedKind::StaleGeneration && late.current == 4,
          "the old generation is fenced");
  const auto replay = must_claim(core, "doc", 4, 0);
  REQUIRE(replay.identity.effect == first.identity.effect && replay.identity.attempt == 2,
          "the replay keeps the effect identity and bumps the attempt");
  REQUIRE(core.reconnect(4).second.kind == EffectCasReconnectKind::Unchanged, "same generation");
  REQUIRE(core.reconnect(3).second.kind == EffectCasReconnectKind::StaleGeneration,
          "an older generation is stale");
}

void upsert_epoch_rules() {
  SCore core(1);
  REQUIRE(core.upsert_desired(intent("a", 2, "doc", 0, {"w"})).second.kind ==
              EffectCasUpsertKind::Accepted,
          "accepted");
  REQUIRE(core.upsert_desired(intent("a", 2, "doc", 0, {"w"})).second.kind ==
              EffectCasUpsertKind::Unchanged,
          "an identical intent at the same epoch is unchanged");
  REQUIRE(core.upsert_desired(intent("b", 2, "doc", 0, {"w"})).second.kind ==
              EffectCasUpsertKind::EpochConflict,
          "a different intent at the same epoch conflicts");
  const auto stale = core.upsert_desired(intent("c", 1, "doc", 0, {"w"})).second;
  REQUIRE(stale.kind == EffectCasUpsertKind::StaleEpoch && stale.current == 2, "stale epoch");
  const auto a = must_claim(core, "doc", 1, 0);
  core.record_applied("doc", a.identity, "r", 1);
  const auto already = core.upsert_desired(intent("d", 2, "doc", 0, {"w"})).second;
  REQUIRE(already.kind == EffectCasUpsertKind::AlreadyApplied && already.applied_through == 2,
          "an epoch at or below the frontier is already applied");
  bool refused = false;
  try {
    EffectPlan<std::string> empty(std::vector<std::string>{});
  } catch (const std::invalid_argument&) {
    refused = true;
  }
  REQUIRE(refused, "an empty plan is refused");
}

} // namespace

int main() {
  ordered_plan_advances_from_receipt_authority_and_dedupes_terminal_receipt();
  retry_uses_logical_clock_and_newer_desire_wins();
  stale_attempts_and_authority_mismatch_never_advance_frontier();
  reconnect_requeues_same_identity_and_fences_old_generation();
  upsert_epoch_rules();

  const auto executed = replay_corpus();
  REQUIRE(executed.first > 0 && executed.second > 0, "the effect-CAS replay executed nothing");

  REQUIRE_FIXTURES_LOADED(1);
  std::cout << "effect-CAS: " << executed.first << " scenarios, " << executed.second
            << " steps executed; 5 core invariants OK" << std::endl;
  return 0;
}
