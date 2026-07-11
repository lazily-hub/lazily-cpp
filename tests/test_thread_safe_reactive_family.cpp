// ThreadSafeReactiveFamily materialization-mode tests (`#lzmatmode`,
// thread-safe flavor).
//
// Mirrors the Rust reference tests in
// `lazily-rs/src/thread_safe_reactive_family.rs` and replays the shared
// lazily-spec materialization conformance fixtures
// (`conformance/materialization/*`) through the `Send + Sync` family, proving it
// obeys the shared laws plus materialization confluence (order-independent
// present set + observed values) across threads.

#include <lazily/lazily.hpp>

#include <cassert>
#include <string>
#include <thread>
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

using SlotFamily = ThreadSafeSlotFamily<uint32_t, uint32_t>;
using CellFam = ThreadSafeCellFamily<std::string, uint32_t>;

// -- Rust-parity unit tests --

TEST(test_default_mode_is_eager) {
  assert(kDefaultMaterializationMode == MaterializationMode::Eager);
}

TEST(test_eager_cell_family_materializes_all_at_build) {
  ThreadSafeContext ctx;
  auto fam = ThreadSafeCellFamily<uint32_t, bool>::eager(
      ctx, {1, 2, 3}, [](const uint32_t&) { return true; });
  assert(fam.entry_kind() == EntryKind::Cell);
  assert(fam.mode() == MaterializationMode::Eager);
  assert(fam.present_count() == 3);
  assert(fam.is_present(1) && fam.is_present(2) && fam.is_present(3));
  assert((fam.present_keys() == std::vector<uint32_t>{1, 2, 3}));
}

TEST(test_lazy_slot_family_defers_until_read) {
  ThreadSafeContext ctx;
  auto fam = SlotFamily::lazy(ctx, {},
                              [](const uint32_t& k) { return k * 10; });
  assert(fam.mode() == MaterializationMode::Lazy);
  assert(fam.present_count() == 0);
  assert(!fam.is_present(2));
  assert(fam.observe(ctx, 2) == 20);
  assert(fam.is_present(2));
  assert(fam.present_count() == 1);
}

TEST(test_lazy_cell_entries_still_materialize_at_build) {
  ThreadSafeContext ctx;
  auto fam = ThreadSafeCellFamily<uint32_t, bool>::lazy(
      ctx, {7, 8}, [](const uint32_t&) { return false; });
  assert(fam.present_count() == 2);
}

TEST(test_observational_transparency_eager_equals_lazy) {
  ThreadSafeContext ctx_e;
  auto eager = SlotFamily::eager(ctx_e, {1, 2, 3},
                                 [](const uint32_t& k) { return k * 2; });
  ThreadSafeContext ctx_l;
  auto lazy = SlotFamily::lazy(ctx_l, {1, 2, 3},
                               [](const uint32_t& k) { return k * 2; });
  for (uint32_t k : {1u, 2u, 3u})
    assert(eager.observe(ctx_e, k) == lazy.observe(ctx_l, k));
}

TEST(test_present_set_grows_monotonically) {
  ThreadSafeContext ctx;
  auto fam = SlotFamily::lazy(ctx, {}, [](const uint32_t& k) { return k; });
  (void)fam.observe(ctx, 5);
  (void)fam.observe(ctx, 5);  // repeat: no growth
  (void)fam.observe(ctx, 9);
  assert(fam.present_count() == 2);
  assert((fam.present_keys() == std::vector<uint32_t>{5, 9}));
}

// The agent-doc liveness shape: cell inputs + a derived count that recomputes
// reactively when a cell flips.
TEST(test_derived_count_reacts_to_cell_writes) {
  ThreadSafeContext ctx;
  auto liveness = ThreadSafeCellFamily<uint32_t, bool>::eager(
      ctx, {10, 20, 30}, [](const uint32_t&) { return true; });
  // All cells are eagerly materialized; capture their handles and let a derived
  // count recompute reactively when one flips (no pull-time scan).
  std::vector<CellHandle<bool>> handles;
  for (uint32_t k : {10u, 20u, 30u}) handles.push_back(liveness.get(ctx, k));
  auto live_count = ctx.computed<int>([handles](Context& c) {
    int n = 0;
    for (const auto& h : handles)
      if (c.get_cell(h)) ++n;
    return n;
  });
  assert(ctx.get(live_count) == 3);
  ctx.set_cell(handles[1], false);
  assert(ctx.get(live_count) == 2);
  ctx.set_cell(handles[1], true);
  assert(ctx.get(live_count) == 3);
}

// The whole point of the thread-safe flavor: a family shared across threads.
TEST(test_shared_across_threads) {
  ThreadSafeContext ctx;
  auto fam = ThreadSafeCellFamily<uint32_t, bool>::eager(
      ctx, {1, 2, 3, 4}, [](const uint32_t&) { return true; });
  std::vector<std::thread> threads;
  std::vector<char> results(4, 0);
  for (uint32_t k = 1; k <= 4; ++k) {
    threads.emplace_back([&, k]() { results[k - 1] = fam.observe(ctx, k); });
  }
  for (auto& t : threads) t.join();
  for (char r : results) assert(r);
  assert(fam.present_count() == 4);
}

// -- Spec conformance fixtures (replayed through the thread-safe family) --

// conformance/materialization/observational_transparency.json
TEST(test_conformance_observational_transparency) {
  ThreadSafeContext ctx;
  auto factory = [](const uint32_t& k) { return k * 3; };
  std::vector<uint32_t> keys{0, 1, 2, 5, 9};
  auto eager = SlotFamily::eager(ctx, keys, factory);
  auto lazy = SlotFamily::lazy(ctx, keys, factory);
  assert(eager.mode() == MaterializationMode::Eager);
  assert(eager.present_count() == 5);
  assert(lazy.present_count() == 0);
  for (uint32_t k : keys) assert(eager.observe(ctx, k) == lazy.observe(ctx, k));
  assert(eager.present_keys() == keys);

  auto lazy2 = SlotFamily::lazy(ctx, keys, factory);
  for (uint32_t k : {1u, 5u}) lazy2.observe(ctx, k);
  assert((lazy2.present_keys() == std::vector<uint32_t>{1, 5}));
}

// conformance/materialization/deferral_not_deallocation.json
TEST(test_conformance_deferral_not_deallocation) {
  ThreadSafeContext ctx;
  auto factory = [](const uint32_t& k) { return k * 2; };
  std::vector<uint32_t> keys{1, 2, 3, 4, 5};
  auto eager = SlotFamily::eager(ctx, keys, factory);
  assert(eager.present_keys() == keys);
  for (uint32_t k : keys) assert(eager.observe(ctx, k) == k * 2);

  auto lazy = SlotFamily::lazy(ctx, keys, factory);
  std::vector<size_t> present_after_each_read;
  for (uint32_t k : {2u, 4u, 2u, 5u}) {
    assert(lazy.observe(ctx, k) == k * 2);
    present_after_each_read.push_back(lazy.present_count());
  }
  assert((present_after_each_read == std::vector<size_t>{1, 2, 2, 3}));
  assert((lazy.present_keys() == std::vector<uint32_t>{2, 4, 5}));
  for (uint32_t k : lazy.present_keys()) assert(eager.is_present(k));
}

// conformance/materialization/entry_kind_orthogonal_to_mode.json
TEST(test_conformance_entry_kind_orthogonal_to_mode) {
  ThreadSafeContext ctx;
  auto cell_val = [](const std::string& k) -> uint32_t {
    return k == "in_a" ? 5 : 7;
  };
  auto slot_val = [](const std::string& k) -> uint32_t {
    return k == "der_x" ? 12 : 35;
  };
  std::vector<std::string> cell_keys{"in_a", "in_b"};
  std::vector<std::string> slot_keys{"der_x", "der_y"};
  using SlotF = ThreadSafeSlotFamily<std::string, uint32_t>;

  auto cells_e = CellFam::eager(ctx, cell_keys, cell_val);
  auto slots_e = SlotF::eager(ctx, slot_keys, slot_val);
  assert(cells_e.present_count() == 2 && slots_e.present_count() == 2);

  auto cells_l = CellFam::lazy(ctx, cell_keys, cell_val);
  auto slots_l = SlotF::lazy(ctx, slot_keys, slot_val);
  assert((cells_l.present_keys() == std::vector<std::string>{"in_a", "in_b"}));
  assert(slots_l.present_count() == 0);
  assert(cells_l.entry_kind() == EntryKind::Cell);
  assert(slots_l.entry_kind() == EntryKind::Slot);
  assert(slots_l.observe(ctx, "der_x") == 12);
  assert(slots_l.is_present("der_x") && !slots_l.is_present("der_y"));
  assert(cells_e.observe(ctx, "in_a") == cells_l.observe(ctx, "in_a"));
  assert(slots_e.observe(ctx, "der_x") == slots_l.observe(ctx, "der_x"));
}

int main() { return test_count == test_passed ? 0 : 1; }
