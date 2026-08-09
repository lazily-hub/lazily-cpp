// Canonical replay of the two `spec.val`-shaped materialization fixtures,
// shared by every shell that claims them (`#lzcppmatreplay`).
//
// `conformance/materialization/observational_transparency.json` and
// `deferral_not_deallocation.json` were previously mirrored BY HAND in
// tests/test_reactive_family.cpp and its thread-safe / async siblings: the
// canonical values were re-typed as `k * 3` and `k * 2`, the read sequences as
// brace-initialiser lists, and the expected present-sets as literal vectors.
//
// A mirror transcribed from the implementation agrees with whatever it was
// transcribed from. It cannot detect drift between this binding and the corpus,
// because the corpus is not what it reads — which is exactly the failure that
// retiring lazily-zig's eight inline mirrors turned into three real wire
// defects. Only `entry_kind_orthogonal_to_mode.json` was replayed from bytes,
// so C++ scored `~` on all three materialization rows in coverage.json.
//
// This header owns the replay once and is parameterised over the map shell, so
// the single-threaded, thread-safe and async flavors all replay the SAME bytes
// instead of three hand-transcriptions of them. The shells cannot be one binary:
// `ThreadSafeContext` / `AsyncContext` force the `-pthread` wasm tier (see
// wasm-tiers.conf), and the core-tier runner must stay buildable without it.
//
// ## What a `Model` must provide
//
//   Model()                                       fresh context + empty map
//   uint32_t read(key, factory)                   LAZY mint-on-access
//   void materialize_all(keys, factory)           EAGER pre-mint
//   std::optional<uint32_t> observe(key)          read back an already-present key
//   std::vector<std::string> present_keys()
//   std::size_t present_count()
//   bool is_present(key)
//   static const char* shell()                    label for failure messages
//
// `read` is the mint-on-access path and `observe` the read-back path; the async
// shell drives its handle in both, which is what "eventual transparency" means
// there and why the two are separate operations rather than one accessor.

#ifndef LAZILY_TESTS_TEST_MATERIALIZATION_REPLAY_HPP
#define LAZILY_TESTS_TEST_MATERIALIZATION_REPLAY_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "test_assertion_keys.hpp"
#include "test_json.hpp"
#include "test_require.hpp"
#include "test_spec_fixture.hpp"

namespace lazily_test {

constexpr const char* kMaterializationArea = "materialization";

// The parsed shape shared by both `spec.val` fixtures: canonical values, the
// declaration order of the keys, and the lazy read sequence.
struct MaterializationFixture {
  JsonPtr root; // owns every `Json` referenced below
  std::string name;
  std::vector<std::string> keys; // spec.val declaration order
  std::map<std::string, uint32_t> val;
  std::vector<std::string> reads;
  const Json* expected = nullptr;

  // Canonical value of `key`. A read of a key the corpus never declared is a
  // hard abort rather than a mint of some default: a defaulted value is what
  // lets a runner replay the wrong shape and still agree with itself.
  uint32_t value_of(const std::string& key) const {
    const auto it = val.find(key);
    REQUIRE(it != val.end(), "fixture reads key '" + key + "', which spec.val does not declare");
    return it->second;
  }
};

inline std::vector<std::string> string_array_of(const Json* array, const char* what) {
  REQUIRE(array != nullptr && array->is_array(), what);
  std::vector<std::string> out;
  for (const auto& element : array->array)
    out.push_back(element->str);
  return out;
}

inline std::vector<std::string> sorted_keys(std::vector<std::string> keys) {
  std::sort(keys.begin(), keys.end());
  return keys;
}

inline MaterializationFixture load_materialization_fixture(const std::string& name) {
  MaterializationFixture fixture;
  fixture.name = name;
  const std::string text = spec_fixture_text(kMaterializationArea, name);
  JsonParser parser(text);
  fixture.root = parser.parse();

  const Json* model = fixture.root->find("model");
  REQUIRE(model != nullptr && model->str == "ComputedMap",
          name + " is not a ComputedMap materialization fixture");

  const Json* spec = fixture.root->find("spec");
  REQUIRE(spec != nullptr, name + " has no spec block");
  const Json* val = spec->find("val");
  REQUIRE(val != nullptr && val->is_object(), name + " spec has no val object");
  for (const auto& kv : val->object) {
    fixture.keys.push_back(kv.first);
    fixture.val.emplace(kv.first, static_cast<uint32_t>(kv.second->as_int()));
  }
  REQUIRE(!fixture.keys.empty(), name + " declares no derived entries");

  fixture.reads = string_array_of(fixture.root->find("reads"), "fixture has no reads array");
  fixture.expected = fixture.root->find("expected");
  REQUIRE(fixture.expected != nullptr, name + " has no expected block");
  return fixture;
}

// Replay one fixture through one shell.
//
// Every expectation is read from the fixture's own `expected` block through an
// `AssertionKeys` tracker, so a key the corpus grows fails this runner by name
// rather than being walked past — the mirrors this replaces could not fail that
// way at all, since they carried no reference to the block.
template <typename Model> void replay_materialization(const MaterializationFixture& fixture) {
  const auto factory = [&fixture](const std::string& key) { return fixture.value_of(key); };
  const std::string where =
      std::string(Model::shell()) + " materialization expected (" + fixture.name + ")";
  AssertionKeys expected(where, *fixture.expected);

  // -- eager: pre-mint every declared key --

  Model eager;
  eager.materialize_all(fixture.keys, factory);
  expected.assert_key_with("eager_present", [&](const Json& want) {
    return sorted_keys(eager.present_keys()) ==
           sorted_keys(string_array_of(&want, "expected.eager_present"));
  });

  // -- lazy: mint only what is read --

  Model lazy;
  REQUIRE(lazy.present_count() == 0, "an untouched derived map must materialize nothing");
  std::vector<std::size_t> present_after_each_read;
  std::size_t high_water = 0;
  for (const auto& key : fixture.reads) {
    REQUIRE(lazy.read(key, factory) == fixture.value_of(key),
            "a lazily-minted derived entry did not read back its canonical value");
    // `materialize_present_monotone`: materializing only ever GROWS the present
    // set. Asserted on every step rather than only on the final size, because a
    // drop followed by a re-mint lands on the same total.
    REQUIRE(lazy.present_count() >= high_water,
            "the present set shrank across a read — materialization deferred a node, it did "
            "not deallocate one");
    high_water = lazy.present_count();
    present_after_each_read.push_back(lazy.present_count());
  }
  expected.assert_key_with("lazy_present_after_reads", [&](const Json& want) {
    return sorted_keys(lazy.present_keys()) ==
           sorted_keys(string_array_of(&want, "expected.lazy_present_after_reads"));
  });
  // Carried only by deferral_not_deallocation.json; consumed either way, so its
  // absence from the other fixture is recorded rather than unnoticed.
  expected.assert_key_with_if_present("present_after_each_read", [&](const Json& want) {
    REQUIRE(want.is_array(), "expected.present_after_each_read must be an array");
    std::vector<std::size_t> counts;
    for (const auto& element : want.array)
      counts.push_back(static_cast<std::size_t>(element->as_int()));
    return counts == present_after_each_read;
  });

  // `lazy_present_subset_eager`: everything the lazy session materialized is
  // present in the eager one. Read from the two live maps, not from the fixture,
  // so it holds for whatever read sequence the corpus grows.
  for (const auto& key : lazy.present_keys())
    REQUIRE(eager.is_present(key), "a lazily-present key was absent from the eager present set");

  // -- observational transparency --
  //
  // The claim is that the two strategies return IDENTICAL values for every key,
  // so each canonical value is checked against both: the eager map's pre-minted
  // slot and a third, untouched map minted on access in fixture order. A
  // values-only check against one strategy would pass on a binding whose eager
  // path never materializes.
  Model observed;
  expected.with_sub("observe", [&](AssertionKeys& observe) {
    for (const auto& name : observe.keys()) {
      observe.assert_key_with(name, [&](const Json& want_value) {
        const auto want = static_cast<uint32_t>(want_value.as_int());
        REQUIRE(eager.observe(name) == std::optional<uint32_t>(want),
                "an eagerly pre-minted entry did not observe its canonical value");
        REQUIRE(observed.read(name, factory) == want,
                "a mint-on-access entry did not observe its canonical value");
        // `materialize_preserves_observe`: minting this key changed no earlier
        // key's observed value — allocation causes no churn.
        for (const auto& seen : observed.present_keys()) {
          REQUIRE(observed.observe(seen) == std::optional<uint32_t>(fixture.value_of(seen)),
                  "materializing one entry changed another entry's observed value");
        }
        return true;
      });
    }
  });

  // `default_mode_eager`. This binding's map carries no mode flag -- eager vs
  // lazy is which call the caller makes -- so the fixture's value SELECTS the
  // build and the asserted fact is that a map built that way is fully
  // materialized at build time. Editing the key therefore changes the outcome;
  // comparing it against the literal "eager" would assert only that the fixture
  // equals itself (`#lzconsumednotasserted`), which is what an earlier draft of
  // this runner did and what the fixture-perturbation probe caught.
  expected.assert_key_with("default_mode", [&](const Json& want) {
    const std::string mode = json_string(want);
    Model as_default;
    if (mode == "eager") {
      as_default.materialize_all(fixture.keys, factory);
    } else {
      REQUIRE(mode == "lazy", "unknown default_mode '" + mode + "'");
    }
    return as_default.present_count() == fixture.keys.size();
  });

  std::cout << "  " << Model::shell() << ": " << fixture.name << " replayed ("
            << fixture.keys.size() << " derived entries, " << fixture.reads.size() << " reads)"
            << std::endl;
}

} // namespace lazily_test

#endif // LAZILY_TESTS_TEST_MATERIALIZATION_REPLAY_HPP
