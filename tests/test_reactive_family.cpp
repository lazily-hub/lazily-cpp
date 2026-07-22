// ReactiveMap keyed-collection tests (`#reactivemap`).
//
// Mirrors the Rust reference tests in `lazily-rs/src/cell_family.rs` and the
// shared lazily-spec conformance fixtures in `conformance/materialization/*`
// (now `"model": "SlotMap"`):
//   - observational_transparency.json
//   - deferral_not_deallocation.json
//   - entry_kind_orthogonal_to_mode.json
//
// The unified primitive is `ReactiveMap<K, V, H>` with two specializations:
// `CellMap<K, V>` (input cells, `set` + eager `entry`) and `SlotMap<K, V>`
// (derived slots, `get_or_insert_with` lazy mint + `materialize_all` eager
// pre-mint). There is no eager/lazy mode flag.

#include <lazily/lazily.hpp>

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

// -- CellMap: eager value-minting + set --

// `entry_caches_one_cell_per_key`: same key -> same cell; second default ignored.
TEST(test_entry_caches_one_cell_per_key) {
  Context ctx;
  CellMap<std::string, int> map(ctx);
  auto a1 = map.entry(ctx, "a", 1);
  auto a2 = map.entry(ctx, "a", 999);
  assert(a1.id() == a2.id());
  assert(ctx.get(a1) == 1);
  assert(map.len_untracked() == 1);
}

// `get_or_insert_with_mints_once_then_returns_existing`.
TEST(test_get_or_insert_with_mints_once) {
  Context ctx;
  CellMap<std::string, int> map(ctx);
  int calls = 0;
  assert(map.get_or_insert_with(ctx, "a", [&](const std::string&) {
    ++calls;
    return 7;
  }) == 7);
  assert(map.len_untracked() == 1);
  // Second access returns the existing value; factory NOT re-run.
  assert(map.get_or_insert_with(ctx, "a", [&](const std::string&) {
    ++calls;
    return 999;
  }) == 7);
  assert(calls == 1);
  // An explicit set is observed by a subsequent get_or_insert_with.
  map.set(ctx, "a", 42);
  assert(map.get_or_insert_with(ctx, "a", [](const std::string&) { return 0; }) ==
         42);
}

// `membership_is_reactive_but_value_changes_are_not`.
TEST(test_membership_reactive_value_not) {
  Context ctx;
  CellMap<std::string, int> map(ctx);
  auto a = map.entry(ctx, "a", 1);
  map.entry(ctx, "b", 2);

  auto count = ctx.computed<int>([&](Context& c) { return (int)map.len(c); });
  assert(ctx.get(count) == 2);

  ctx.set(a, 100);
  assert(ctx.is_set(count) && "membership reader stayed cached");
  assert(ctx.get(count) == 2);

  map.entry(ctx, "c", 3);
  assert(ctx.get(count) == 3);

  assert(map.remove(ctx, "b"));
  assert(ctx.get(count) == 2);
  auto keys = map.keys(ctx);
  assert((keys == std::vector<std::string>{"a", "c"}));
}

// -- SlotMap: lazy mint-on-access + eager materialize_all --

// `slot_map_mints_lazily_and_caches`.
TEST(test_slot_map_mints_lazily_and_caches) {
  Context ctx;
  SlotMap<uint32_t, uint32_t> fam(ctx);
  assert(fam.present_count() == 0);
  assert(fam.get_or_insert_with(ctx, 7, [](const uint32_t& k) { return k * 2; }) ==
         14);
  assert(fam.present_count() == 1);
  assert(fam.is_present(7));
  // Same key -> same derived slot (value preserved, factory not re-run).
  auto h = fam.handle(7);
  assert(h && ctx.get(*h) == 14);
  assert(fam.get_or_insert_with(ctx, 7, [](const uint32_t& k) { return k * 999; }) ==
         14);
}

// `slot_map_materialize_all_is_eager`.
TEST(test_slot_map_materialize_all_is_eager) {
  Context ctx;
  SlotMap<uint32_t, uint32_t> fam(ctx);
  fam.materialize_all(ctx, {0, 1, 2, 5, 9}, [](const uint32_t& k) { return k * 3; });
  assert(fam.present_count() == 5);
  for (uint32_t k : {0u, 1u, 2u, 5u, 9u}) assert(fam.is_present(k));
  assert(fam.get(ctx, 5) == std::optional<uint32_t>(15));
  assert(fam.entry_kind() == EntryKind::Slot);
}

// -- move_* (order/membership separation) --

TEST(test_move_to_reorders_and_keeps_identity) {
  Context ctx;
  CellMap<std::string, int> map(ctx);
  auto a = map.entry(ctx, "a", 1);
  map.entry(ctx, "b", 2);
  map.entry(ctx, "c", 3);
  assert((map.keys(ctx) == std::vector<std::string>{"a", "b", "c"}));

  assert(map.move_to(ctx, "c", 0));
  assert((map.keys(ctx) == std::vector<std::string>{"c", "a", "b"}));
  assert(map.handle("a")->id() == a.id());
  assert(map.get(ctx, "a") == std::optional<int>(1));

  assert(!map.move_to(ctx, "z", 0));
}

TEST(test_pure_move_spares_membership) {
  Context ctx;
  CellMap<std::string, int> map(ctx);
  map.entry(ctx, "a", 1);
  map.entry(ctx, "b", 2);
  map.entry(ctx, "c", 3);

  auto order_reader = ctx.computed<size_t>([&](Context& c) {
    return map.keys(c).size();
  });
  auto count = ctx.computed<int>([&](Context& c) { return (int)map.len(c); });
  auto has_b =
      ctx.computed<bool>([&](Context& c) { return map.contains_key(c, "b"); });
  ctx.get(order_reader);
  ctx.get(count);
  ctx.get(has_b);

  assert(map.move_to(ctx, "a", 2));
  assert(ctx.is_set(count) && "len reader stays cached on pure move");
  assert(ctx.is_set(has_b) && "contains_key stays cached on pure move");
}

TEST(test_move_before_and_after) {
  Context ctx;
  CellMap<int, int> map(ctx);
  for (int k = 0; k < 4; ++k) map.entry(ctx, k, k * 10);
  assert((map.keys(ctx) == std::vector<int>{0, 1, 2, 3}));

  assert(map.move_before(ctx, 3, 1));
  assert((map.keys(ctx) == std::vector<int>{0, 3, 1, 2}));

  assert(map.move_after(ctx, 0, 2));
  assert((map.keys(ctx) == std::vector<int>{3, 1, 2, 0}));

  assert(!map.move_before(ctx, 3, 99));
  assert(!map.move_after(ctx, 99, 2));
}

// -- Spec conformance fixtures (model: SlotMap) --

// conformance/materialization/observational_transparency.json
TEST(test_conformance_observational_transparency) {
  Context ctx;
  auto factory = [](const uint32_t& k) { return k * 3; };  // spec.val = k*3
  std::vector<uint32_t> keys{0, 1, 2, 5, 9};

  // Eager pre-mints all; lazy (untouched map) has none present.
  SlotMap<uint32_t, uint32_t> eager(ctx);
  eager.materialize_all(ctx, keys, factory);
  SlotMap<uint32_t, uint32_t> lazy(ctx);
  assert(eager.present_count() == 5);
  assert(lazy.present_count() == 0);

  // observe: identical canonical values whether pre-minted or minted on access.
  for (uint32_t k : keys)
    assert(eager.get(ctx, k) ==
           std::optional<uint32_t>(lazy.get_or_insert_with(ctx, k, factory)));
  assert(eager.present_keys() == keys);

  // Lazy reads [1,5] -> present set exactly {1,5}.
  SlotMap<uint32_t, uint32_t> lazy2(ctx);
  for (uint32_t k : {1u, 5u}) lazy2.get_or_insert_with(ctx, k, factory);
  assert((lazy2.present_keys() == std::vector<uint32_t>{1, 5}));
}

// conformance/materialization/deferral_not_deallocation.json
TEST(test_conformance_deferral_not_deallocation) {
  Context ctx;
  auto factory = [](const uint32_t& k) { return k * 2; };  // spec.val = k*2
  std::vector<uint32_t> keys{1, 2, 3, 4, 5};

  SlotMap<uint32_t, uint32_t> eager(ctx);
  eager.materialize_all(ctx, keys, factory);
  assert(eager.present_keys() == keys);
  for (uint32_t k : keys) assert(eager.get(ctx, k) == std::optional<uint32_t>(k * 2));

  // reads = [2,4,2,5]; present_after_each_read is monotone [1,2,2,3].
  SlotMap<uint32_t, uint32_t> lazy(ctx);
  std::vector<size_t> present_after_each_read;
  for (uint32_t k : {2u, 4u, 2u, 5u}) {
    assert(lazy.get_or_insert_with(ctx, k, factory) == k * 2);
    present_after_each_read.push_back(lazy.present_count());
  }
  assert((present_after_each_read == std::vector<size_t>{1, 2, 2, 3}));
  assert((lazy.present_keys() == std::vector<uint32_t>{2, 4, 5}));
  // Every lazily-present key is eagerly present.
  for (uint32_t k : lazy.present_keys()) assert(eager.is_present(k));
}

// conformance/materialization/entry_kind_orthogonal_to_mode.json
TEST(test_conformance_entry_kind) {
  // Entries: in_a/in_b are cells (val 5/7); der_x/der_y are slots (val 12/35).
  // A CellMap models the input side; a SlotMap models the derived side.
  Context ctx;
  auto cell_val = [](const std::string& k) -> uint32_t {
    return k == "in_a" ? 5 : 7;
  };
  auto slot_val = [](const std::string& k) -> uint32_t {
    return k == "der_x" ? 12 : 35;
  };

  CellMap<std::string, uint32_t> cells(ctx);
  cells.set(ctx, "in_a", cell_val("in_a"));
  cells.set(ctx, "in_b", cell_val("in_b"));
  assert(cells.entry_kind() == EntryKind::Cell);
  assert(cells.present_count() == 2);
  assert(cells.get(ctx, "in_a") == std::optional<uint32_t>(5));
  assert(cells.get(ctx, "in_b") == std::optional<uint32_t>(7));

  // Slots deferred until read.
  SlotMap<std::string, uint32_t> slots(ctx);
  assert(slots.entry_kind() == EntryKind::Slot);
  assert(slots.present_count() == 0);
  assert(slots.get_or_insert_with(ctx, "der_x", slot_val) == 12);
  assert(slots.is_present("der_x") && !slots.is_present("der_y"));
}

int main() {
  return test_count == test_passed ? 0 : 1;
}
