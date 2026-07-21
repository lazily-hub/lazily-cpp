// ThreadSafeReactiveMap keyed-collection tests (`#reactivemap`, thread-safe
// flavor).
//
// Mirrors the Rust reference tests in
// `lazily-rs/src/thread_safe_reactive_family.rs` and replays the shared
// lazily-spec materialization conformance fixtures (`conformance/materialization/*`,
// `"model": "SlotMap"`) through the `Send + Sync` map: eager pre-mint
// (`materialize_all`) vs. lazy mint-on-access (`get_or_insert_*`), observational
// transparency, and present-set monotonicity across threads. There is no
// eager/lazy mode flag.

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

// -- Rust-parity unit tests --

TEST(test_eager_cell_map_materializes_all_via_set) {
  ThreadSafeContext ctx;
  ThreadSafeCellMap<uint32_t, bool> fam(ctx);
  for (uint32_t k : {1u, 2u, 3u}) fam.set(ctx, k, true);
  assert(fam.entry_kind() == EntryKind::Cell);
  assert(fam.present_count() == 3);
  assert(fam.is_present(1) && fam.is_present(2) && fam.is_present(3));
  assert((fam.present_keys() == std::vector<uint32_t>{1, 2, 3}));
}

TEST(test_lazy_slot_map_defers_until_read) {
  ThreadSafeContext ctx;
  ThreadSafeSlotMap<uint32_t, uint32_t> fam(ctx);
  assert(fam.present_count() == 0);
  assert(!fam.is_present(2));
  assert(fam.get_or_insert_with(ctx, 2, [](const uint32_t& k) { return k * 10; }) ==
         20);
  assert(fam.is_present(2));
  assert(fam.present_count() == 1);
}

TEST(test_eager_slot_map_materializes_all_up_front) {
  ThreadSafeContext ctx;
  ThreadSafeSlotMap<uint32_t, uint32_t> fam(ctx);
  fam.materialize_all(ctx, {7, 8}, [](const uint32_t& k) { return k; });
  assert(fam.present_count() == 2);
}

TEST(test_observational_transparency_eager_equals_lazy) {
  ThreadSafeContext ctx_e;
  ThreadSafeSlotMap<uint32_t, uint32_t> eager(ctx_e);
  eager.materialize_all(ctx_e, {1, 2, 3}, [](const uint32_t& k) { return k * 2; });
  ThreadSafeContext ctx_l;
  ThreadSafeSlotMap<uint32_t, uint32_t> lazy(ctx_l);
  for (uint32_t k : {1u, 2u, 3u}) {
    auto ve = eager.observe(ctx_e, k);
    auto vl = lazy.get_or_insert_with(ctx_l, k, [](const uint32_t& kk) { return kk * 2; });
    assert(ve == std::optional<uint32_t>(vl));
  }
}

TEST(test_present_set_grows_monotonically) {
  ThreadSafeContext ctx;
  ThreadSafeSlotMap<uint32_t, uint32_t> fam(ctx);
  auto id = [](const uint32_t& k) { return k; };
  (void)fam.get_or_insert_with(ctx, 5, id);
  (void)fam.get_or_insert_with(ctx, 5, id);  // repeat: no growth
  (void)fam.get_or_insert_with(ctx, 9, id);
  assert(fam.present_count() == 2);
  assert((fam.present_keys() == std::vector<uint32_t>{5, 9}));
}

// The agent-doc liveness shape: cell inputs + a derived count that recomputes
// reactively when a cell flips.
TEST(test_derived_count_reacts_to_cell_writes) {
  ThreadSafeContext ctx;
  ThreadSafeCellMap<uint32_t, bool> liveness(ctx);
  for (uint32_t k : {10u, 20u, 30u}) liveness.set(ctx, k, true);
  std::vector<CellHandle<bool>> handles;
  for (uint32_t k : {10u, 20u, 30u}) handles.push_back(*liveness.handle(k));
  auto live_count = ctx.memo<int>([handles](Context& c) {
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

// The whole point of the thread-safe flavor: a map shared across threads.
TEST(test_shared_across_threads) {
  ThreadSafeContext ctx;
  ThreadSafeCellMap<uint32_t, bool> fam(ctx);
  for (uint32_t k : {1u, 2u, 3u, 4u}) fam.set(ctx, k, true);
  std::vector<std::thread> threads;
  std::vector<char> results(4, 0);
  for (uint32_t k = 1; k <= 4; ++k) {
    threads.emplace_back(
        [&, k]() { results[k - 1] = fam.observe(ctx, k).value_or(false); });
  }
  for (auto& t : threads) t.join();
  for (char r : results) assert(r);
  assert(fam.present_count() == 4);
}

// -- Spec conformance fixtures (replayed through the thread-safe map) --

// conformance/materialization/observational_transparency.json
TEST(test_conformance_observational_transparency) {
  ThreadSafeContext ctx;
  auto factory = [](const uint32_t& k) { return k * 3; };
  std::vector<uint32_t> keys{0, 1, 2, 5, 9};
  ThreadSafeSlotMap<uint32_t, uint32_t> eager(ctx);
  eager.materialize_all(ctx, keys, factory);
  ThreadSafeSlotMap<uint32_t, uint32_t> lazy(ctx);
  assert(eager.present_count() == 5);
  assert(lazy.present_count() == 0);
  for (uint32_t k : keys)
    assert(eager.observe(ctx, k) ==
           std::optional<uint32_t>(lazy.get_or_insert_with(ctx, k, factory)));
  assert(eager.present_keys() == keys);

  ThreadSafeSlotMap<uint32_t, uint32_t> lazy2(ctx);
  for (uint32_t k : {1u, 5u}) lazy2.get_or_insert_with(ctx, k, factory);
  assert((lazy2.present_keys() == std::vector<uint32_t>{1, 5}));
}

// conformance/materialization/deferral_not_deallocation.json
TEST(test_conformance_deferral_not_deallocation) {
  ThreadSafeContext ctx;
  auto factory = [](const uint32_t& k) { return k * 2; };
  std::vector<uint32_t> keys{1, 2, 3, 4, 5};
  ThreadSafeSlotMap<uint32_t, uint32_t> eager(ctx);
  eager.materialize_all(ctx, keys, factory);
  assert(eager.present_keys() == keys);
  for (uint32_t k : keys) assert(eager.observe(ctx, k) == std::optional<uint32_t>(k * 2));

  ThreadSafeSlotMap<uint32_t, uint32_t> lazy(ctx);
  std::vector<size_t> present_after_each_read;
  for (uint32_t k : {2u, 4u, 2u, 5u}) {
    assert(lazy.get_or_insert_with(ctx, k, factory) == k * 2);
    present_after_each_read.push_back(lazy.present_count());
  }
  assert((present_after_each_read == std::vector<size_t>{1, 2, 2, 3}));
  assert((lazy.present_keys() == std::vector<uint32_t>{2, 4, 5}));
  for (uint32_t k : lazy.present_keys()) assert(eager.is_present(k));
}

// conformance/materialization/entry_kind_orthogonal_to_mode.json
TEST(test_conformance_entry_kind) {
  ThreadSafeContext ctx;
  auto cell_val = [](const std::string& k) -> uint32_t {
    return k == "in_a" ? 5 : 7;
  };
  auto slot_val = [](const std::string& k) -> uint32_t {
    return k == "der_x" ? 12 : 35;
  };

  ThreadSafeCellMap<std::string, uint32_t> cells(ctx);
  cells.set(ctx, "in_a", cell_val("in_a"));
  cells.set(ctx, "in_b", cell_val("in_b"));
  assert(cells.present_count() == 2);
  assert(cells.entry_kind() == EntryKind::Cell);

  ThreadSafeSlotMap<std::string, uint32_t> slots(ctx);
  assert(slots.present_count() == 0);
  assert(slots.entry_kind() == EntryKind::Slot);
  assert(slots.get_or_insert_with(ctx, "der_x", slot_val) == 12);
  assert(slots.is_present("der_x") && !slots.is_present("der_y"));
  assert(cells.observe(ctx, "in_a") == std::optional<uint32_t>(5));
}

int main() { return test_count == test_passed ? 0 : 1; }
