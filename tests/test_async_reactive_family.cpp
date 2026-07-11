// AsyncReactiveFamily materialization-mode tests (`#lzmatmode`, async flavor).
//
// Mirrors the Rust reference tests in `lazily-rs/src/async_reactive_family.rs`
// and replays the shared lazily-spec materialization conformance fixtures
// through the async family: present-set laws plus EVENTUAL transparency (a driven
// async slot resolves to the canonical value, eager ≡ lazy). Input cells are
// always resolved; a derived slot reads `std::nullopt` until driven with
// `get_async()`.

#include <lazily/lazily.hpp>

#include <cassert>
#include <optional>
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

using SlotFamily = AsyncSlotFamily<uint32_t, uint32_t>;

// Drive a lazily-resolved async slot handle to its resolved value.
template <typename H>
static uint32_t drive(H handle) {
  return handle.get_async().get();
}

TEST(test_default_mode_is_eager) {
  assert(kDefaultMaterializationMode == MaterializationMode::Eager);
}

TEST(test_eager_cell_family_resolves_immediately) {
  AsyncContext ctx;
  auto fam = AsyncCellFamily<uint32_t, bool>::eager(
      ctx, {1, 2, 3}, [](const uint32_t&) { return true; });
  assert(fam.entry_kind() == EntryKind::Cell);
  assert(fam.present_count() == 3);
  assert(fam.observe(ctx, 2) == std::optional<bool>(true));
  assert((fam.present_keys() == std::vector<uint32_t>{1, 2, 3}));
}

TEST(test_lazy_slot_family_defers_until_read) {
  AsyncContext ctx;
  auto fam = SlotFamily::lazy(ctx, {},
                              [](const uint32_t& k) { return k * 10; });
  assert(fam.mode() == MaterializationMode::Lazy);
  assert(fam.present_count() == 0);
  auto handle = fam.get(ctx, 4);  // materialize (not yet resolved)
  assert(fam.is_present(4));
  assert(fam.present_count() == 1);
  assert(fam.observe(ctx, 4) == std::nullopt);  // pending until driven
  assert(drive(handle) == 40);
}

TEST(test_eventual_transparency_eager_equals_lazy) {
  AsyncContext ctx_e;
  auto eager = SlotFamily::eager(ctx_e, {1, 2, 3},
                                 [](const uint32_t& k) { return k * 2; });
  AsyncContext ctx_l;
  auto lazy = SlotFamily::lazy(ctx_l, {1, 2, 3},
                               [](const uint32_t& k) { return k * 2; });
  for (uint32_t k : {1u, 2u, 3u}) {
    uint32_t ve = drive(eager.get(ctx_e, k));
    uint32_t vl = drive(lazy.get(ctx_l, k));
    assert(ve == vl);
  }
}

TEST(test_present_set_grows_monotonically) {
  AsyncContext ctx;
  auto fam = SlotFamily::lazy(ctx, {}, [](const uint32_t& k) { return k; });
  (void)fam.get(ctx, 5);
  (void)fam.get(ctx, 5);
  (void)fam.get(ctx, 9);
  assert(fam.present_count() == 2);
  assert((fam.present_keys() == std::vector<uint32_t>{5, 9}));
}

TEST(test_cell_family_reacts_to_set) {
  AsyncContext ctx;
  auto fam = AsyncCellFamily<uint32_t, bool>::eager(
      ctx, {10, 20}, [](const uint32_t&) { return true; });
  assert(fam.observe(ctx, 20) == std::optional<bool>(true));
  auto h = fam.get(ctx, 20);
  h.set(false);
  assert(fam.observe(ctx, 20) == std::optional<bool>(false));
}

// -- Spec conformance fixtures (replayed through the async family) --

// conformance/materialization/observational_transparency.json (eventual)
TEST(test_conformance_observational_transparency) {
  AsyncContext ctx;
  auto factory = [](const uint32_t& k) { return k * 3; };
  std::vector<uint32_t> keys{0, 1, 2, 5, 9};
  auto eager = SlotFamily::eager(ctx, keys, factory);
  auto lazy = SlotFamily::lazy(ctx, keys, factory);
  assert(eager.present_count() == 5);
  assert(lazy.present_count() == 0);
  for (uint32_t k : keys) assert(drive(eager.get(ctx, k)) == drive(lazy.get(ctx, k)));
  assert(eager.present_keys() == keys);
}

// conformance/materialization/deferral_not_deallocation.json
TEST(test_conformance_deferral_not_deallocation) {
  AsyncContext ctx;
  auto factory = [](const uint32_t& k) { return k * 2; };
  std::vector<uint32_t> keys{1, 2, 3, 4, 5};
  auto lazy = SlotFamily::lazy(ctx, keys, factory);
  std::vector<size_t> present_after_each_read;
  for (uint32_t k : {2u, 4u, 2u, 5u}) {
    assert(drive(lazy.get(ctx, k)) == k * 2);
    present_after_each_read.push_back(lazy.present_count());
  }
  assert((present_after_each_read == std::vector<size_t>{1, 2, 2, 3}));
  assert((lazy.present_keys() == std::vector<uint32_t>{2, 4, 5}));
}

// conformance/materialization/entry_kind_orthogonal_to_mode.json
TEST(test_conformance_entry_kind_orthogonal_to_mode) {
  AsyncContext ctx;
  auto cell_val = [](const std::string& k) -> uint32_t {
    return k == "in_a" ? 5 : 7;
  };
  auto slot_val = [](const std::string& k) -> uint32_t {
    return k == "der_x" ? 12 : 35;
  };
  using CellF = AsyncCellFamily<std::string, uint32_t>;
  using SlotF = AsyncSlotFamily<std::string, uint32_t>;

  auto cells_l = CellF::lazy(ctx, {"in_a", "in_b"}, cell_val);
  auto slots_l = SlotF::lazy(ctx, {"der_x", "der_y"}, slot_val);
  // Cell entries present at build; slot entries deferred.
  assert((cells_l.present_keys() == std::vector<std::string>{"in_a", "in_b"}));
  assert(slots_l.present_count() == 0);
  assert(cells_l.observe(ctx, "in_a") == std::optional<uint32_t>(5));
  // A read materializes + (once driven) resolves der_x only.
  assert(drive(slots_l.get(ctx, "der_x")) == 12);
  assert(slots_l.is_present("der_x") && !slots_l.is_present("der_y"));
}

int main() { return test_count == test_passed ? 0 : 1; }
