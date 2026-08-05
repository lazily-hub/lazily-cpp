// Reactive family-granularity sync conformance (`#lzfamilysync`).
//
// Replays the canonical `lazily-spec/conformance/familysync/
// materialize_on_ingest.json` fixture against the `CrdtPlaneRuntime` family
// layer. The fixture is the authority: peers, writes, re-ingest, and every
// expected projection are read from its scenarios rather than transcribed here.

#include <lazily/lazily.hpp>

#include "test_spec_fixture.hpp"
#include <algorithm>
#include <cassert>
#include <set>
#include <string>
#include <vector>

using namespace lazily;

static int test_count = 0;
static int test_passed = 0;

#define TEST(name)                                                                                 \
  static void name();                                                                              \
  struct name##_runner {                                                                           \
    name##_runner() {                                                                              \
      ++test_count;                                                                                \
      name();                                                                                      \
      ++test_passed;                                                                               \
    }                                                                                              \
  } name##_instance;                                                                               \
  static void name()

static std::string suffix_of(const std::string& key) {
  const auto slash = key.rfind('/');
  return slash == std::string::npos ? key : key.substr(slash + 1);
}

static std::vector<std::string> json_strings(const lazily_test::Json& value) {
  std::vector<std::string> out;
  for (const auto& item : lazily_test::json_array(value))
    out.push_back(lazily_test::json_string(*item));
  std::sort(out.begin(), out.end());
  return out;
}

static void require_fixture_keys(const lazily_test::Json& object,
                                 std::initializer_list<const char*> expected,
                                 const std::string& where) {
  REQUIRE(object.is_object(), where + ": expected object");
  std::set<std::string> actual;
  for (const auto& entry : object.object)
    actual.insert(entry.first);
  std::set<std::string> want;
  for (const char* key : expected)
    want.insert(key);
  REQUIRE(actual == want, where + ": fixture object keys changed");
}

TEST(test_materialize_on_ingest_fixture_replay) {
  constexpr const char* fixture_id = "familysync/materialize_on_ingest.json";
  const auto root = lazily_test::parse_json(
      lazily_test::spec_fixture_text("familysync", "materialize_on_ingest.json"));
  require_fixture_keys(
      *root, {"description", "kind", "model", "namespace", "value_type", "scenarios"}, fixture_id);
  REQUIRE(lazily_test::json_string(lazily_test::json_member(*root, "kind")) == "FamilySync",
          "family-sync kind");
  REQUIRE(lazily_test::json_string(lazily_test::json_member(*root, "model")) == "FamilySync",
          "family-sync model");
  REQUIRE(lazily_test::json_string(lazily_test::json_member(*root, "value_type")) == "bool",
          "family-sync value type");
  const std::string family_namespace =
      lazily_test::json_string(lazily_test::json_member(*root, "namespace"));

  const auto& raw_scenarios = lazily_test::json_array(lazily_test::json_member(*root, "scenarios"));
  for (const auto& view : lazily_test::scenario_views(fixture_id, raw_scenarios)) {
    const auto& scenario = view.replay();
    const std::string where = std::string(fixture_id) + " " + view.id();
    const bool reingest = scenario.has("reingest");
    if (reingest) {
      require_fixture_keys(
          scenario,
          {"id", "name", "origin_peer", "target_peer", "origin_sets", "reingest", "expect"}, where);
      REQUIRE(lazily_test::json_bool(lazily_test::json_member(scenario, "reingest")),
              where + ": reingest selector must be true");
    } else {
      require_fixture_keys(
          scenario, {"id", "name", "origin_peer", "target_peer", "origin_sets", "expect"}, where);
    }

    CrdtPlaneRuntime origin(
        lazily_test::json_u64(lazily_test::json_member(scenario, "origin_peer")));
    CrdtPlaneRuntime target(
        lazily_test::json_u64(lazily_test::json_member(scenario, "target_peer")));
    origin.register_family_lww(family_namespace);
    target.register_family_lww(family_namespace);
    const uint64_t epoch_before = target.membership_epoch();

    for (const auto& raw_set :
         lazily_test::json_array(lazily_test::json_member(scenario, "origin_sets"))) {
      const auto& set = *raw_set;
      require_fixture_keys(set, {"key", "value", "now"}, where + " origin_set");
      const auto op = origin.family_set_lww(
          family_namespace, lazily_test::json_string(lazily_test::json_member(set, "key")),
          lazily_test::json_bool(lazily_test::json_member(set, "value")),
          static_cast<int64_t>(lazily_test::json_u64(lazily_test::json_member(set, "now"))));
      REQUIRE(op.has_value(), where + ": origin family write was rejected");
    }

    const CrdtSync frame{origin.frontier_entries(), origin.ops()};
    REQUIRE(target.ingest(frame) > 0, where + ": initial family ingest applied no ops");
    int reingest_applied = -1;
    if (reingest) reingest_applied = target.ingest(frame);

    std::vector<std::string> target_keys;
    for (const auto& key : target.family_keys(family_namespace))
      target_keys.push_back(suffix_of(key));
    std::sort(target_keys.begin(), target_keys.end());

    int count_true = 0;
    for (const auto& key : target_keys) {
      const auto value = target.family_value_lww(family_namespace, key);
      if (value.has_value() && *value) ++count_true;
    }

    lazily_test::AssertionKeys expected(where + " expect",
                                        lazily_test::json_member(scenario, "expect"));
    expected.assert_key("target_keys", target_keys, json_strings);
    expected.with_sub("target_values", [&](lazily_test::AssertionKeys& values) {
      for (const auto& key : target_keys) {
        const auto value = target.family_value_lww(family_namespace, key);
        REQUIRE(value.has_value(), where + ": materialized key has no value");
        values.assert_key(key, *value);
      }
    });
    expected.assert_key("target_present_count", target_keys.size());
    expected.assert_key("target_count_true", count_true);
    if (reingest) expected.assert_key("reingest_applied", reingest_applied);
    expected.assert_key("target_epoch_bumped", target.membership_epoch() != epoch_before);
  }
}

int main() {
  REQUIRE_FIXTURES_LOADED(1);
  return test_count == test_passed ? 0 : 1;
}
