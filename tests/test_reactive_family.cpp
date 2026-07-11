// ReactiveFamily materialization-mode tests (`#lzmatmode`).
//
// Mirrors the Rust reference tests in `lazily-rs/src/reactive_family.rs` and the
// lazily-spec conformance fixtures in `conformance/materialization/*`:
//   - observational_transparency.json
//   - deferral_not_deallocation.json
//   - entry_kind_orthogonal_to_mode.json

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

using SlotFamily = ReactiveFamily<uint32_t, uint32_t, SlotHandle<uint32_t>>;
using CellFam = ReactiveFamily<std::string, uint32_t, CellHandle<uint32_t>>;

// -- Rust-parity unit tests --

// `default_mode_eager`: the default materialization mode is eager.
TEST(test_default_mode_is_eager) {
  assert(kDefaultMaterializationMode == MaterializationMode::Eager);
}

// `eager_materializes_all`: eager allocates every declared node up front.
TEST(test_eager_materializes_all_up_front) {
  Context ctx;
  auto fam = SlotFamily::eager(ctx, {0, 1, 2, 5, 9},
                               [](const uint32_t& k) { return k * 3; });
  assert(fam.mode() == MaterializationMode::Eager);
  assert(fam.entry_kind() == EntryKind::Slot);
  assert(fam.present_count() == 5);
  for (uint32_t k : {0u, 1u, 2u, 5u, 9u}) assert(fam.is_present(k));
}

// `lazy_defers_slots`: lazy leaves an unread derived slot unallocated.
TEST(test_lazy_defers_slots_until_read) {
  Context ctx;
  auto fam = SlotFamily::lazy(ctx, {0, 1, 2, 5, 9},
                              [](const uint32_t& k) { return k * 3; });
  assert(fam.mode() == MaterializationMode::Lazy);
  assert(fam.present_count() == 0);
  assert(!fam.is_present(5));

  // First read materializes just that key ("materialize on pull").
  assert(fam.observe(ctx, 5) == 15);
  assert(fam.is_present(5));
  assert(fam.present_keys() == std::vector<uint32_t>{5});
}

// `eager_lazy_observationally_equivalent`: identical values under either mode.
TEST(test_eager_and_lazy_observe_identically) {
  Context ctx;
  auto eager = SlotFamily::eager(ctx, {0, 1, 2, 5, 9},
                                 [](const uint32_t& k) { return k * 3; });
  auto lazy = SlotFamily::lazy(ctx, {0, 1, 2, 5, 9},
                               [](const uint32_t& k) { return k * 3; });
  for (uint32_t k : {0u, 1u, 2u, 5u, 9u})
    assert(eager.observe(ctx, k) == lazy.observe(ctx, k));
}

// `materialize_present_monotone`: re-reading a key does not change the present
// set; the set only grows.
TEST(test_present_set_is_monotone_across_reads) {
  Context ctx;
  auto fam = SlotFamily::lazy(ctx, {1, 2, 3, 4, 5},
                              [](const uint32_t& k) { return k * 2; });
  std::vector<size_t> sizes;
  for (uint32_t k : {2u, 4u, 2u, 5u}) {
    fam.observe(ctx, k);
    sizes.push_back(fam.present_count());
  }
  // Re-reading 2 does not re-materialize; sizes are non-decreasing.
  assert((sizes == std::vector<size_t>{1, 2, 2, 3}));
  assert((fam.present_keys() == std::vector<uint32_t>{2, 4, 5}));
}

// `cell_entries_materialized_in_every_mode`: an input-cell family is fully
// materialized at build under **either** mode.
TEST(test_cell_family_materialized_in_every_mode) {
  Context ctx;
  for (bool mode_lazy : {false, true}) {
    std::vector<std::string> keys{"a", "b", "c"};
    auto fam = mode_lazy
                   ? CellFam::lazy(ctx, keys, [](const std::string&) { return 0u; })
                   : CellFam::eager(ctx, keys, [](const std::string&) { return 0u; });
    assert(fam.entry_kind() == EntryKind::Cell);
    // Cells are always present at build, even under lazy.
    assert(fam.present_count() == 3);
  }
}

// Cell entries are writable inputs (materialized-by-set), distinct from slots.
TEST(test_cell_family_entries_are_writable_inputs) {
  Context ctx;
  auto fam = ReactiveFamily<uint32_t, uint32_t, CellHandle<uint32_t>>::eager(
      ctx, {7}, [](const uint32_t& k) { return k; });
  auto h = fam.get(ctx, 7);
  assert(ctx.get_cell(h) == 7);
  ctx.set_cell(h, 100u);
  assert(fam.observe(ctx, 7) == 100);
}

// -- Spec conformance fixtures --

// conformance/materialization/observational_transparency.json
TEST(test_conformance_observational_transparency) {
  Context ctx;
  auto factory = [](const uint32_t& k) { return k * 3; };  // spec.val = k*3
  std::vector<uint32_t> keys{0, 1, 2, 5, 9};

  auto eager = SlotFamily::eager(ctx, keys, factory);
  auto lazy = SlotFamily::lazy(ctx, keys, factory);

  // default_mode eager; eager materializes all; lazy defers.
  assert(eager.mode() == MaterializationMode::Eager);
  assert(eager.present_count() == 5);
  assert(lazy.present_count() == 0);

  // observe: identical canonical values under either mode.
  for (uint32_t k : keys) {
    assert(eager.observe(ctx, k) == lazy.observe(ctx, k));
  }
  // eager_present == all keys.
  assert(eager.present_keys() == keys);

  // Re-run lazy for the fixture `reads` sequence to check its present set.
  auto lazy2 = SlotFamily::lazy(ctx, keys, factory);
  for (uint32_t k : {1u, 5u}) lazy2.observe(ctx, k);
  assert((lazy2.present_keys() == std::vector<uint32_t>{1, 5}));
}

// conformance/materialization/deferral_not_deallocation.json
TEST(test_conformance_deferral_not_deallocation) {
  Context ctx;
  auto factory = [](const uint32_t& k) { return k * 2; };  // spec.val = k*2
  std::vector<uint32_t> keys{1, 2, 3, 4, 5};

  // default_mode eager; eager_present is every key.
  auto eager = SlotFamily::eager(ctx, keys, factory);
  assert(eager.mode() == MaterializationMode::Eager);
  assert(eager.present_keys() == keys);

  // observe map holds for every key.
  for (uint32_t k : keys) assert(eager.observe(ctx, k) == k * 2);

  // reads = [2,4,2,5]; present_after_each_read is monotone [1,2,2,3];
  // final lazy present set = [2,4,5], a subset of eager_present.
  auto lazy = SlotFamily::lazy(ctx, keys, factory);
  std::vector<size_t> present_after_each_read;
  for (uint32_t k : {2u, 4u, 2u, 5u}) {
    // materialize_preserves_observe: reading one never changes another's value.
    assert(lazy.observe(ctx, k) == k * 2);
    present_after_each_read.push_back(lazy.present_count());
  }
  assert((present_after_each_read == std::vector<size_t>{1, 2, 2, 3}));
  assert((lazy.present_keys() == std::vector<uint32_t>{2, 4, 5}));
  // lazy_present_subset_eager: every lazily-present key is eagerly present.
  for (uint32_t k : lazy.present_keys()) assert(eager.is_present(k));
}

// conformance/materialization/entry_kind_orthogonal_to_mode.json
TEST(test_conformance_entry_kind_orthogonal_to_mode) {
  // Entries: in_a/in_b are cells (val 5/7); der_x/der_y are slots (val 12/35).
  // C++ families are single-kind (H is fixed), so we model the two kinds as two
  // families sharing the key space, exactly as the fixture's `spec.entries`
  // splits by kind.
  Context ctx;
  auto cell_val = [](const std::string& k) -> uint32_t {
    return k == "in_a" ? 5 : 7;
  };
  auto slot_val = [](const std::string& k) -> uint32_t {
    return k == "der_x" ? 12 : 35;
  };
  std::vector<std::string> cell_keys{"in_a", "in_b"};
  std::vector<std::string> slot_keys{"der_x", "der_y"};

  using CellF = ReactiveFamily<std::string, uint32_t, CellHandle<uint32_t>>;
  using SlotF = ReactiveFamily<std::string, uint32_t, SlotHandle<uint32_t>>;

  // Eager: all entries present; reads mode-independent.
  auto cells_e = CellF::eager(ctx, cell_keys, cell_val);
  auto slots_e = SlotF::eager(ctx, slot_keys, slot_val);
  assert(cells_e.present_count() == 2 && slots_e.present_count() == 2);
  assert(cells_e.observe(ctx, "in_a") == 5 && cells_e.observe(ctx, "in_b") == 7);
  assert(slots_e.observe(ctx, "der_x") == 12 && slots_e.observe(ctx, "der_y") == 35);

  // Lazy: cell entries present at build; slot entries deferred until read.
  auto cells_l = CellF::lazy(ctx, cell_keys, cell_val);
  auto slots_l = SlotF::lazy(ctx, slot_keys, slot_val);
  // lazy_present_at_build (cell side) = [in_a, in_b].
  assert((cells_l.present_keys() == std::vector<std::string>{"in_a", "in_b"}));
  assert(slots_l.present_count() == 0);  // slots absent at build.
  assert(cells_l.entry_kind() == EntryKind::Cell);
  assert(slots_l.entry_kind() == EntryKind::Slot);

  // reads = [der_x]: lazy_present_after_reads adds der_x to the cell set.
  assert(slots_l.observe(ctx, "der_x") == 12);
  assert(slots_l.is_present("der_x") && !slots_l.is_present("der_y"));
  // Observed values are mode-independent.
  assert(cells_e.observe(ctx, "in_a") == cells_l.observe(ctx, "in_a"));
  assert(slots_e.observe(ctx, "der_x") == slots_l.observe(ctx, "der_x"));
}

int main() {
  return test_count == test_passed ? 0 : 1;
}
