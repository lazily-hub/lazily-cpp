// Keyed-map materialization conformance, replayed from the canonical
// lazily-spec fixture bytes.
//
// This runner opens all three `conformance/materialization/*` fixtures and
// replays their ACTUAL bytes -- entry kinds, canonical values, reads, and the
// expected present-sets -- through `SourceMap` / `ComputedMap`.
//
// It used to replay only `entry_kind_orthogonal_to_mode.json`, while
// `observational_transparency.json` and `deferral_not_deallocation.json` were
// mirrored by hand in `tests/test_reactive_family.cpp` and its thread-safe /
// async siblings. That kept the unit suite runnable without the sibling spec
// checkout and could not detect drift: a hand transcription agrees with
// whatever it was transcribed from, so C++ scored `~` on all three
// materialization rows in coverage.json (`#lzcppmatreplay`). The two
// `spec.val`-shaped fixtures now replay through `test_materialization_replay.hpp`
// -- shared with the thread-safe / async shells, which live in the `-pthread`
// wasm tier and therefore in their own binary.
//
// ## Entry-kind spellings (forward compatibility)
//
// The fixture's `spec.entries[key].kind` is wire data shared by every binding
// runner. It spells the two kinds `"cell"` / `"slot"` today; the v2 kernel
// renamed the node kinds to `Source` / `Computed` and the fixture is expected to
// follow. `entry_kind_of` therefore accepts BOTH spellings so this binding does
// not go red on the day the corpus flips, and neither spelling is privileged.
//
// Anything else is a hard abort, not a default: a silently-defaulted kind is the
// exact failure that lets a runner report green while replaying the wrong shape.
// `main` asserts all four accepted spellings directly, so the forward-compatible
// half is exercised even while the on-disk fixture still says `"cell"`/`"slot"`.
// (The reject path aborts by design and so is not asserted here.)
//
// ## Positive assertion
//
// `REQUIRE_FIXTURES_LOADED(3)` asserts all three canonical files were actually
// opened -- an absence guard proves the corpus is on disk, not that this binary
// read it, and the exact count turns a dropped fixture red instead of quietly
// shrinking coverage.

#include <lazily/core.hpp>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "test_assertion_keys.hpp"
#include "test_json.hpp"
#include "test_materialization_replay.hpp"
#include "test_require.hpp"
#include "test_spec_fixture.hpp"

using namespace lazily;
using lazily_test::Json;
using lazily_test::JsonParser;
using lazily_test::JsonPtr;

namespace {

constexpr const char* kArea = "materialization";
constexpr const char* kFixture = "entry_kind_orthogonal_to_mode.json";

// The single-threaded shell, adapted to the shared replay contract.
class SyncModel {
public:
  static const char* shell() { return "Context/ComputedMap"; }

  template <typename Factory> uint32_t read(const std::string& key, Factory factory) {
    return slots_.get_or_insert_with(ctx_, key, factory);
  }

  template <typename Factory>
  void materialize_all(const std::vector<std::string>& keys, Factory factory) {
    slots_.materialize_all(ctx_, keys, factory);
  }

  std::optional<uint32_t> observe(const std::string& key) { return slots_.get(ctx_, key); }
  std::vector<std::string> present_keys() const { return slots_.present_keys(); }
  std::size_t present_count() const { return slots_.present_count(); }
  bool is_present(const std::string& key) const { return slots_.is_present(key); }

private:
  Context ctx_;
  ComputedMap<std::string, uint32_t> slots_{ctx_};
};

// Fixture entry-kind string -> `EntryKind`.
//
// `"cell"`/`"slot"` are the pre-v2 wire spellings; `"source"`/`"computed"` are
// the v2 kernel names the corpus is moving to. Both are accepted; anything else
// aborts.
EntryKind entry_kind_of(const std::string& kind) {
  if (kind == "cell" || kind == "source") return EntryKind::Source;
  if (kind == "slot" || kind == "computed") return EntryKind::Computed;
  REQUIRE(false, "fixture entry kind '" + kind +
                     "' is neither an input (cell/source) nor a derived "
                     "(slot/computed) spelling");
  return EntryKind::Source; // unreachable: REQUIRE aborts
}

std::vector<std::string> strings_of(const Json* array, const char* what) {
  REQUIRE(array != nullptr && array->is_array(), what);
  std::vector<std::string> out;
  for (const auto& element : array->array)
    out.push_back(element->str);
  return out;
}

std::vector<std::string> sorted(std::vector<std::string> keys) {
  std::sort(keys.begin(), keys.end());
  return keys;
}

} // namespace

int main() {
  // Both spellings of each kind resolve to the same `EntryKind`, so a corpus
  // flip from cell/slot to source/computed is a no-op for this runner.
  REQUIRE(entry_kind_of("cell") == EntryKind::Source &&
              entry_kind_of("source") == EntryKind::Source,
          "cell and source must both name the input entry kind");
  REQUIRE(entry_kind_of("slot") == EntryKind::Computed &&
              entry_kind_of("computed") == EntryKind::Computed,
          "slot and computed must both name the derived entry kind");

  const std::string text = lazily_test::spec_fixture_text(kArea, kFixture);
  JsonParser parser(text);
  const JsonPtr fixture = parser.parse();

  const Json* model = fixture->find("model");
  REQUIRE(model != nullptr && model->str == "ComputedMap",
          "fixture is not a ComputedMap materialization corpus");

  const Json* spec = fixture->find("spec");
  REQUIRE(spec != nullptr, "fixture has no spec block");
  const Json* entries = spec->find("entries");
  REQUIRE(entries != nullptr && entries->is_object(), "fixture spec has no entries object");

  // Partition the fixture's entries by kind. Input entries are modelled by a
  // `SourceMap`, derived entries by a `ComputedMap` -- the two handle kinds the
  // one keyed primitive abstracts over.
  std::vector<std::pair<std::string, uint32_t>> cell_entries;
  std::vector<std::pair<std::string, uint32_t>> slot_entries;
  for (const auto& kv : entries->object) {
    const Json* kind = kv.second->find("kind");
    const Json* val = kv.second->find("val");
    REQUIRE(kind != nullptr && val != nullptr, "fixture entry needs both a kind and a val");
    const auto value = static_cast<uint32_t>(val->as_int());
    switch (entry_kind_of(kind->str)) {
    case EntryKind::Source:
      cell_entries.emplace_back(kv.first, value);
      break;
    case EntryKind::Computed:
      slot_entries.emplace_back(kv.first, value);
      break;
    }
  }
  REQUIRE(!cell_entries.empty() && !slot_entries.empty(),
          "the entry-kind fixture must exercise both entry kinds");

  const std::map<std::string, uint32_t> slot_val(slot_entries.begin(), slot_entries.end());
  auto factory = [&slot_val](const std::string& key) -> uint32_t {
    auto it = slot_val.find(key);
    REQUIRE(it != slot_val.end(), "fixture reads a key that is not a derived entry");
    return it->second;
  };

  const Json* expected_block = fixture->find("expected");
  REQUIRE(expected_block != nullptr, "fixture has no expected block");
  lazily_test::AssertionKeys expected("materialization expected", *expected_block);
  const auto reads = strings_of(fixture->find("reads"), "fixture has no reads");
  // The strategy a caller gets without asking for one. The fixture carried it
  // and nothing read it, so this runner exercised both strategies while never
  // stating which the corpus calls the default (#lzassertunknownkeys).
  const std::string default_mode = lazily_test::json_string(expected.required("default_mode"));
  REQUIRE(default_mode == "eager" || default_mode == "lazy",
          "default_mode must name a strategy this runner exercises");
  const auto eager_present = strings_of(expected.find("eager_present"), "expected.eager_present");
  const auto lazy_after_reads =
      strings_of(expected.find("lazy_present_after_reads"), "expected.lazy_present_after_reads");
  const Json* observe = expected.find("observe");
  REQUIRE(observe != nullptr && observe->is_object(), "expected.observe must be an object");

  // -- Lazy strategy --

  Context ctx;
  SourceMap<std::string, uint32_t> cells(ctx);
  for (const auto& kv : cell_entries)
    cells.set(ctx, kv.first, kv.second);
  REQUIRE(cells.entry_kind() == EntryKind::Source, "a SourceMap must report the input entry kind");

  ComputedMap<std::string, uint32_t> slots(ctx);
  REQUIRE(slots.entry_kind() == EntryKind::Computed,
          "a ComputedMap must report the derived entry kind");

  auto present_keys = [&cells, &slots]() {
    std::vector<std::string> keys = cells.present_keys();
    for (const auto& key : slots.present_keys())
      keys.push_back(key);
    return sorted(std::move(keys));
  };

  // Input entries are materialized at build in every strategy; derived entries
  // are deferred until read.
  expected.assert_key_with("lazy_present_at_build", [&](const Json& want) {
    return present_keys() == sorted(strings_of(&want, "expected.lazy_present_at_build"));
  });

  for (const auto& key : reads) {
    REQUIRE(slots.get_or_insert_with(ctx, key, factory) == factory(key),
            "a lazily-minted derived entry read back its canonical value");
  }
  expected.assert_key_with("lazy_present_after_reads", [&](const Json& want) {
    return present_keys() == sorted(strings_of(&want, "expected.lazy_present_after_reads"));
  });

  // Reads are strategy-independent: every key observes its canonical value,
  // whether it was present at build or minted on access.
  // Descended into (`#lzsubblockkeyset`): the child tracker owns each entry
  // name, so a key the fixture grows must reach a comparison instead of being
  // walked past by a loop nothing audits.
  expected.with_sub("observe", [&](lazily_test::AssertionKeys& observe) {
    for (const auto& name : observe.keys()) {
      observe.assert_key_with(name, [&](const Json& want_value) {
        const auto want = static_cast<uint32_t>(want_value.as_int());
        if (cells.is_present(name)) {
          REQUIRE(cells.get(ctx, name) == std::optional<uint32_t>(want),
                  "an input entry did not observe its canonical value");
        } else {
          REQUIRE(slots.get_or_insert_with(ctx, name, factory) == want,
                  "a derived entry did not observe its canonical value");
        }
        return true;
      });
    }
  });

  // -- Eager strategy --

  Context eager_ctx;
  SourceMap<std::string, uint32_t> eager_cells(eager_ctx);
  for (const auto& kv : cell_entries)
    eager_cells.set(eager_ctx, kv.first, kv.second);

  std::vector<std::string> slot_keys;
  for (const auto& kv : slot_entries)
    slot_keys.push_back(kv.first);
  ComputedMap<std::string, uint32_t> eager_slots(eager_ctx);
  eager_slots.materialize_all(eager_ctx, slot_keys, factory);

  expected.assert_key_with("eager_present", [&](const Json& want) {
    std::vector<std::string> eager_keys = eager_cells.present_keys();
    for (const auto& key : eager_slots.present_keys())
      eager_keys.push_back(key);
    return sorted(std::move(eager_keys)) == sorted(strings_of(&want, "expected.eager_present"));
  });

  for (const auto& kv : observe->object) {
    const auto want = static_cast<uint32_t>(kv.second->as_int());
    const bool is_input = eager_cells.is_present(kv.first);
    const std::optional<uint32_t> got =
        is_input ? eager_cells.get(eager_ctx, kv.first) : eager_slots.get(eager_ctx, kv.first);
    REQUIRE(got == std::optional<uint32_t>(want),
            "an eagerly-materialized entry did not observe its canonical value");
  }

  // ... and the declared default's present-set is the one that has to hold
  // for a map built without a strategy argument.
  expected.assert_key_with("default_mode", [&](const Json& want) {
    const std::string mode = lazily_test::json_string(want);
    REQUIRE(mode == "eager" || mode == "lazy",
            "default_mode must name a strategy this runner exercises");
    std::vector<std::string> default_keys = eager_cells.present_keys();
    const auto& want_default = mode == "eager" ? eager_present : lazy_after_reads;
    for (const auto& key : eager_slots.present_keys())
      default_keys.push_back(key);
    return sorted(std::move(default_keys)) == sorted(want_default);
  });

  std::cout << "materialization conformance: " << kFixture << " replayed (" << cell_entries.size()
            << " input + " << slot_entries.size() << " derived entries)" << std::endl;

  // The two `spec.val`-shaped fixtures, replayed from bytes through the same
  // single-threaded shell. These retire the hand mirrors in
  // tests/test_reactive_family.cpp.
  for (const char* name : {"observational_transparency.json", "deferral_not_deallocation.json"}) {
    lazily_test::replay_materialization<SyncModel>(lazily_test::load_materialization_fixture(name));
  }

  REQUIRE_FIXTURES_LOADED(3);
  return 0;
}
