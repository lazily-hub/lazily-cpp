// The Cell kernel (`#lzcellkernel`) — `SourceCell` / `FormulaCell` over the
// genus `Cell<T, K>`.
//
// Covers the constructor surface (`source`/`source_with`/`formula`), reads,
// source-only writes (`set`/`merge`), the driven-formula eager construction
// (`formula().drive()` — idempotent, side-table teardown), and the COMPILE-TIME
// write protection: `formula.set(...)` must not compile. That last property is
// proved two ways — a `has_set<>` detector `static_assert` here (runs in this
// TU), and `tests/compile_fail_formula_set.cpp`, a WILL_FAIL build.

#include <lazily/lazily.hpp>

#include <cstdlib>
#include <iostream>
#include <set>
#include <type_traits>

#include "test_require.hpp"

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

// ── Compile-time write protection (§3/§4) ──────────────────────────────────
//
// Detection idiom: `has_set<C>` is true iff `c.set(ctx, v)` is well-formed. A
// `SourceCell` has `set`; a `FormulaCell` does not, so `formula.set(...)` is a
// compile error — no shared base, no runtime gate.

template <typename C, typename = void>
struct has_set : std::false_type {};
template <typename C>
struct has_set<C, std::void_t<decltype(std::declval<C&>().set(
                      std::declval<Context&>(), 0LL))>> : std::true_type {};

template <typename C, typename = void>
struct has_merge : std::false_type {};
template <typename C>
struct has_merge<C, std::void_t<decltype(std::declval<C&>().merge(
                        std::declval<Context&>(), 0LL))>> : std::true_type {};

static_assert(has_set<SourceCell<long long>>::value,
              "a SourceCell must be settable");
static_assert(has_merge<SourceCell<long long>>::value,
              "a SourceCell must be mergeable");
static_assert(has_set<SourceCell<long long, Sum>>::value,
              "a folding SourceCell must be settable");
static_assert(!has_set<FormulaCell<long long>>::value,
              "a FormulaCell must NOT be settable — formula.set(...) must be a "
              "compile error");
static_assert(!has_merge<FormulaCell<long long>>::value,
              "a FormulaCell must NOT be mergeable");

// The two aliases are the two kinds of the one genus.
static_assert(std::is_same<SourceCell<long long>,
                           Cell<long long, Source<KeepLatest>>>::value,
              "SourceCell<T> == Cell<T, Source<KeepLatest>> — Cell ≡ "
              "SourceCell<KeepLatest>");
static_assert(std::is_same<FormulaCell<long long>,
                           Cell<long long, Formula>>::value,
              "FormulaCell<T> == Cell<T, Formula>");

// ── Runtime behaviour ──────────────────────────────────────────────────────

TEST(test_source_read_write) {
  Context ctx;
  SourceCell<long long> n = ctx.source<long long>(1);
  REQUIRE(n.get(ctx) == 1, "source reads its initial value");
  n.set(ctx, 2);
  REQUIRE(n.get(ctx) == 2, "source reads the value it was set to");
}

TEST(test_formula_reads_upstream_guarded) {
  Context ctx;
  SourceCell<long long> n = ctx.source<long long>(2);
  int computes = 0;
  FormulaCell<long long> doubled =
      ctx.formula<long long>([n, &computes](Context& c) {
        ++computes;
        return n.get(c) * 2;
      });
  REQUIRE(doubled.get(ctx) == 4, "formula computes from upstream");
  REQUIRE(computes == 1, "one compute so far");
  doubled.get(ctx);
  REQUIRE(computes == 1, "cached — no recompute on an unchanged read");

  // Guarded by default (§9.3): setting the source to a value that leaves the
  // formula's result unchanged must NOT propagate a recompute past the guard.
  n.set(ctx, 2);  // same value: no source change, no invalidation
  REQUIRE(doubled.get(ctx) == 4, "value unchanged");
  REQUIRE(computes == 1, "no recompute for a no-op write");

  n.set(ctx, 3);
  REQUIRE(doubled.get(ctx) == 6, "recomputes when upstream actually changes");
  REQUIRE(computes == 2, "exactly one recompute");
}

TEST(test_source_with_policy_merge) {
  Context ctx;
  // A folding source cell: Sum accumulates writes.
  SourceCell<long long, Sum> acc = ctx.source_with<Sum, long long>(10);
  REQUIRE(acc.get(ctx) == 10, "folding source reads its initial value");
  acc.merge(ctx, 5);
  REQUIRE(acc.get(ctx) == 15, "merge folds under Sum");
  acc.merge(ctx, 100);
  REQUIRE(acc.get(ctx) == 115, "merge folds again");
  acc.set(ctx, 0);
  REQUIRE(acc.get(ctx) == 0, "set replaces outright, bypassing the policy");
}

TEST(test_drive_is_eager_and_idempotent) {
  Context ctx;
  SourceCell<long long> n = ctx.source<long long>(1);
  int computes = 0;
  FormulaCell<long long> f = ctx.formula<long long>([n, &computes](Context& c) {
    ++computes;
    return n.get(c) + 100;
  });

  REQUIRE(!f.is_driven(ctx), "a fresh formula is lazy");
  FormulaCell<long long> g = f.drive(ctx);
  REQUIRE(f.is_driven(ctx), "drive makes it eager");
  REQUIRE(g == f, "drive returns the SAME handle, mutated (not a driver)");
  const int after_drive = computes;

  // Idempotent — a second drive attaches no second puller.
  f.drive(ctx);
  REQUIRE(f.is_driven(ctx), "still driven");

  // Eager: a source change re-materializes the formula WITHOUT a read.
  n.set(ctx, 2);
  REQUIRE(computes > after_drive,
          "a driven formula recomputes on invalidation without being read");
  REQUIRE(f.get(ctx) == 102, "and holds the fresh value");

  // Undrive reverts to lazy; the value stays readable.
  f.undrive(ctx);
  REQUIRE(!f.is_driven(ctx), "undrive de-eagers");
  REQUIRE(f.get(ctx) == 102, "value survives undrive");
}

TEST(test_drive_coalesces_in_a_batch) {
  // Clause 3 of signal eagerness, now via the kernel: N invalidations inside one
  // batch coalesce into ONE recompute because the puller is a scheduled effect.
  Context ctx;
  SourceCell<long long> n = ctx.source<long long>(0);
  int computes = 0;
  FormulaCell<long long> f = ctx.formula<long long>([n, &computes](Context& c) {
    ++computes;
    return n.get(c);
  });
  f.drive(ctx);
  const int before = computes;
  ctx.batch([&](Context& c) {
    n.set(c, 1);
    n.set(c, 2);
    n.set(c, 3);
  });
  REQUIRE(computes == before + 1,
          "three writes in one batch coalesce into a single eager recompute");
  REQUIRE(f.get(ctx) == 3, "and the value is the last write");
}

TEST(test_dispose_driven_formula_tears_down_puller) {
  // Disposing a driven formula must tear down its puller (§9.3.4) — no stranded
  // effect, and the owner-keyed side table entry is cleared so a recycled id
  // never inherits the driver.
  Context ctx;
  SourceCell<long long> n = ctx.source<long long>(1);
  FormulaCell<long long> f =
      ctx.formula<long long>([n](Context& c) { return n.get(c); });
  f.drive(ctx);
  REQUIRE(f.is_driven(ctx), "driven before dispose");
  f.dispose(ctx);
  // Reading a disposed formula throws; the puller no longer runs. A surviving
  // write must not resurrect anything.
  n.set(ctx, 2);
  bool threw = false;
  try {
    f.get(ctx);
  } catch (const DisposedError&) {
    threw = true;
  }
  REQUIRE(threw, "a disposed driven formula is gone, puller and all");
}

int main() {
  std::cout << "lazily-cpp cell-kernel tests: " << test_passed << "/"
            << test_count << " passed" << std::endl;
  return test_passed == test_count ? 0 : 1;
}
