// Reactive family-granularity sync conformance (`#lzfamilysync`).
//
// Replays the canonical `lazily-spec/conformance/familysync/
// materialize_on_ingest.json` fixture against the `CrdtPlaneRuntime` family
// layer — the language-agnostic conformance every binding MUST validate
// (`lazily-spec/protocol.md` § "Reactive family sync", proved in `lazily-formal`
// `FamilySync.lean`).
//
// A keyed op for a family entry NOT known locally MATERIALIZES the entry on
// ingest (seeded from the op's converged register) instead of being dropped, so
// membership propagates (a key added on one replica appears on the other), values
// are adopted, a later last-writer-wins update converges, re-ingest is
// idempotent, and a derived aggregate over the family (a count of `true` entries)
// converges across replicas. The three scenarios are transcribed by hand, the
// same fixture-mirroring pattern the other conformance tests use.

#include <lazily/lazily.hpp>

#include <algorithm>
#include <cassert>
#include <string>
#include <vector>

using namespace lazily;

static int test_count = 0;
static int test_passed = 0;

#define TEST(name)                                        \
  static void name();                                     \
  struct name##_runner {                                  \
    name##_runner() {                                     \
      ++test_count;                                       \
      name();                                             \
      ++test_passed;                                      \
    }                                                     \
  } name##_instance;                                      \
  static void name()

static const char* kNamespace = "live";

// Last segment of a `namespace/suffix` family key.
static std::string suffix_of(const std::string& key) {
  auto slash = key.rfind('/');
  return slash == std::string::npos ? key : key.substr(slash + 1);
}

// One local set on the origin runtime.
struct Set {
  std::string key;
  bool value;
  int64_t now;
};

// Ship the origin's whole op log + frontier and ingest into the target, then run
// every fixture assertion for a scenario.
static void run_scenario(PeerId origin_peer, PeerId target_peer,
                         const std::vector<Set>& sets, bool reingest,
                         const std::vector<std::string>& want_suffixes,
                         const std::vector<std::pair<std::string, bool>>& want_values,
                         int want_present_count, int want_count_true,
                         int want_reingest_applied, bool want_epoch_bumped) {
  CrdtPlaneRuntime origin(origin_peer);
  origin.register_family_lww(kNamespace);

  CrdtPlaneRuntime target(target_peer);
  target.register_family_lww(kNamespace);
  uint64_t epoch_before = target.membership_epoch();

  for (const auto& s : sets) {
    auto op = origin.family_set_lww(kNamespace, s.key, s.value, s.now);
    assert(op.has_value());
  }

  CrdtSync frame{origin.frontier_entries(), origin.ops()};
  int applied = target.ingest(frame);
  assert(applied > 0);

  if (reingest) {
    int reapplied = target.ingest(frame);
    assert(reapplied == want_reingest_applied);
  }

  // Materialized key set (by suffix), order-independent.
  std::vector<std::string> got_suffixes;
  for (const auto& k : target.family_keys(kNamespace))
    got_suffixes.push_back(suffix_of(k));
  std::sort(got_suffixes.begin(), got_suffixes.end());
  std::vector<std::string> want_sorted = want_suffixes;
  std::sort(want_sorted.begin(), want_sorted.end());
  assert(got_suffixes == want_sorted);

  // Present count.
  assert(static_cast<int>(target.family_keys(kNamespace).size()) ==
         want_present_count);

  // Adopted values converge.
  for (const auto& kv : want_values) {
    auto v = target.family_value_lww(kNamespace, kv.first);
    assert(v.has_value() && *v == kv.second);
  }

  // Derived aggregate: count of entries whose value is true.
  int count_true = 0;
  for (const auto& k : target.family_keys(kNamespace)) {
    auto v = target.family_value_lww(kNamespace, suffix_of(k));
    if (v.has_value() && *v) ++count_true;
  }
  assert(count_true == want_count_true);

  // Membership epoch bumped on materialize.
  if (want_epoch_bumped) assert(target.membership_epoch() != epoch_before);
}

// scenario "materialize remote keys and converge"
TEST(test_materialize_remote_keys_and_converge) {
  run_scenario(1, 2, {{"2", true, 100}, {"3", true, 101}},
               /*reingest=*/false, /*want_suffixes=*/{"2", "3"},
               /*want_values=*/{{"2", true}, {"3", true}},
               /*present=*/2, /*count_true=*/2, /*reingest_applied=*/0,
               /*epoch_bumped=*/true);
}

// scenario "last-writer-wins update converges after materialize"
TEST(test_lww_update_converges_after_materialize) {
  run_scenario(1, 2, {{"2", true, 100}, {"3", true, 101}, {"2", false, 300}},
               /*reingest=*/false, /*want_suffixes=*/{"2", "3"},
               /*want_values=*/{{"2", false}, {"3", true}},
               /*present=*/2, /*count_true=*/1, /*reingest_applied=*/0,
               /*epoch_bumped=*/true);
}

// scenario "membership only grows and re-ingest is idempotent"
TEST(test_membership_grows_and_reingest_idempotent) {
  run_scenario(1, 2, {{"7", true, 10}},
               /*reingest=*/true, /*want_suffixes=*/{"7"},
               /*want_values=*/{{"7", true}},
               /*present=*/1, /*count_true=*/1, /*reingest_applied=*/0,
               /*epoch_bumped=*/true);
}

int main() { return test_count == test_passed ? 0 : 1; }
