// Statechart conformance, replayed from the canonical lazily-spec fixture bytes.
//
// `conformance/statechart/` holds SEVEN fixtures and lazily-cpp replayed NONE of
// them. `tests/test_statechart.cpp` builds charts by hand with the fluent
// builders and asserts hand-written expectations; it never opens the corpus, so
// nothing in this binding could detect the spec's entry/exit ordering, LCA,
// history, or parallel-region rules changing — or being wrong here to begin with.
// The coverage matrix nevertheless showed statechart as covered.
//
// This runner builds each fixture's `chart` block through `ChartBuilder` /
// `StateBuilder` from the ACTUAL fixture bytes and replays every step: the event,
// the per-step guard resolutions, whether the event was accepted, the resulting
// active-leaf configuration, the ordered action trace, and the `matches`
// predicates over the full active configuration (ancestors included).
//
// ## No silent defaults
//
// Every discriminator read out of the fixture — a state's kind, a history depth,
// a transition's shape, the `active` expectation's shape — is mapped through a
// function that hard-fails on an unrecognised value. A runner that quietly
// resolves an unknown spelling to some default replays the wrong chart and
// reports green, which is the exact failure this file exists to remove.
// Unrecognised STEP keys are rejected too: a corpus that grows a new expectation
// must not be silently half-checked.
//
// ## Positive assertion
//
// `REQUIRE_FIXTURES_LOADED(7)` asserts all seven canonical files were actually
// opened, and `g_steps_replayed` asserts the replay did real work — an empty or
// truncated corpus cannot pass vacuously.

#include <lazily/core.hpp>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "test_assertion_keys.hpp"
#include "test_json.hpp"
#include "test_require.hpp"
#include "test_spec_fixture.hpp"

using namespace lazily;
using lazily_test::Json;
using lazily_test::JsonPtr;
using lazily_test::parse_json;

static const char* kArea = "statechart";

// Every fixture in the area. Compared against the directory listing in `main`,
// so a fixture landing upstream fails this runner instead of being ignored.
static const std::vector<std::string> kFixtures = {
    "entry_exit_actions.json",  "flat_cycle.json",   "guarded_door.json",
    "hierarchical_player.json", "history_deep.json", "history_shallow.json",
    "parallel_regions.json",
};

// Work actually performed, asserted non-zero in `main`.
static size_t g_steps_replayed = 0;
static size_t g_assertions = 0;

static void chart_require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

// ---------------------------------------------------------------------------
// Fixture -> chart
// ---------------------------------------------------------------------------

// `history` spells the depth of a history pseudo-state. An unknown spelling is
// an abort: silently picking one depth replays a different chart.
static bool history_is_deep(const std::string& spelling) {
  if (spelling == "deep") return true;
  if (spelling == "shallow") return false;
  chart_require(false, "unknown statechart history depth in fixture");
  return false; // unreachable
}

static std::vector<std::string> str_array(const Json* node) {
  chart_require(node != nullptr && node->is_array(), "expected a JSON array");
  std::vector<std::string> out;
  for (const auto& item : node->array) {
    chart_require(item->type == Json::Type::String, "expected an array of strings");
    out.push_back(item->str);
  }
  return out;
}

// `active` is spelled as a bare string for single-leaf charts and as an array
// for parallel ones. Anything else is an abort.
static std::vector<std::string> active_expectation(const Json* node) {
  REQUIRE(node != nullptr, "step has no `active` expectation");
  if (node->type == Json::Type::String) return {node->str};
  REQUIRE(node->is_array(), "`active` must be a state id or an array of state ids");
  auto out = str_array(node);
  std::sort(out.begin(), out.end());
  return out;
}

// A transition is either a bare target id or an object carrying target/guard/
// action/internal. Both spellings appear in the corpus.
static void add_transition(StateBuilder& sb, const std::string& event, const Json* value) {
  chart_require(value != nullptr, "transition has no value");
  if (value->type == Json::Type::String) {
    sb.on(event, value->str);
    return;
  }
  chart_require(value->is_object(), "a transition must be a target id or a transition object");
  const Json* target = value->find("target");
  chart_require(target != nullptr && target->type == Json::Type::String,
                "transition object has no target");
  TransitionBuilder tb = TransitionBuilder::to(target->str);
  if (const Json* guard = value->find("guard")) {
    chart_require(guard->type == Json::Type::String, "transition guard must be a name");
    tb.guard(guard->str);
  }
  if (const Json* action = value->find("action")) {
    for (const auto& act : str_array(action))
      tb.action(act);
  }
  if (const Json* internal = value->find("internal")) {
    chart_require(internal->type == Json::Type::Bool, "`internal` must be a boolean");
    if (internal->as_bool()) tb.internal();
  }
  sb.on_transition(event, std::move(tb));
}

// A state's kind is derived from which discriminating key it carries. The keys
// are mutually exclusive by construction; asserting that keeps a malformed
// fixture from silently collapsing to an atomic state.
static StateBuilder build_state(const std::string& id, const Json* def) {
  chart_require(def != nullptr && def->is_object(), "state definition is not an object");
  const Json* parallel = def->find("parallel");
  const Json* history = def->find("history");
  const Json* initial = def->find("initial");

  bool is_parallel = false;
  if (parallel != nullptr) {
    chart_require(parallel->type == Json::Type::Bool, "`parallel` must be a boolean");
    is_parallel = parallel->as_bool();
  }

  StateBuilder sb = StateBuilder::atomic(id);
  std::string inferred_kind = "atomic";
  if (is_parallel) {
    chart_require(history == nullptr, "a state cannot be both parallel and a history pseudo-state");
    sb = StateBuilder::parallel(id);
    inferred_kind = "parallel";
  } else if (history != nullptr) {
    chart_require(history->type == Json::Type::String, "`history` must be a spelling");
    chart_require(initial == nullptr, "a history pseudo-state has a `default`, never an `initial`");
    sb = history_is_deep(history->str) ? StateBuilder::history_deep(id)
                                       : StateBuilder::history_shallow(id);
    inferred_kind = "history";
    const Json* fallback = def->find("default");
    chart_require(fallback != nullptr && fallback->type == Json::Type::String,
                  "a history pseudo-state needs a `default` target");
    sb.default_child(fallback->str);
  } else if (initial != nullptr) {
    chart_require(initial->type == Json::Type::String, "`initial` must be a state id");
    sb = StateBuilder::compound(id, initial->str);
    inferred_kind = "compound";
  }

  if (const Json* kind = def->find("kind")) {
    chart_require(kind->type == Json::Type::String, "`kind` must be a string");
    const bool known = kind->str == "atomic" || kind->str == "compound" ||
                       kind->str == "parallel" || kind->str == "history" || kind->str == "final";
    chart_require(known, "unknown statechart kind");
    if (kind->str == "final") {
      chart_require(inferred_kind == "atomic", "`kind: final` contradicts structural fields");
      sb = StateBuilder::final_state(id);
    } else {
      chart_require(kind->str == inferred_kind, "declared `kind` contradicts structural fields");
    }
  }

  if (const Json* parent = def->find("parent")) {
    chart_require(parent->type == Json::Type::String, "`parent` must be a state id");
    sb.parent(parent->str);
  }
  if (const Json* entry = def->find("entry")) {
    for (const auto& act : str_array(entry))
      sb.entry(act);
  }
  if (const Json* exit_actions = def->find("exit")) {
    for (const auto& act : str_array(exit_actions))
      sb.exit(act);
  }
  if (const Json* on = def->find("on")) {
    chart_require(on->is_object(), "`on` must be an event map");
    for (const auto& kv : on->object)
      add_transition(sb, kv.first, kv.second.get());
  }
  return sb;
}

static ChartDef build_chart(const Json* chart) {
  chart_require(chart != nullptr && chart->is_object(), "fixture has no chart object");
  const Json* initial = chart->find("initial");
  chart_require(initial != nullptr && initial->type == Json::Type::String,
                "chart.initial must be a string");
  const Json* states = chart->find("states");
  chart_require(states != nullptr && states->is_object(), "chart has no states object");
  chart_require(states->find(initial->str) != nullptr, "chart.initial must name a declared state");
  ChartBuilder builder;
  for (const auto& kv : states->object)
    builder.state(build_state(kv.first, kv.second.get()));
  auto def = builder.build();
  chart_require(
      def.has_value(),
      "the fixture's chart does not build — duplicate ids, dangling refs, or no single root");
  return std::move(*def);
}

static void reject_malformed_corpus() {
  const JsonPtr fixture =
      parse_json(lazily_test::spec_fixture_text(kArea, "malformed_rejected.json"));
  const Json* cases = fixture->find("cases");
  REQUIRE(cases != nullptr && cases->is_array() && !cases->array.empty(),
          "malformed statechart corpus has no cases");
  for (const auto& scenario : cases->array) {
    const Json* name = scenario->find("name");
    const Json* chart = scenario->find("chart");
    REQUIRE(name != nullptr && name->type == Json::Type::String,
            "malformed statechart case has no name");
    bool rejected = false;
    try {
      (void)build_chart(chart);
    } catch (const std::exception&) {
      rejected = true;
    }
    REQUIRE(rejected, "a malformed statechart case was accepted");
  }
}

// ---------------------------------------------------------------------------
// Replay
// ---------------------------------------------------------------------------

static void expect_states(const std::string& fixture, size_t step, const char* what,
                          const std::vector<std::string>& actual,
                          const std::vector<std::string>& expected) {
  ++g_assertions;
  if (actual == expected) return;
  std::cout << "FAIL: " << fixture << " step " << step << ": " << what << " mismatch\n  expected:";
  for (const auto& s : expected)
    std::cout << " " << s;
  std::cout << "\n  actual:  ";
  for (const auto& s : actual)
    std::cout << " " << s;
  std::cout << std::endl;
  std::abort();
}

static void replay(const std::string& name) {
  const std::string text = lazily_test::spec_fixture_text(kArea, name);
  const JsonPtr fixture = parse_json(text);
  REQUIRE(fixture->is_object(), "fixture root is not an object");

  // Name the fixture AND the kind actually read (#lzledgeragreementaudit). A
  // bare "fixture is not a StateChart corpus" over eight replays says neither
  // which file carries the unrecognised kind nor what that kind was.
  const Json* kind = fixture->find("kind");
  REQUIRE(kind != nullptr && kind->type == Json::Type::String && kind->str == "StateChart",
          std::string(kArea) + "/" + name + ": unrecognised fixture kind " +
              (kind == nullptr                    ? "<absent>"
               : kind->type == Json::Type::String ? "'" + kind->str + "'"
                                                  : "<not a string>") +
              " — this runner replays only kind 'StateChart'. A corpus fixture carrying a new "
              "kind needs a replay arm here before it can be replayed; never widen this to a skip");

  Context ctx;
  StateChart chart(ctx, build_chart(fixture->find("chart")));

  expect_states(name, 0, "initial active configuration", chart.active_leaves(ctx),
                active_expectation(fixture->find("initial_active")));

  if (const Json* initial_actions = fixture->find("initial_actions")) {
    expect_states(name, 0, "initial action trace", chart.last_actions(),
                  str_array(initial_actions));
  }

  const Json* steps = fixture->find("steps");
  REQUIRE(steps != nullptr && steps->is_array() && !steps->array.empty(),
          "fixture has no steps to replay");

  for (size_t i = 0; i < steps->array.size(); ++i) {
    const Json& step = *steps->array[i];
    REQUIRE(step.is_object(), "a step is not an object");
    lazily_test::AssertionKeys expected(
        std::string(kArea) + "/" + name + " steps[" + std::to_string(i) + "]", step);
    expected.excuse_key("event", "operation input replayed by StateChart::send");
    if (step.find("guards"))
      expected.excuse_key("guards", "operation input replayed as StateChart guard resolutions");

    const Json* event = step.find("event");
    REQUIRE(event != nullptr && event->type == Json::Type::String, "step has no event");

    std::unordered_map<std::string, bool> guards;
    if (const Json* g = step.find("guards")) {
      REQUIRE(g->is_object(), "`guards` must be an object");
      for (const auto& kv : g->object) {
        REQUIRE(kv.second->type == Json::Type::Bool, "a guard resolution must be a boolean");
        guards[kv.first] = kv.second->as_bool();
      }
    }

    const bool accepted = chart.send(ctx, event->str, guards);
    ++g_steps_replayed;

    ++g_assertions;
    expected.assert_key("accepted", accepted);

    expected.assert_key_with("active", [&](const Json& active) {
      expect_states(name, i + 1, "active configuration", chart.active_leaves(ctx),
                    active_expectation(&active));
      return true;
    });

    expected.assert_key_with_if_present("actions", [&](const Json& actions) {
      expect_states(name, i + 1, "action trace", chart.last_actions(), str_array(&actions));
      return true;
    });

    expected.with_sub_if_present("matches", [&](lazily_test::AssertionKeys& matches) {
      for (const auto& state : matches.keys()) {
        ++g_assertions;
        matches.assert_key(state, chart.matches(ctx, state));
      }
    });
  }
}

int main() {
  // Both history spellings resolve, so a corpus that only exercises one today
  // still pins the mapper. (The reject path aborts by design and is not
  // asserted here.)
  REQUIRE(history_is_deep("deep") && !history_is_deep("shallow"),
          "history depth spellings must map to distinct depths");

  lazily_test::require_spec_checkout(kArea);

  // The fixture list is checked against the corpus rather than trusted: a file
  // landing upstream that this runner does not replay fails here instead of
  // shrinking coverage silently.
  std::vector<std::string> on_disk;
  for (const auto& entry :
       std::filesystem::directory_iterator(lazily_test::spec_conformance_dir() / kArea)) {
    if (entry.path().extension() == ".json") on_disk.push_back(entry.path().filename().string());
  }
  std::sort(on_disk.begin(), on_disk.end());
  std::vector<std::string> known = kFixtures;
  known.push_back("malformed_rejected.json");
  std::sort(known.begin(), known.end());
  // Name the offending files, in both directions (#lzledgeragreementaudit).
  // This guard exists to fire on a corpus somebody else grew, and "the set does
  // not match" leaves that reader diffing two lists by hand.
  if (on_disk != known) {
    std::string detail;
    for (const auto& file : on_disk)
      if (!std::binary_search(known.begin(), known.end(), file))
        detail += "\n  ON DISK but not in kFixtures: " + file +
                  " — add it to kFixtures so it cannot go unrun";
    for (const auto& file : known)
      if (!std::binary_search(on_disk.begin(), on_disk.end(), file))
        detail += "\n  in kFixtures but NOT ON DISK: " + file +
                  " — renamed or removed upstream; update kFixtures";
    REQUIRE(false, "the statechart corpus on disk does not match this runner's fixture list — a "
                   "fixture was added or renamed upstream" +
                       detail);
  }

  for (const auto& name : kFixtures)
    replay(name);
  reject_malformed_corpus();

  REQUIRE(g_steps_replayed >= 33, "the statechart replay performed too few steps — the corpus is "
                                  "empty, truncated, or short-circuited");
  REQUIRE(g_assertions >= 80, "the statechart replay made too few assertions to be meaningful");

  std::cout << "statechart conformance: " << kFixtures.size() << " fixtures, " << g_steps_replayed
            << " steps, " << g_assertions << " assertions" << std::endl;

  REQUIRE_FIXTURES_LOADED(8);
  return 0;
}
