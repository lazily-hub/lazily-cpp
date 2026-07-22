// Fortified compute-view tests (`#lzcellkernel`) — the C++ mirror of
// lazily-rs `tests/compute_fortification.rs`.
//
// Dependency tracking is value-threaded through the per-recompute `Compute`
// view, not ambient. This suite pins the observable consequences:
//
//   1. A TRACKED read through `Compute` registers the edge against the
//      recomputing node — the dependent recomputes on change and the edge is
//      visible in both directions (`dependency_count` / `dependent_count`).
//   2. `Compute::untracked()` registers NO edge — the dependent keeps its stale
//      value and never recomputes.
//   3. An effect tracks through its own `Compute` view.
//   4. The untracked barrier holds under nesting: an untracked read inside a
//      value-threaded closure that is itself nested inside an ambient-bridge
//      (`Fn(Context&)`) recompute does NOT leak an edge to the outer frame.
//   5. Fortification by construction — `Compute` is non-copyable, non-movable,
//      and non-heap-allocatable (static_asserts here; a WILL_FAIL compile in
//      `compile_fail_compute_escape.cpp` locks the copy ban at build time).

#include <lazily/lazily.hpp>

#include <cstdlib>
#include <iostream>
#include <type_traits>

#include "test_require.hpp"

using namespace lazily;

static int test_count = 0;
static int test_passed = 0;

#define TEST(name)                 \
  static void name();              \
  struct name##_runner {           \
    name##_runner() {              \
      ++test_count;                \
      name();                      \
      ++test_passed;               \
    }                              \
  } name##_instance;               \
  static void name()

// ── Fortification by construction ───────────────────────────────────────────
//
// The compute view must not be copyable, movable, or heap-allocatable, so it
// cannot be stored and later replayed to register an edge against the wrong
// node. This is the C++ spelling of lazily-rs binding `Compute` by lifetime and
// making it `!Send`.
static_assert(!std::is_copy_constructible_v<Compute>,
              "Compute must not be copy-constructible (non-escapable)");
static_assert(!std::is_copy_assignable_v<Compute>,
              "Compute must not be copy-assignable (non-escapable)");
static_assert(!std::is_move_constructible_v<Compute>,
              "Compute must not be move-constructible (scope-bound)");
static_assert(!std::is_move_assignable_v<Compute>,
              "Compute must not be move-assignable (scope-bound)");

// ── 1. Tracked read registers the edge against the recomputing node ─────────
TEST(tracked_read_registers_edge) {
  Context ctx;
  auto a = ctx.source<int>(1);
  // Value-threaded closure: reads `a` through the compute view.
  auto d = ctx.computed<int>([a](Compute& cx) { return cx.get(a) * 10; });

  REQUIRE(d.get(ctx) == 10, "initial tracked compute");
  // Edge visible in BOTH directions.
  REQUIRE(ctx.dependency_count(d) == 1, "d depends on a (forward edge)");
  REQUIRE(ctx.dependent_count(a) == 1, "a has d as a dependent (reverse edge)");

  a.set(ctx, 2);
  REQUIRE(d.get(ctx) == 20, "d recomputed after its tracked dependency changed");
}

// ── 2. Untracked read registers NO edge ─────────────────────────────────────
TEST(untracked_read_registers_no_edge) {
  Context ctx;
  auto a = ctx.source<int>(1);
  // Reads `a` through the explicit untracked escape — no edge should form.
  auto d = ctx.computed<int>(
      [a](Compute& cx) { return a.get(cx.untracked()) * 10; });

  REQUIRE(d.get(ctx) == 10, "initial untracked compute");
  REQUIRE(ctx.dependency_count(d) == 0, "untracked read created no forward edge");
  REQUIRE(ctx.dependent_count(a) == 0, "untracked read created no reverse edge");

  a.set(ctx, 2);
  // No edge ⇒ no invalidation ⇒ the cached (stale) value survives.
  REQUIRE(d.get(ctx) == 10, "untracked dependent kept its stale value");
}

// ── 3. Effects track through their compute view ─────────────────────────────
TEST(effect_tracks_through_compute_view) {
  Context ctx;
  auto a = ctx.source<int>(1);
  int runs = 0;
  int last = 0;
  auto e = ctx.effect_void([a, &runs, &last](Compute& cx) {
    last = cx.get(a);
    ++runs;
  });

  REQUIRE(runs == 1 && last == 1, "effect ran once with the initial value");
  REQUIRE(ctx.dependency_count(e) == 1, "effect registered one tracked dep");

  a.set(ctx, 5);
  REQUIRE(runs == 2 && last == 5, "effect re-ran on its tracked dep change");
}

// ── 4. The untracked barrier holds under nesting ────────────────────────────
//
// `inner` is value-threaded and reads `leak` UNTRACKED. `outer` is a legacy
// ambient-bridge closure (`Fn(Context&)`) that reads `inner`, so `inner`
// recomputes WHILE `outer`'s ambient frame is live. Without the engine's
// untracked barrier, `inner`'s untracked read would attribute to `outer` and
// wrongly wire `leak -> outer`. The barrier makes the untracked read genuinely
// untracked regardless of the ambient frame beneath it.
TEST(untracked_barrier_holds_under_nesting) {
  Context ctx;
  auto src = ctx.source<int>(1);
  auto leak = ctx.source<int>(100);

  auto inner = ctx.computed<int>(
      [leak](Compute& cx) { return leak.get(cx.untracked()); });

  // Legacy Context& closure — exercised via the ambient bridge.
  auto outer = ctx.computed<int>(
      [inner, src](Compute& c) { return inner.get(c) + src.get(c); });

  REQUIRE(outer.get(ctx) == 101, "outer composed inner + src");
  REQUIRE(ctx.dependent_count(leak) == 0,
          "untracked read inside a nested value-closure leaked no edge to the "
          "ambient outer frame");
  // outer tracked inner and src (both tracked reads on the bridge), not leak.
  REQUIRE(ctx.dependency_count(outer) == 2, "outer tracked exactly inner + src");
}

// ── 5. Untracked writes from inside a value-threaded compute ────────────────
//
// A value-threaded closure reaches mutators through `untracked()` without
// registering a spurious dependency on the cell it writes.
TEST(untracked_write_from_compute) {
  Context ctx;
  auto trigger = ctx.source<int>(0);
  auto sink = ctx.source<int>(0);
  auto d = ctx.computed<int>([trigger, sink](Compute& cx) {
    int t = cx.get(trigger);           // tracked
    cx.untracked().set(sink, t + 1);   // untracked write, no edge to sink
    return t;
  });

  REQUIRE(d.get(ctx) == 0, "initial");
  REQUIRE(sink.get(ctx) == 1, "untracked write took effect");
  REQUIRE(ctx.dependency_count(d) == 1, "only the tracked read formed an edge");

  trigger.set(ctx, 41);
  REQUIRE(d.get(ctx) == 41, "recomputed on tracked change");
  REQUIRE(sink.get(ctx) == 42, "untracked write re-applied on recompute");
}

int main() {
  std::cout << "compute_fortification: " << test_passed << "/" << test_count
            << " passed" << std::endl;
  REQUIRE(test_passed == test_count, "all compute_fortification tests passed");
  return 0;
}
