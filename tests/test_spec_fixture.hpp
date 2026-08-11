// Canonical conformance-fixture loader (#lzspecconf).
//
// Conformance fixtures are owned by the sibling `lazily-spec` repo and are read
// from `../lazily-spec/conformance/<area>/` — never from a copy inside this
// repo. lazily-cpp previously vendored them under `tests/conformance/`, which
// meant the suite validated against whatever this repo happened to hold rather
// than against the spec. A vendored copy cannot drift-detect: elsewhere in the
// family a vendored fixture silently shrank to a third of its source.
//
// There is deliberately NO fallback to a local copy. A fallback is precisely
// what makes drift invisible — the suite would go green against stale data and
// nobody would learn.
//
// Absence is a HARD FAILURE, not a skip (#lzcppsiblingskipvsfail). It used to be
// a SKIP (CTest exit 77), and that was measured rather than argued: with the
// corpus absent, 25 of this repo's 62 ctest targets execute and 37 skip, 0 of the
// 139 canonical fixtures and 0 of the 149 scenarios are replayed, and the whole
// cross-binding conformance layer — 654 of the suite's 927 assertion sites —
// contributes nothing. `make check` is the pre-commit gate; a green there over
// that state is a false green, and it is the same false green that let 9b0ff08
// pass locally and land red on CI. The skip also protected nothing that worked:
// since bd2380a a checkout without the sibling has failed `make check` anyway,
// at `assertion-ordering-check`, which runs a script out of the same sibling.
//
// So the corpus is a REQUIRED input, answered the same way by every guard that
// touches it: refuse, name the path, and say how to get it.
//
// The spec directory is baked in at configure time from
// `LAZILY_SPEC_CONFORMANCE_DIR` (see tests/CMakeLists.txt) so the path resolves
// identically no matter what working directory ctest runs the binary from.
// Overridable at run time via the LAZILY_SPEC_CONFORMANCE_DIR env var.
//
// An absence guard cannot catch a test that loads nothing at all, so each
// suite ends with REQUIRE_FIXTURES_LOADED(n): a positive assertion that the
// expected number of DISTINCT canonical fixtures were actually opened.
//
// "Was the file opened" is still one rung short of "was the file replayed".
// A fixture carrying several named scenarios can be PARTIALLY replayed and
// nothing notices: the file was opened, so the manifest is satisfied; and the
// assertion-key guards in test_assertion_keys.hpp only bind blocks a runner
// actually reaches, so a scenario nobody enters contributes no unconsumed and
// no unasserted key. Skipping a whole scenario is invisible to a guard that
// only inspects the scenarios you ran.
//
// The scenario ledger below is that rung (see the plan item in
// tasks/software/plan-lazily-scenario-coverage.md). Two halves, deliberately
// asymmetric:
//
//   * the DECLARED half is automatic. `spec_fixture_text` parses every `.json`
//     it hands out and records each scenario id the file carries, so a runner
//     cannot opt out of being measured and a NEW runner that never learned
//     about the ledger still declares its scenarios;
//   * the REPLAYED half is explicit: `record_scenario_at` at the top of the
//     replay loop body, AFTER any `continue`, so a scenario the runner skips
//     does not record itself.
//
// scripts/check-conformance-coverage.sh compares the two halves across the
// whole suite and refuses a declared-but-never-replayed scenario unless the
// excuse list living beside KNOWN_UNCOVERED names it with a reason.

#ifndef LAZILY_TESTS_TEST_SPEC_FIXTURE_HPP
#define LAZILY_TESTS_TEST_SPEC_FIXTURE_HPP

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "test_assertion_keys.hpp"
#include "test_json.hpp"
#include "test_require.hpp"

#ifndef LAZILY_SPEC_CONFORMANCE_DIR
#error "LAZILY_SPEC_CONFORMANCE_DIR is not defined — configure via tests/CMakeLists.txt"
#endif

namespace lazily_test {

// Distinct canonical fixture ids opened by this binary, spelled relative to the
// conformance root (`statechart/flat_cycle.json`, `snapshot_minimal.json`).
inline std::set<std::string>& loaded_fixtures() {
  static std::set<std::string> loaded;
  return loaded;
}

// Cross-binary coverage manifest.
//
// REQUIRE_FIXTURES_LOADED is per-binary: it proves *this* executable read what
// it claims to. It cannot see the suite as a whole, so an entire area losing its
// runner — deleted, renamed, dropped from tests/CMakeLists.txt, filtered out by
// a `ctest -R` selector — leaves every surviving binary green. lazily-kt closes
// that hole with a manifest of fixtures actually read, flushed at JVM shutdown
// and audited by scripts/check-conformance-coverage.sh; this is the same
// mechanism for a suite of independent C++ executables. Every binary APPENDS the
// fixtures it opened to the file named by LAZILY_CONFORMANCE_MANIFEST, and the
// script asserts the union covers every required area.
//
// The append happens at static-destruction time. `ensure_manifest_flusher()`
// touches `loaded_fixtures()` before constructing the flusher so the set is
// created first and therefore destroyed last — the flusher's destructor is
// guaranteed to see a live set. A run that aborts (REQUIRE failure) skips the
// flush, which is correct: an aborted run has already failed.
// Per-scenario replay ledger.
//
// Scenario id resolution, fixed and identical in every binding:
//
//   1. `id` when the scenario carries one
//   2. else `name`
//
// There is no third step (#lzspecscenarioids). The positional "#<n>" fallback
// let the ledger record a scenario BY POSITION, where inserting one ahead of it
// silently rebinds that entry -- and any excuse naming it -- to a different
// scenario, with nothing turning red: the guard compares "index 1 was replayed"
// against whatever now sits at index 1 and agrees with itself.
//
// It was load-bearing for exactly one fixture,
// collections/mergecell_algebra.json, whose three scenarios were told apart by
// `policy` alone. They carry ids now, and lazily-spec's
// `scenario-identity-check` keeps every scenario identified -- so this is a hole
// with no users, which is one waiting to become load-bearing again.
//
// `unidentified` is set instead of inventing an id, and a BLANK identifier sets
// it too: accepting a blank would file every blank-id scenario under one ledger
// entry, which reads as "replayed" the moment any one of them runs.
inline bool scenario_identifier_is_blank(const std::string& value) {
  return value.find_first_not_of(" \t\r\n") == std::string::npos;
}

inline std::string resolve_scenario_id(const Json& scenario, std::size_t index,
                                       bool* unidentified = nullptr) {
  if (unidentified != nullptr) *unidentified = false;
  if (scenario.is_object()) {
    for (const char* key : {"id", "name"}) {
      const Json* value = scenario.find(key);
      if (value != nullptr && value->type == Json::Type::String &&
          !scenario_identifier_is_blank(value->str))
        return value->str;
    }
  }
  if (unidentified != nullptr) *unidentified = true;
  return "#" + std::to_string(index);
}

// (fixture id, scenario id) pairs. `declared` is what the corpus on disk says
// exists; `replayed` is what a runner actually entered; `unidentified` flags the
// entries the corpus gives no stable identifier at all -- a corpus defect the
// coverage script FAILS on rather than booking by position
// (#lzspecscenarioids).
struct ScenarioLedger {
  std::set<std::pair<std::string, std::string>> declared;
  std::set<std::pair<std::string, std::string>> replayed;
  std::set<std::pair<std::string, std::string>> unidentified;
  std::set<std::string> scanned; // fixtures already enumerated
};

inline ScenarioLedger& scenario_ledger() {
  static ScenarioLedger ledger;
  return ledger;
}

// Enumerate the scenarios a fixture carries. Called from `spec_fixture_text`,
// so declaration is automatic: opening a scenario-bearing fixture is itself the
// claim that its scenarios will be replayed, and a runner that reads the bytes
// and replays nothing is exactly the failure this rung exists to catch.
inline void declare_fixture_scenarios(const std::string& fixture_id, const std::string& text) {
  if (fixture_id.size() < 5 || fixture_id.compare(fixture_id.size() - 5, 5, ".json") != 0) return;
  auto& ledger = scenario_ledger();
  if (!ledger.scanned.insert(fixture_id).second) return;
  const JsonPtr root = parse_json(text);
  if (!root->is_object()) return;
  const Json* scenarios = root->find("scenarios");
  if (scenarios == nullptr || !scenarios->is_array()) return;
  for (std::size_t i = 0; i < scenarios->array.size(); ++i) {
    bool unidentified = false;
    const std::string id = resolve_scenario_id(*scenarios->array[i], i, &unidentified);
    ledger.declared.emplace(fixture_id, id);
    if (unidentified) ledger.unidentified.emplace(fixture_id, id);
  }
}

// Record that this run REPLAYED `scenario_id` of `fixture_id`. Call it as the
// first statement of the replay loop body — after any `continue` that skips a
// scenario, never before it, or a skip records itself as covered.
inline void record_scenario(const std::string& fixture_id, const std::string& scenario_id) {
  scenario_ledger().replayed.emplace(fixture_id, scenario_id);
}

// The common form: resolve the id from the scenario node and record it, so a
// runner never spells an id by hand and cannot drift from the resolution order
// above. Returns the id, which doubles as the label for failure messages.
inline std::string record_scenario_at(const std::string& fixture_id, const Json& scenario,
                                      std::size_t index) {
  bool unidentified = false;
  std::string id = resolve_scenario_id(scenario, index, &unidentified);
  // An unidentified scenario is a corpus defect, not an id to invent
  // (#lzspecscenarioids). Booking it by POSITION would silently rebind this
  // ledger entry to a different scenario on any corpus reorder, so refuse.
  if (unidentified)
    throw std::runtime_error(fixture_id + ": scenario at index " + std::to_string(index) +
                             " carries neither `id` nor `name`. The replay ledger would have "
                             "to record it by POSITION, where inserting a scenario ahead of "
                             "it silently rebinds that entry to a different scenario. Give "
                             "it a stable id upstream in lazily-spec (#lzspecscenarioids).");
  record_scenario(fixture_id, id);
  return id;
}

// ---------------------------------------------------------------------------
// Yielding is not replaying (#lzscenariobodyskip)
// ---------------------------------------------------------------------------
//
// `record_scenario_at` above books at the top of the loop body, which is one
// step better than booking at the yield and still blind to the case rung 4
// exists for: the call runs, and then the body does nothing. An early
// `continue`, an unmatched dispatch arm, a `return` past the replay, a body
// someone commented out — all of them leave a booked scenario that ran nothing,
// and no other guard can see it, because an unreplayed scenario contributes no
// unconsumed and no unasserted key.
//
// lazily-py found this against the contract's own probe; lazily-js, lazily-rs,
// lazily-cs, lazily-kt, lazily-go, lazily-dart and lazily-zig carry the fix.
// The rule is the same everywhere: book on the PAYLOAD, stay silent on the
// LABEL.
//
// C++ has no property interception, so the seam is a handle that owns the
// payload. `id()` and `peek()` are label reads; `replay()` is the only way to
// reach the scenario a runner is about to replay, so forgetting to book stops
// being expressible.
class ScenarioView {
public:
  ScenarioView(std::string fixture, const Json& scenario, std::size_t index)
      : fixture_(std::move(fixture)), scenario_(&scenario), index_(index) {}

  // The resolved ledger id. Silent — naming a scenario is not replaying it.
  const std::string& id() const {
    if (id_.empty()) {
      bool unidentified = false;
      id_ = resolve_scenario_id(*scenario_, index_, &unidentified);
      if (unidentified)
        throw std::runtime_error(
            fixture_ + ": scenario at index " + std::to_string(index_) +
            " carries neither `id` nor `name`. The replay ledger would have to record it "
            "by POSITION, where inserting a scenario ahead of it silently rebinds that "
            "entry to a different scenario. Give it a stable id upstream in lazily-spec "
            "(#lzspecscenarioids).");
    }
    return id_;
  }

  std::size_t index() const { return index_; }

  // The scenario WITHOUT booking. For a runner that must inspect one it is not
  // replaying.
  const Json& peek() const { return *scenario_; }

  // Book this scenario as REPLAYED and hand over its payload.
  const Json& replay() const {
    if (!booked_) {
      booked_ = true;
      record_scenario(fixture_, id());
    }
    return *scenario_;
  }

private:
  std::string fixture_;
  const Json* scenario_;
  std::size_t index_;
  mutable std::string id_;
  mutable bool booked_ = false;
};

// Wrap a fixture's `scenarios` array. Iterating books nothing; each view books
// when its payload is taken.
inline std::vector<ScenarioView> scenario_views(const std::string& fixture_id,
                                                const std::vector<JsonPtr>& scenarios) {
  std::vector<ScenarioView> out;
  out.reserve(scenarios.size());
  for (std::size_t i = 0; i < scenarios.size(); ++i)
    out.emplace_back(fixture_id, *scenarios[i], i);
  return out;
}

struct ManifestFlusher {
  ~ManifestFlusher() {
    const char* out = std::getenv("LAZILY_CONFORMANCE_MANIFEST");
    if (out == nullptr || *out == '\0') return;
    std::ofstream manifest(out, std::ios::app);
    if (!manifest) return;
    for (const auto& id : loaded_fixtures())
      manifest << id << "\n";
    // Scenario records are tab-delimited and tag-prefixed so the coverage
    // script can separate them from the bare fixture ids above; a scenario id
    // may contain spaces (28 of the 31 fixtures name theirs in prose) but
    // never a tab.
    const auto& ledger = scenario_ledger();
    for (const auto& entry : ledger.declared)
      manifest << "@declared\t" << entry.first << "\t" << entry.second << "\n";
    for (const auto& entry : ledger.replayed)
      manifest << "@replayed\t" << entry.first << "\t" << entry.second << "\n";
    for (const auto& entry : ledger.unidentified)
      manifest << "@unidentified\t" << entry.first << "\t" << entry.second << "\n";
  }
};

inline void ensure_manifest_flusher() {
  loaded_fixtures();              // constructed first => destroyed last
  scenario_ledger();              // ditto, so the flusher outlives neither
  static ManifestFlusher flusher; // destroyed before both
  (void)flusher;
}

// Root of the canonical conformance corpus (sibling lazily-spec checkout).
inline std::filesystem::path spec_conformance_dir() {
  if (const char* override_dir = std::getenv("LAZILY_SPEC_CONFORMANCE_DIR")) {
    if (*override_dir != '\0') return std::filesystem::path(override_dir);
  }
  return std::filesystem::path(LAZILY_SPEC_CONFORMANCE_DIR);
}

// Fail when the sibling spec checkout is absent. Called before any fixture read,
// so a missing sibling is a refusal — never a skip and never a pass
// (#lzcppsiblingskipvsfail).
inline void require_spec_checkout(const std::string& area) {
  const auto dir = spec_conformance_dir() / area;
  if (!std::filesystem::is_directory(dir)) {
    std::cerr << "ERROR: canonical conformance corpus not found at " << dir << "\n"
              << "       git clone https://github.com/lazily-hub/lazily-spec.git "
                 "../lazily-spec\n"
              << "       (or point LAZILY_SPEC_CONFORMANCE_DIR at a checkout)\n"
              << "       The canonical corpus is a REQUIRED input, not an optional one:\n"
              << "       without it the " << area
              << " suite replays nothing, and a run that replays\n"
              << "       nothing must not report success." << std::endl;
    std::exit(1);
  }
}

// Read a canonical fixture's raw text, recording that it was actually opened.
inline std::string spec_fixture_text(const std::string& area, const std::string& name) {
  require_spec_checkout(area);
  const auto path = spec_conformance_dir() / area / name;
  std::ifstream input(path);
  REQUIRE(input, "canonical conformance fixture missing from the lazily-spec sibling "
                 "— a conformance test must not pass without its fixture");
  ensure_manifest_flusher();
  const std::string fixture_id = area.empty() ? name : area + "/" + name;
  loaded_fixtures().insert(fixture_id);
  std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  declare_fixture_scenarios(fixture_id, text);
  // Rule 8 (`#lzprosekeyconvention`). A fixture whose `assertions` block
  // declares `prose` must reach `verify_prose`, and the requirement is derived
  // HERE -- from the bytes just read -- rather than from a list a runner keeps.
  // Opening a fixture and never replaying it satisfies rules 1-7 vacuously.
  declare_prose_requirement(fixture_id, text);
  // Rung 0 (`#lznullformblind`). A top-level `assertions` block must be BOUND to
  // AssertionKeys by someone. Derived here for the same reason: every rung above
  // rung 0 is scoped to a block a runner already bound, so an unbound block is
  // not reported as unread -- it is not reported at all.
  if (text.find("\"assertions\"") != std::string::npos)
    declare_assertion_block(fixture_id, *parse_json(text));
  return text;
}

} // namespace lazily_test

// Positive assertion that fixtures actually ran. An absence guard proves the
// corpus is present; it cannot prove this binary read any of it. Assert the
// exact distinct-fixture count so deleting or short-circuiting a fixture read
// turns the suite red instead of quietly shrinking its coverage.
#define REQUIRE_FIXTURES_LOADED(expected)                                                          \
  do {                                                                                             \
    const std::size_t actual_ = lazily_test::loaded_fixtures().size();                             \
    if (actual_ != static_cast<std::size_t>(expected)) {                                           \
      std::cout << "FAIL: expected " << (expected)                                                 \
                << " distinct canonical fixtures to be read, but " << actual_                      \
                << " were — the suite is not exercising the spec corpus it "                     \
                   "claims to"                                                                     \
                << std::endl;                                                                      \
      return 1;                                                                                    \
    }                                                                                              \
  } while (0)

#endif // LAZILY_TESTS_TEST_SPEC_FIXTURE_HPP
