// Phase 1 law-tests for the merge algebra (#relaycell). Every policy MUST be
// associative; commutativity/idempotency are asserted per flag. The converged-
// determinism cases mirror lazily-spec/conformance/collections/
// mergecell_algebra.json — lazily-cpp converges identically to the other bindings.

#include <lazily/lazily.hpp>

#include <cassert>
#include <iostream>
#include <set>
#include <vector>

using namespace lazily;

static int test_count = 0;
static int test_passed = 0;

#define TEST(name)                     \
  static void name();                  \
  struct name##_runner {               \
    name##_runner() {                  \
      ++test_count;                    \
      name();                          \
      ++test_passed;                   \
    }                                  \
  } name##_instance;                   \
  static void name()

TEST(test_merge_associativity) {
  assert(KeepLatest::merge<long>(KeepLatest::merge<long>(5, -3), 8) ==
         KeepLatest::merge<long>(5, KeepLatest::merge<long>(-3, 8)));
  assert(Sum::merge<long>(Sum::merge<long>(5, -3), 8) ==
         Sum::merge<long>(5, Sum::merge<long>(-3, 8)));
  assert(Max::merge<long>(Max::merge<long>(5, -3), 8) ==
         Max::merge<long>(5, Max::merge<long>(-3, 8)));
  std::vector<int> a{1}, b{2}, c{3};
  assert(RawFifo::merge<std::vector<int>>(RawFifo::merge<std::vector<int>>(a, b), c) ==
         RawFifo::merge<std::vector<int>>(a, RawFifo::merge<std::vector<int>>(b, c)));
}

TEST(test_merge_commutativity_flags) {
  static_assert(Sum::commutative && Max::commutative, "Sum/Max commutative");
  static_assert(!KeepLatest::commutative, "KeepLatest not commutative");
  static_assert(!RawFifo::commutative, "RawFifo not commutative");
  assert(Sum::merge<long>(Sum::merge<long>(5, -3), 8) ==
         Sum::merge<long>(Sum::merge<long>(5, 8), -3));
  // KeepLatest genuinely reorder-sensitive.
  assert(KeepLatest::merge<long>(KeepLatest::merge<long>(0, 1), 2) !=
         KeepLatest::merge<long>(KeepLatest::merge<long>(0, 2), 1));
}

TEST(test_merge_idempotency_flags) {
  static_assert(Max::idempotent && SetUnion::idempotent, "Max/SetUnion idempotent");
  static_assert(!Sum::idempotent && !RawFifo::idempotent, "Sum/RawFifo not idempotent");
  static_assert(!RawFifo::conflates, "RawFifo cannot conflate");
  assert(Max::merge<long>(Max::merge<long>(3, 9), 9) == Max::merge<long>(3, 9));
  assert(Sum::merge<long>(Sum::merge<long>(0, 5), 5) != Sum::merge<long>(0, 5));
}

TEST(test_set_union_and_raw_fifo) {
  std::set<int> s1{1, 2}, s2{2, 3};
  auto u = SetUnion::merge<std::set<int>>(s1, s2);
  assert((u == std::set<int>{1, 2, 3}));
  // idempotent: (a ∪ b) ∪ b == a ∪ b
  assert(SetUnion::merge<std::set<int>>(u, s2) == u);
}

TEST(test_cell_is_merge_cell_keep_latest) {
  Context ctx;
  auto cell = ctx.source<long>(0);
  MergeCell<long, KeepLatest> mc(ctx, 0);
  for (long v : {3L, 3L, 7L, 7L, 1L}) {
    ctx.set(cell, v);
    mc.merge(v);
    assert(ctx.get(cell) == mc.get());
  }
  assert(mc.get() == 1);
}

TEST(test_sum_converges_regardless_of_order) {
  Context ctx;
  MergeCell<long, Sum> a(ctx, 0);
  for (long d : {5L, -3L, 8L, 2L, -1L}) a.merge(d);
  MergeCell<long, Sum> b(ctx, 0);
  for (long d : {-1L, 2L, 8L, -3L, 5L}) b.merge(d);
  assert(a.get() == b.get());
  assert(a.get() == 11);
}

TEST(test_idempotent_merge_no_ops_via_guard) {
  Context ctx;
  MergeCell<long, Max> mc(ctx, 10);
  int runs = 0;
  ctx.effect([&](Compute& c) -> CleanupFn {
    (void)mc.get(c);
    ++runs;
    return {};
  });
  assert(runs == 1);
  mc.merge(5);
  mc.merge(10);
  mc.merge(0);
  assert(runs == 1);  // merges at/below max fire no cascade
  mc.merge(42);
  assert(mc.get() == 42);
  assert(runs == 2);
}

TEST(test_converged_determinism_mirrors_fixture) {
  // Mirror of mergecell_algebra.json (Sum initial 0: 5,-3,8,2,0 → 12;
  // Max initial 10: 5,10,42,0,42 → 42).
  Context ctx;
  MergeCell<long, Sum> s(ctx, 0);
  for (long op : {5L, -3L, 8L, 2L, 0L}) s.merge(op);
  assert(s.get() == 12);
  MergeCell<long, Max> m(ctx, 10);
  for (long op : {5L, 10L, 42L, 0L, 42L}) m.merge(op);
  assert(m.get() == 42);
}

int main() {
  std::cout << "test_merge: " << test_passed << "/" << test_count << " passed\n";
  return test_passed == test_count ? 0 : 1;
}
