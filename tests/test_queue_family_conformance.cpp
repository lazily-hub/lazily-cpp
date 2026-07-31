// Queue-family flavor ledger. All three primitives ship on all three execution
// flavors. The actual replays live in test_queue_conformance,
// test_topic_conformance, and test_work_queue_conformance; this gate pins the
// public type inventory and opens every canonical corpus so a missing flavor or
// silently-added fixture cannot leave the matrix falsely green.
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
      "queuecell_spsc_push_pop.json",       "queuecell_popped_head_observation.json",
      "queuecell_mpsc_multi_writer.json",   "queuecell_bounded_backpressure.json",
      "queuecell_closure_lifecycle.json",   "topiccell_broadcast_cursor_isolation.json",
      "topiccell_durable_replay_gc.json",   "topiccell_ephemeral_lifecycle.json",
      "topiccell_offline_tail_bounds.json", "workqueue_competing_delivery.json",
      "workqueue_lease_deadletter.json",
  };
  return names;
}

struct Flavor {
  const char* name;
  // Grepped, not referenced: naming a type that does not exist would not
  // compile, and a ledger you cannot write until the work is done is no ledger
  // at all.
  const char* marker_type;
};

const std::vector<Flavor>& ledger() {
  static const std::vector<Flavor> entries = {
      {"QueueCell sync", "class QueueCell"},
      {"QueueCell thread-safe", "class ThreadSafeQueueCell"},
      {"QueueCell async", "class AsyncQueueCell"},
      {"TopicCell sync", "class TopicCell"},
      {"TopicCell thread-safe", "class ThreadSafeTopicCell"},
      {"TopicCell async", "class AsyncTopicCell"},
      {"WorkQueueCell sync", "class WorkQueueCell"},
      {"WorkQueueCell thread-safe", "class ThreadSafeWorkQueueCell"},
      {"WorkQueueCell async", "class AsyncWorkQueueCell"},
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

void every_flavor_is_defined() {
  const std::string sources = header_sources();
  REQUIRE(!sources.empty(),
          "read no headers from include/lazily; the ledger check would be vacuous");

  for (const auto& flavor : ledger()) {
    const bool defined = sources.find(flavor.marker_type) != std::string::npos;
    REQUIRE(defined,
            std::string("shipped flavor ") + flavor.name + " is not defined in include/lazily");
  }
}

void ledger_is_complete() {
  REQUIRE(ledger().size() == 9, "the ledger must cover three primitives x three execution flavors");
}

// Positive proof this binary read the corpus. An absence guard proves the
// fixtures exist on disk; only a count proves they were opened.
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
      // The matrix nests under `expected`, NOT on the step. lazily-rs's MAP
      // runner read it off the step, so it was always absent and the assertion
      // never ran once. Pin the nesting so that cannot recur here.
      REQUIRE(step.find("invalidates") == nullptr,
              name + " step " + std::to_string(i) +
                  ": `invalidates` appears at STEP level; runners read "
                  "expected.invalidates, so a step-level copy is silently ignored");
      const Json* expected = step.find("expected");
      REQUIRE(expected != nullptr, name + " step " + std::to_string(i) + ": no expected block");
      if (expected->find("invalidates") != nullptr) ++matrices_seen;
    }
  }

  REQUIRE(fixtures_read == queue_fixtures().size(), "did not read every declared queue fixture");
  REQUIRE(steps_seen > 0, "read the corpus but saw zero steps");
  REQUIRE(matrices_seen > 0, "no fixture carried an expected.invalidates matrix - the reader-kind "
                             "independence contract would be unasserted");
}

} // namespace

int main() {
  every_flavor_is_defined();
  ledger_is_complete();
  shipped_flavor_replays_the_corpus();

  REQUIRE_FIXTURES_LOADED(11);
  std::cout << "queue family ledger: 9 shipped flavors, 11 fixtures read" << std::endl;
  return 0;
}
