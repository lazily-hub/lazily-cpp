// AsyncReactiveMap keyed-collection tests (`#reactivemap`, async flavor).
//
// Mirrors the Rust reference tests in `lazily-rs/src/async_reactive_family.rs`.
// Input cells are always resolved; a derived slot reads `std::nullopt` until
// driven with `get_async()`. There is no eager/lazy mode flag.
//
// The two `spec.val` materialization fixtures were mirrored here by hand and now
// replay from the canonical bytes through this same async shell in
// `tests/test_materialization_family_conformance.cpp` (`#lzcppmatreplay`).

#include <lazily/lazily.hpp>

#include <cassert>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
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

// Drive a lazily-resolved async slot handle to its resolved value.
template <typename H> static uint32_t drive(H handle) { return handle.get_async().get(); }

static_assert(std::is_same_v<AsyncCellHandle<uint32_t>, AsyncSource<uint32_t>>);
static_assert(std::is_same_v<AsyncSlotHandle<uint32_t>, AsyncComputed<uint32_t>>);
static_assert(std::is_same_v<decltype(std::declval<AsyncContext&>().source(uint32_t{})),
                             AsyncSource<uint32_t>>);
static_assert(std::is_same_v<decltype(std::declval<AsyncContext&>().template computed<uint32_t>(
                                 std::declval<uint32_t (*)()>())),
                             AsyncComputed<uint32_t>>);

TEST(test_async_v2_names_and_legacy_factories_share_types) {
  AsyncContext ctx;
  auto source = ctx.source(uint32_t{2});
  auto computed = ctx.computed<uint32_t>([source]() mutable { return source.get() * 3; });
  assert(computed.get_async().get() == 6);
  assert(computed.state() == AsyncComputedState::Resolved);

  AsyncCellHandle<uint32_t> legacy_source = ctx.cell(uint32_t{4});
  AsyncSlotHandle<uint32_t> legacy_computed =
      ctx.slot<uint32_t>([legacy_source]() mutable { return legacy_source.get() + 1; });
  assert(legacy_computed.get_async().get() == 5);

  auto guarded =
      ctx.memo<uint32_t>([] { return uint32_t{7}; },
                         [](const uint32_t& left, const uint32_t& right) { return left == right; });
  static_assert(std::is_same_v<decltype(guarded), AsyncComputed<uint32_t>>);
  assert(guarded.get_async().get() == 7);
}

TEST(test_eager_source_map_resolves_immediately) {
  AsyncContext ctx;
  AsyncSourceMap<uint32_t, bool> fam(ctx);
  for (uint32_t k : {1u, 2u, 3u})
    fam.set(ctx, k, true);
  assert(fam.entry_kind() == EntryKind::Source);
  assert(fam.present_count() == 3);
  assert(fam.observe(ctx, 2) == std::optional<bool>(true));
  assert((fam.present_keys() == std::vector<uint32_t>{1, 2, 3}));
}

TEST(test_lazy_computed_map_defers_until_read) {
  AsyncContext ctx;
  AsyncComputedMap<uint32_t, uint32_t> fam(ctx);
  assert(fam.present_count() == 0);
  auto handle = fam.get_or_insert_handle(ctx, 4, [](const uint32_t& k) { return k * 10; });
  assert(fam.is_present(4));
  assert(fam.present_count() == 1);
  assert(fam.observe(ctx, 4) == std::nullopt); // pending until driven
  assert(drive(handle) == 40);
}

TEST(test_eventual_transparency_eager_equals_lazy) {
  AsyncContext ctx_e;
  AsyncComputedMap<uint32_t, uint32_t> eager(ctx_e);
  eager.materialize_all(ctx_e, {1, 2, 3}, [](const uint32_t& k) { return k * 2; });
  AsyncContext ctx_l;
  AsyncComputedMap<uint32_t, uint32_t> lazy(ctx_l);
  for (uint32_t k : {1u, 2u, 3u}) {
    uint32_t ve = drive(*eager.handle(k));
    uint32_t vl =
        drive(lazy.get_or_insert_handle(ctx_l, k, [](const uint32_t& kk) { return kk * 2; }));
    assert(ve == vl);
  }
}

TEST(test_present_set_grows_monotonically) {
  AsyncContext ctx;
  AsyncComputedMap<uint32_t, uint32_t> fam(ctx);
  auto id = [](const uint32_t& k) { return k; };
  (void)fam.get_or_insert_handle(ctx, 5, id);
  (void)fam.get_or_insert_handle(ctx, 5, id);
  (void)fam.get_or_insert_handle(ctx, 9, id);
  assert(fam.present_count() == 2);
  assert((fam.present_keys() == std::vector<uint32_t>{5, 9}));
}

TEST(test_source_map_reacts_to_set) {
  AsyncContext ctx;
  AsyncSourceMap<uint32_t, bool> fam(ctx);
  for (uint32_t k : {10u, 20u})
    fam.set(ctx, k, true);
  assert(fam.observe(ctx, 20) == std::optional<bool>(true));
  fam.set(ctx, 20, false);
  assert(fam.observe(ctx, 20) == std::optional<bool>(false));
}

// -- Spec conformance fixtures (replayed through the async map) --

// conformance/materialization/entry_kind_orthogonal_to_mode.json
TEST(test_conformance_entry_kind) {
  AsyncContext ctx;
  auto cell_val = [](const std::string& k) -> uint32_t { return k == "in_a" ? 5 : 7; };
  auto slot_val = [](const std::string& k) -> uint32_t { return k == "der_x" ? 12 : 35; };

  AsyncSourceMap<std::string, uint32_t> cells(ctx);
  cells.set(ctx, "in_a", cell_val("in_a"));
  cells.set(ctx, "in_b", cell_val("in_b"));
  assert((cells.present_keys() == std::vector<std::string>{"in_a", "in_b"}));
  assert(cells.observe(ctx, "in_a") == std::optional<uint32_t>(5));

  AsyncComputedMap<std::string, uint32_t> slots(ctx);
  assert(slots.present_count() == 0);
  assert(drive(slots.get_or_insert_handle(ctx, "der_x", slot_val)) == 12);
  assert(slots.is_present("der_x") && !slots.is_present("der_y"));
}

int main() { return test_count == test_passed ? 0 : 1; }
