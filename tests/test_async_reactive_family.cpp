// AsyncReactiveMap keyed-collection tests (`#reactivemap`, async flavor).
//
// Mirrors the Rust reference tests in `lazily-rs/src/async_reactive_family.rs`
// and replays the shared lazily-spec materialization conformance fixtures
// (`"model": "SlotMap"`) through the async map: present-set laws plus EVENTUAL
// transparency (a driven async slot resolves to the canonical value, eager
// pre-mint ≡ lazy mint-on-access). Input cells are always resolved; a derived
// slot reads `std::nullopt` until driven with `get_async()`. There is no
// eager/lazy mode flag.

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

// Drive a lazily-resolved async slot handle to its resolved value.
template <typename H>
static uint32_t drive(H handle) {
  return handle.get_async().get();
}

TEST(test_eager_cell_map_resolves_immediately) {
  AsyncContext ctx;
  AsyncCellMap<uint32_t, bool> fam(ctx);
  for (uint32_t k : {1u, 2u, 3u}) fam.set(ctx, k, true);
  assert(fam.entry_kind() == EntryKind::Cell);
  assert(fam.present_count() == 3);
  assert(fam.observe(ctx, 2) == std::optional<bool>(true));
  assert((fam.present_keys() == std::vector<uint32_t>{1, 2, 3}));
}

TEST(test_lazy_slot_map_defers_until_read) {
  AsyncContext ctx;
  AsyncSlotMap<uint32_t, uint32_t> fam(ctx);
  assert(fam.present_count() == 0);
  auto handle = fam.get_or_insert_handle(ctx, 4,
                                         [](const uint32_t& k) { return k * 10; });
  assert(fam.is_present(4));
  assert(fam.present_count() == 1);
  assert(fam.observe(ctx, 4) == std::nullopt);  // pending until driven
  assert(drive(handle) == 40);
}

TEST(test_eventual_transparency_eager_equals_lazy) {
  AsyncContext ctx_e;
  AsyncSlotMap<uint32_t, uint32_t> eager(ctx_e);
  eager.materialize_all(ctx_e, {1, 2, 3}, [](const uint32_t& k) { return k * 2; });
  AsyncContext ctx_l;
  AsyncSlotMap<uint32_t, uint32_t> lazy(ctx_l);
  for (uint32_t k : {1u, 2u, 3u}) {
    uint32_t ve = drive(*eager.handle(k));
    uint32_t vl = drive(lazy.get_or_insert_handle(ctx_l, k,
                                                  [](const uint32_t& kk) { return kk * 2; }));
    assert(ve == vl);
  }
}

TEST(test_present_set_grows_monotonically) {
  AsyncContext ctx;
  AsyncSlotMap<uint32_t, uint32_t> fam(ctx);
  auto id = [](const uint32_t& k) { return k; };
  (void)fam.get_or_insert_handle(ctx, 5, id);
  (void)fam.get_or_insert_handle(ctx, 5, id);
  (void)fam.get_or_insert_handle(ctx, 9, id);
  assert(fam.present_count() == 2);
  assert((fam.present_keys() == std::vector<uint32_t>{5, 9}));
}

TEST(test_cell_map_reacts_to_set) {
  AsyncContext ctx;
  AsyncCellMap<uint32_t, bool> fam(ctx);
  for (uint32_t k : {10u, 20u}) fam.set(ctx, k, true);
  assert(fam.observe(ctx, 20) == std::optional<bool>(true));
  fam.set(ctx, 20, false);
  assert(fam.observe(ctx, 20) == std::optional<bool>(false));
}

// -- Spec conformance fixtures (replayed through the async map) --

// conformance/materialization/observational_transparency.json (eventual)
TEST(test_conformance_observational_transparency) {
  AsyncContext ctx;
  auto factory = [](const uint32_t& k) { return k * 3; };
  std::vector<uint32_t> keys{0, 1, 2, 5, 9};
  AsyncSlotMap<uint32_t, uint32_t> eager(ctx);
  eager.materialize_all(ctx, keys, factory);
  AsyncSlotMap<uint32_t, uint32_t> lazy(ctx);
  assert(eager.present_count() == 5);
  assert(lazy.present_count() == 0);
  for (uint32_t k : keys)
    assert(drive(*eager.handle(k)) == drive(lazy.get_or_insert_handle(ctx, k, factory)));
  assert(eager.present_keys() == keys);
}

// conformance/materialization/deferral_not_deallocation.json
TEST(test_conformance_deferral_not_deallocation) {
  AsyncContext ctx;
  auto factory = [](const uint32_t& k) { return k * 2; };
  AsyncSlotMap<uint32_t, uint32_t> lazy(ctx);
  std::vector<size_t> present_after_each_read;
  for (uint32_t k : {2u, 4u, 2u, 5u}) {
    assert(drive(lazy.get_or_insert_handle(ctx, k, factory)) == k * 2);
    present_after_each_read.push_back(lazy.present_count());
  }
  assert((present_after_each_read == std::vector<size_t>{1, 2, 2, 3}));
  assert((lazy.present_keys() == std::vector<uint32_t>{2, 4, 5}));
}

// conformance/materialization/entry_kind_orthogonal_to_mode.json
TEST(test_conformance_entry_kind) {
  AsyncContext ctx;
  auto cell_val = [](const std::string& k) -> uint32_t {
    return k == "in_a" ? 5 : 7;
  };
  auto slot_val = [](const std::string& k) -> uint32_t {
    return k == "der_x" ? 12 : 35;
  };

  AsyncCellMap<std::string, uint32_t> cells(ctx);
  cells.set(ctx, "in_a", cell_val("in_a"));
  cells.set(ctx, "in_b", cell_val("in_b"));
  assert((cells.present_keys() == std::vector<std::string>{"in_a", "in_b"}));
  assert(cells.observe(ctx, "in_a") == std::optional<uint32_t>(5));

  AsyncSlotMap<std::string, uint32_t> slots(ctx);
  assert(slots.present_count() == 0);
  assert(drive(slots.get_or_insert_handle(ctx, "der_x", slot_val)) == 12);
  assert(slots.is_present("der_x") && !slots.is_present("der_y"));
}

int main() { return test_count == test_passed ? 0 : 1; }
