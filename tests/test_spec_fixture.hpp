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
// nobody would learn. Absence is a SKIP (CTest exit 77) with an explicit
// message, and CI asserts the directory exists so a skip cannot pass silently.
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
#include <string>
#include <utility>

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
//   1. `id` when the scenario carries one   (the three stdlib corpora)
//   2. else `name`                          (28 of the 31 scenario fixtures)
//   3. else the positional index, spelled "#<n>", 0-based
//
// The positional fallback exists so this guard is not blocked on a shared
// corpus edit: collections/mergecell_algebra.json distinguishes its three
// scenarios by `policy` alone and carries no identifier at all. A fallback id
// is REPORTED by the coverage script rather than silently accepted, because
// that visibility is what makes the corpus gap fixable upstream later.
inline std::string resolve_scenario_id(const Json& scenario, std::size_t index,
                                       bool* positional = nullptr) {
  if (positional != nullptr) *positional = false;
  if (scenario.is_object()) {
    for (const char* key : {"id", "name"}) {
      const Json* value = scenario.find(key);
      if (value != nullptr && value->type == Json::Type::String &&
          !value->str.empty())
        return value->str;
    }
  }
  if (positional != nullptr) *positional = true;
  return "#" + std::to_string(index);
}

// (fixture id, scenario id) pairs. `declared` is what the corpus on disk says
// exists; `replayed` is what a runner actually entered; `positional` flags the
// entries whose id came from the index fallback.
struct ScenarioLedger {
  std::set<std::pair<std::string, std::string>> declared;
  std::set<std::pair<std::string, std::string>> replayed;
  std::set<std::pair<std::string, std::string>> positional;
  std::set<std::string> scanned;  // fixtures already enumerated
};

inline ScenarioLedger& scenario_ledger() {
  static ScenarioLedger ledger;
  return ledger;
}

// Enumerate the scenarios a fixture carries. Called from `spec_fixture_text`,
// so declaration is automatic: opening a scenario-bearing fixture is itself the
// claim that its scenarios will be replayed, and a runner that reads the bytes
// and replays nothing is exactly the failure this rung exists to catch.
inline void declare_fixture_scenarios(const std::string& fixture_id,
                                      const std::string& text) {
  if (fixture_id.size() < 5 ||
      fixture_id.compare(fixture_id.size() - 5, 5, ".json") != 0)
    return;
  auto& ledger = scenario_ledger();
  if (!ledger.scanned.insert(fixture_id).second) return;
  const JsonPtr root = parse_json(text);
  if (!root->is_object()) return;
  const Json* scenarios = root->find("scenarios");
  if (scenarios == nullptr || !scenarios->is_array()) return;
  for (std::size_t i = 0; i < scenarios->array.size(); ++i) {
    bool positional = false;
    const std::string id =
        resolve_scenario_id(*scenarios->array[i], i, &positional);
    ledger.declared.emplace(fixture_id, id);
    if (positional) ledger.positional.emplace(fixture_id, id);
  }
}

// Record that this run REPLAYED `scenario_id` of `fixture_id`. Call it as the
// first statement of the replay loop body — after any `continue` that skips a
// scenario, never before it, or a skip records itself as covered.
inline void record_scenario(const std::string& fixture_id,
                            const std::string& scenario_id) {
  scenario_ledger().replayed.emplace(fixture_id, scenario_id);
}

// The common form: resolve the id from the scenario node and record it, so a
// runner never spells an id by hand and cannot drift from the resolution order
// above. Returns the id, which doubles as the label for failure messages.
inline std::string record_scenario_at(const std::string& fixture_id,
                                      const Json& scenario, std::size_t index) {
  std::string id = resolve_scenario_id(scenario, index);
  record_scenario(fixture_id, id);
  return id;
}

struct ManifestFlusher {
  ~ManifestFlusher() {
    const char* out = std::getenv("LAZILY_CONFORMANCE_MANIFEST");
    if (out == nullptr || *out == '\0') return;
    std::ofstream manifest(out, std::ios::app);
    if (!manifest) return;
    for (const auto& id : loaded_fixtures()) manifest << id << "\n";
    // Scenario records are tab-delimited and tag-prefixed so the coverage
    // script can separate them from the bare fixture ids above; a scenario id
    // may contain spaces (28 of the 31 fixtures name theirs in prose) but
    // never a tab.
    const auto& ledger = scenario_ledger();
    for (const auto& entry : ledger.declared)
      manifest << "@declared\t" << entry.first << "\t" << entry.second << "\n";
    for (const auto& entry : ledger.replayed)
      manifest << "@replayed\t" << entry.first << "\t" << entry.second << "\n";
    for (const auto& entry : ledger.positional)
      manifest << "@positional\t" << entry.first << "\t" << entry.second
               << "\n";
  }
};

inline void ensure_manifest_flusher() {
  loaded_fixtures();               // constructed first => destroyed last
  scenario_ledger();               // ditto, so the flusher outlives neither
  static ManifestFlusher flusher;  // destroyed before both
  (void)flusher;
}

// Root of the canonical conformance corpus (sibling lazily-spec checkout).
inline std::filesystem::path spec_conformance_dir() {
  if (const char* override_dir = std::getenv("LAZILY_SPEC_CONFORMANCE_DIR")) {
    if (*override_dir != '\0') return std::filesystem::path(override_dir);
  }
  return std::filesystem::path(LAZILY_SPEC_CONFORMANCE_DIR);
}

// Exit 77 (CTest SKIP) when the sibling spec checkout is absent. Called before
// any fixture read so a missing sibling is an explicit skip, never a pass.
inline void require_spec_checkout_or_skip(const std::string& area) {
  const auto dir = spec_conformance_dir() / area;
  if (!std::filesystem::is_directory(dir)) {
    std::cout << "SKIP: canonical conformance fixtures not found at " << dir
              << " — clone the lazily-spec sibling "
                 "(git clone https://github.com/lazily-hub/lazily-spec.git "
                 "../lazily-spec) to run the "
              << area << " conformance suite" << std::endl;
    std::exit(77);
  }
}

// Read a canonical fixture's raw text, recording that it was actually opened.
inline std::string spec_fixture_text(const std::string& area,
                                     const std::string& name) {
  require_spec_checkout_or_skip(area);
  const auto path = spec_conformance_dir() / area / name;
  std::ifstream input(path);
  REQUIRE(input,
          "canonical conformance fixture missing from the lazily-spec sibling "
          "— a conformance test must not pass without its fixture");
  ensure_manifest_flusher();
  const std::string fixture_id = area.empty() ? name : area + "/" + name;
  loaded_fixtures().insert(fixture_id);
  std::string text{std::istreambuf_iterator<char>(input),
                   std::istreambuf_iterator<char>()};
  declare_fixture_scenarios(fixture_id, text);
  return text;
}

}  // namespace lazily_test

// Positive assertion that fixtures actually ran. An absence guard proves the
// corpus is present; it cannot prove this binary read any of it. Assert the
// exact distinct-fixture count so deleting or short-circuiting a fixture read
// turns the suite red instead of quietly shrinking its coverage.
#define REQUIRE_FIXTURES_LOADED(expected)                                     \
  do {                                                                        \
    const std::size_t actual_ = lazily_test::loaded_fixtures().size();        \
    if (actual_ != static_cast<std::size_t>(expected)) {                      \
      std::cout << "FAIL: expected " << (expected)                            \
                << " distinct canonical fixtures to be read, but "            \
                << actual_                                                    \
                << " were — the suite is not exercising the spec corpus it "  \
                   "claims to"                                                \
                << std::endl;                                                 \
      return 1;                                                               \
    }                                                                         \
  } while (0)

#endif  // LAZILY_TESTS_TEST_SPEC_FIXTURE_HPP
