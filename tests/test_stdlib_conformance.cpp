#include <lazily/stdlib.hpp>

#include "test_json.hpp"
#include "test_spec_fixture.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <string>

namespace {

using lazily_test::Json;
using lazily_test::JsonPtr;

const Json &required(const Json &object, const std::string &key) {
  const auto *value = object.find(key);
  REQUIRE(value != nullptr, "missing fixture field");
  return *value;
}

std::string string_field(const Json &value, const std::string &key) {
  return required(value, key).as_str();
}

std::uint64_t u64_field(const Json &value, const std::string &key) {
  return lazily_test::json_u64(required(value, key));
}

bool bool_field(const Json &value, const std::string &key) {
  return required(value, key).as_bool();
}

std::optional<std::uint64_t> optional_u64_field(const Json &value,
                                                const std::string &key) {
  return lazily_test::json_optional_u64(required(value, key));
}

struct Actual {
  std::string outcome;
  std::optional<std::uint64_t> deadline;
  std::optional<std::uint64_t> fired_at;
  std::optional<std::string> reason;
  std::optional<std::string> value;
  std::optional<std::uint64_t> operation_calls;
  std::optional<std::uint64_t> cancellation_calls;
  std::optional<std::uint64_t> revision;
  std::optional<std::uint64_t> generation;
};

void assert_expected(const Json &expected, const Actual &actual) {
  for (const auto &[key, value] : expected.object) {
    if (key == "outcome") {
      REQUIRE(actual.outcome == value->as_str(), "outcome mismatch");
    } else if (key == "deadline") {
      REQUIRE(actual.deadline &&
                  *actual.deadline == lazily_test::json_u64(*value),
              "deadline mismatch");
    } else if (key == "fired_at") {
      REQUIRE(actual.fired_at &&
                  *actual.fired_at == lazily_test::json_u64(*value),
              "fired_at mismatch");
    } else if (key == "reason") {
      REQUIRE(actual.reason && *actual.reason == value->as_str(),
              "reason mismatch");
    } else if (key == "value") {
      REQUIRE(actual.value && *actual.value == value->as_str(),
              "value mismatch");
    } else if (key == "operation_calls") {
      REQUIRE(actual.operation_calls &&
                  *actual.operation_calls == lazily_test::json_u64(*value),
              "operation_calls mismatch");
    } else if (key == "cancellation_calls") {
      REQUIRE(actual.cancellation_calls &&
                  *actual.cancellation_calls == lazily_test::json_u64(*value),
              "cancellation_calls mismatch");
    } else if (key == "revision") {
      REQUIRE(actual.revision &&
                  *actual.revision == lazily_test::json_u64(*value),
              "revision mismatch");
    } else if (key == "generation") {
      REQUIRE(actual.generation &&
                  *actual.generation == lazily_test::json_u64(*value),
              "generation mismatch");
    } else {
      REQUIRE(false, "unknown stdlib expectation field");
    }
  }
}

std::string timer_error(lazily::TimerError error) {
  return error == lazily::TimerError::deadline_overflow ? "deadline_overflow"
                                                        : "clock_regression";
}

std::string timer_outcome(lazily::TimerObservation::Outcome outcome) {
  switch (outcome) {
  case lazily::TimerObservation::Outcome::pending:
    return "pending";
  case lazily::TimerObservation::Outcome::fired:
    return "fired";
  case lazily::TimerObservation::Outcome::unavailable:
    return "unavailable";
  }
  return "unavailable";
}

void replay_timer(const Json &scenario) {
  std::unique_ptr<lazily::Timer> timer;
  for (const auto &step_ptr : required(scenario, "steps").array) {
    const auto &step = *step_ptr;
    Actual actual;
    const auto op = string_field(step, "op");
    if (op == "start") {
      auto started = lazily::Timer::start(u64_field(step, "now"),
                                          u64_field(step, "duration"));
      timer = std::move(started.first);
      if (started.second) {
        actual = {"unavailable", std::nullopt, std::nullopt,
                  timer_error(*started.second)};
      } else {
        const auto deadline = lazily::checked_deadline(
            u64_field(step, "now"), u64_field(step, "duration"));
        actual = {"pending", deadline.value};
      }
    } else {
      REQUIRE(timer != nullptr, "timer observe before start");
      const auto observation = timer->observe(u64_field(step, "now"));
      actual.outcome = timer_outcome(observation.outcome);
      if (observation.outcome == lazily::TimerObservation::Outcome::pending ||
          observation.outcome == lazily::TimerObservation::Outcome::unavailable)
        actual.deadline = observation.deadline;
      if (observation.outcome == lazily::TimerObservation::Outcome::fired)
        actual.fired_at = observation.fired_at;
      if (observation.error)
        actual.reason = timer_error(*observation.error);
    }
    assert_expected(required(step, "expect"), actual);
  }
}

using TimeoutObservation = lazily::TimeoutObservation<std::string>;

std::string timeout_outcome(TimeoutObservation::Outcome outcome) {
  switch (outcome) {
  case TimeoutObservation::Outcome::pending:
    return "pending";
  case TimeoutObservation::Outcome::completed:
    return "completed";
  case TimeoutObservation::Outcome::timed_out:
    return "timed_out";
  case TimeoutObservation::Outcome::cancelled:
    return "cancelled";
  case TimeoutObservation::Outcome::unavailable:
    return "unavailable";
  }
  return "unavailable";
}

lazily::TimeoutCancellation cancellation(const std::string &value) {
  if (value == "cancelled")
    return lazily::TimeoutCancellation::cancelled;
  if (value == "unavailable")
    return lazily::TimeoutCancellation::unavailable;
  return lazily::TimeoutCancellation::pending;
}

void replay_timeout(const Json &scenario) {
  std::unique_ptr<lazily::Timeout<std::string>> timeout;
  for (const auto &step_ptr : required(scenario, "steps").array) {
    const auto &step = *step_ptr;
    Actual actual;
    const auto op = string_field(step, "op");
    if (op == "start") {
      auto started = lazily::Timeout<std::string>::start(
          u64_field(step, "now"), u64_field(step, "duration"));
      REQUIRE(!started.second, "timeout start overflow");
      timeout = std::move(started.first);
      actual = {"pending", lazily::checked_deadline(u64_field(step, "now"),
                                                    u64_field(step, "duration"))
                               .value};
    } else {
      REQUIRE(timeout != nullptr, "timeout poll before start");
      std::uint64_t operation_calls = 0;
      std::uint64_t cancellation_calls = 0;
      const auto operation = string_field(step, "operation");
      const auto cancellation_state = string_field(step, "cancellation");
      const auto observation = timeout->poll(
          u64_field(step, "now"),
          [&] {
            ++operation_calls;
            if (operation == "completed")
              return lazily::TimeoutOperation<std::string>::completed(
                  string_field(step, "value"));
            if (operation == "unavailable")
              return lazily::TimeoutOperation<std::string>::unavailable();
            return lazily::TimeoutOperation<std::string>::pending();
          },
          [&] {
            ++cancellation_calls;
            return cancellation(cancellation_state);
          });
      actual.outcome = timeout_outcome(observation.outcome);
      actual.operation_calls = operation_calls;
      actual.cancellation_calls = cancellation_calls;
      if (observation.outcome == TimeoutObservation::Outcome::pending)
        actual.deadline = observation.deadline;
      if (observation.outcome == TimeoutObservation::Outcome::completed)
        actual.value = observation.value;
      if (!observation.reason.empty())
        actual.reason = observation.reason;
    }
    assert_expected(required(step, "expect"), actual);
  }
}

std::string
barrier_outcome(lazily::RevisionBarrierObservation::Outcome outcome) {
  using Outcome = lazily::RevisionBarrierObservation::Outcome;
  switch (outcome) {
  case Outcome::pending:
    return "pending";
  case Outcome::satisfied:
    return "satisfied";
  case Outcome::timed_out:
    return "timed_out";
  case Outcome::cancelled:
    return "cancelled";
  case Outcome::disposed:
    return "disposed";
  case Outcome::unavailable:
    return "unavailable";
  }
  return "unavailable";
}

Actual barrier_actual(const lazily::RevisionBarrierObservation &observation) {
  Actual actual;
  actual.outcome = barrier_outcome(observation.outcome);
  actual.revision = observation.revision;
  actual.generation = observation.generation;
  if (!observation.reason.empty())
    actual.reason = observation.reason;
  return actual;
}

void replay_barrier(const Json &scenario) {
  std::unique_ptr<lazily::RevisionBarrier> barrier;
  for (const auto &step_ptr : required(scenario, "steps").array) {
    const auto &step = *step_ptr;
    const auto op = string_field(step, "op");
    lazily::RevisionBarrierObservation observation;
    std::uint64_t cancellation_calls = 0;
    if (op == "start") {
      barrier = std::make_unique<lazily::RevisionBarrier>(
          u64_field(step, "revision"), u64_field(step, "required_revision"),
          optional_u64_field(step, "deadline"));
      observation = barrier->receipt("");
    } else if (op == "observe") {
      REQUIRE(barrier != nullptr, "barrier observe before start");
      observation = barrier->observe(
          u64_field(step, "now"), bool_field(step, "predicate"), [&] {
            ++cancellation_calls;
            return cancellation(string_field(step, "cancellation"));
          });
    } else if (op == "register_recheck") {
      observation = barrier->register_recheck(
          u64_field(step, "now"), u64_field(step, "observed_revision"),
          bool_field(step, "predicate"));
    } else if (op == "advance") {
      observation = barrier->advance(u64_field(step, "revision"),
                                     bool_field(step, "predicate"));
    } else if (op == "dispose") {
      observation = barrier->dispose();
    } else if (op == "receipt") {
      observation = barrier->receipt(string_field(step, "key"));
    } else {
      REQUIRE(false, "unknown barrier op");
    }
    auto actual = barrier_actual(observation);
    if (op == "observe")
      actual.cancellation_calls = cancellation_calls;
    assert_expected(required(step, "expect"), actual);
  }
}

void validate_mutations(const Json &fixture,
                        const std::set<std::string> &scenario_ids) {
  for (const auto &mutation : required(fixture, "mutations").array) {
    const auto &must_fail = required(*mutation, "must_fail").array;
    REQUIRE(!must_fail.empty(), "mutation has no required kill");
    for (const auto &id : must_fail)
      REQUIRE(scenario_ids.count(id->as_str()) == 1,
              "mutation references missing scenario");
  }
}

void validate_reentrant_barrier_cancellation() {
  lazily::RevisionBarrier barrier(0, 1, std::nullopt);
  const auto observation = barrier.observe(0, false, [&] {
    const auto disposed = barrier.dispose();
    REQUIRE(disposed.outcome ==
                lazily::RevisionBarrierObservation::Outcome::disposed,
            "reentrant disposal did not latch");
    return lazily::TimeoutCancellation::cancelled;
  });
  REQUIRE(observation.outcome ==
              lazily::RevisionBarrierObservation::Outcome::disposed,
          "later cancellation overwrote reentrant disposal");
}

} // namespace

int main() {
  const std::pair<const char *, const char *> fixtures[] = {
      {"timer.json", "stdlib_timer_v1"},
      {"timeout.json", "stdlib_timeout_v1"},
      {"revision_barrier.json", "stdlib_revision_barrier_v1"},
  };
  for (const auto &[name, feature] : fixtures) {
    const auto fixture =
        lazily_test::parse_json(lazily_test::spec_fixture_text("stdlib", name));
    REQUIRE(string_field(*fixture, "feature") == feature,
            "stdlib feature mismatch");
    std::set<std::string> scenario_ids;
    for (const auto &scenario : required(*fixture, "scenarios").array) {
      scenario_ids.insert(string_field(*scenario, "id"));
      if (std::string(feature) == "stdlib_timer_v1")
        replay_timer(*scenario);
      else if (std::string(feature) == "stdlib_timeout_v1")
        replay_timeout(*scenario);
      else
        replay_barrier(*scenario);
    }
    validate_mutations(*fixture, scenario_ids);
  }
  validate_reentrant_barrier_cancellation();
  REQUIRE_FIXTURES_LOADED(3);
  std::cout << "stdlib conformance: ok\n";
  return 0;
}
