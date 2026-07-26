// The queue-family flavor ledger — enforced against the source, not a comment.
//
// `test_queue.cpp` covers the single-threaded `QueueCell`. That is currently the
// only flavor: no binding in the family ships a thread-safe or async queue
// primitive, and `cell-model.md` § "Core surface vs. binding extensions (queue
// family)" now makes those Core, so their absence is a conformance gap rather than
// an unfinished nicety.
//
// A three-flavor replay written today would skip two of three flavors entirely,
// and a suite that skips almost everything while reporting green is exactly the
// failure this file prevents. So the ledger is wired to the source: it greps
// `include/lazily` for each unshipped flavor's type name, and the moment one
// appears this goes red and names the runner to extend.
//
// This file ALSO opens the canonical bytes, which `test_queue.cpp` does not — that
// file hand-mirrors the fixtures and its coverage script honestly lists them as
// KNOWN_UNCOVERED. Reading them here proves the corpus is present and well-formed
// even while the replay itself is still a transcription (tracked separately).
//
// Mirrors lazily-rs/tests/queue_family_conformance.rs.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "test_json.hpp"
#include "test_require.hpp"
#include "test_spec_fixture.hpp"

using lazily_test::Json;
using lazily_test::JsonParser;
using lazily_test::JsonPtr;

namespace {

constexpr const char* kArea = "collections";

const std::vector<std::string>& queue_fixtures() {
  static const std::vector<std::string> names = {
      "queuecell_spsc_push_pop.json",
      "queuecell_popped_head_observation.json",
      "queuecell_mpsc_multi_writer.json",
      "queuecell_bounded_backpressure.json",
      "queuecell_closure_lifecycle.json",
  };
  return names;
}

struct Flavor {
  const char* name;
  // Grepped, not referenced: naming a type that does not exist would not compile,
  // and a ledger you cannot write until the work is done is no ledger at all.
  const char* marker_type;
  bool shipped;
};

const std::vector<Flavor>& ledger() {
  static const std::vector<Flavor> entries = {
      {"single-threaded", "class QueueCell", true},
      {"thread-safe", "ThreadSafeQueueCell", false},
      {"async", "AsyncQueueCell", false},
  };
  return entries;
}

// Resolve `include/lazily` by walking up from the working directory. ctest runs
// this binary from `build/`, so a cwd-relative path finds nothing — and the
// vacuity guard below correctly turned that into a loud failure rather than a
// silent pass, which is exactly what it is for.
std::filesystem::path find_include_dir() {
  std::filesystem::path dir = std::filesystem::current_path();
  for (int up = 0; up < 5; ++up) {
    const auto candidate = dir / "include" / "lazily";
    if (std::filesystem::is_directory(candidate)) return candidate;
    if (!dir.has_parent_path()) break;
    dir = dir.parent_path();
  }
  return {};
}

std::string header_sources() {
  std::string out;
  const std::filesystem::path root = find_include_dir();
  if (root.empty() || !std::filesystem::is_directory(root)) return out;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file()) continue;
    if (entry.path().extension() != ".hpp") continue;
    std::ifstream input(entry.path());
    std::stringstream buffer;
    buffer << input.rdbuf();
    out += buffer.str();
  }
  return out;
}

// The ledger is enforced. When a ThreadSafeQueueCell or AsyncQueueCell lands, this
// fails and says what to do, so a newly-shipped flavor cannot sit silently
// unreplayed while the suite reports green.
void unshipped_flavors_are_really_absent() {
  const std::string sources = header_sources();
  REQUIRE(!sources.empty(),
          "read no headers from include/lazily; the ledger check would be vacuous");

  for (const auto& flavor : ledger()) {
    const bool defined = sources.find(flavor.marker_type) != std::string::npos;
    if (flavor.shipped) {
      REQUIRE(defined,
              std::string("flavor ") + flavor.name +
                  " is recorded as shipped but its type is not defined in "
                  "include/lazily - the ledger claims coverage this library does not have");
    } else {
      REQUIRE(!defined,
              std::string("flavor ") + flavor.name +
                  " now EXISTS in include/lazily but the queue-family ledger still "
                  "records it as unshipped, so the canonical corpus is not being replayed "
                  "against it. Fix: flip `shipped` for it AND extend the replay to drive "
                  "it, as test_collections_family_conformance.cpp drives all three map "
                  "flavors. Do NOT flip the flag alone - that restores the false green "
                  "this test prevents.");
    }
  }
}

// A runner that skips everything must fail: in a summary line, "skipped" and
// "passed" are indistinguishable.
void ledger_is_not_all_skips() {
  std::size_t shipped = 0;
  for (const auto& flavor : ledger()) {
    if (flavor.shipped) ++shipped;
  }
  REQUIRE(shipped > 0,
          "every queue flavor is recorded as unshipped, so this suite would assert "
          "nothing while still reporting success");
  REQUIRE(ledger().size() == 3,
          "the ledger must cover all three execution flavors; a missing entry is an "
          "unscored gap, not an absent one");
}

// Positive proof this binary read the corpus. An absence guard proves the fixtures
// exist on disk; only a count proves they were opened.
void shipped_flavor_replays_the_corpus() {
  std::size_t fixtures_read = 0;
  std::size_t steps_seen = 0;
  std::size_t matrices_seen = 0;

  for (const auto& name : queue_fixtures()) {
    const std::string text = lazily_test::spec_fixture_text(kArea, name);
    JsonParser parser(text);
    JsonPtr root = parser.parse();
    REQUIRE(root && root->is_object(), name + ": fixture is not a JSON object");
    ++fixtures_read;

    const Json* steps = root->find("steps");
    REQUIRE(steps != nullptr && steps->is_array(), name + ": no steps array");
    REQUIRE(!steps->array.empty(),
            name + ": fixture has no steps - a vacuous replay would report green");
    steps_seen += steps->array.size();

    for (std::size_t i = 0; i < steps->array.size(); ++i) {
      const Json& step = *steps->array[i];
      // The matrix nests under `expected`, NOT on the step. lazily-rs's MAP runner
      // read it off the step, so it was always absent and the assertion never ran
      // once. Pin the nesting so that cannot recur here.
      REQUIRE(step.find("invalidates") == nullptr,
              name + " step " + std::to_string(i) +
                  ": `invalidates` appears at STEP level; runners read "
                  "expected.invalidates, so a step-level copy is silently ignored");
      const Json* expected = step.find("expected");
      REQUIRE(expected != nullptr, name + " step " + std::to_string(i) + ": no expected block");
      if (expected->find("invalidates") != nullptr) ++matrices_seen;
    }
  }

  REQUIRE(fixtures_read == queue_fixtures().size(),
          "did not read every declared queue fixture");
  REQUIRE(steps_seen > 0, "read the corpus but saw zero steps");
  REQUIRE(matrices_seen > 0,
          "no fixture carried an expected.invalidates matrix - the reader-kind "
          "independence contract would be unasserted");
}

}  // namespace

int main() {
  unshipped_flavors_are_really_absent();
  ledger_is_not_all_skips();
  shipped_flavor_replays_the_corpus();

  REQUIRE_FIXTURES_LOADED(5);
  std::cout << "queue family ledger: 1 shipped flavor, 2 enforced-absent, 5 fixtures read"
            << std::endl;
  return 0;
}
