// `Context::computed_ripple_when` (`#lzcellkernel`) — a guarded computed with an
// explicit, PURE change predicate (`true` = propagate the ripple). Mirrors
// lazily-rs/tests/computed_ripple_when.rs. Covers the two motivating shapes — a
// custom significance policy (bucket proxy) and "propagate every N" where the
// increment evidence lives in the value (so the predicate stays pure) — plus the
// two identities: `computed(f) ≡ computed_ripple_when(f, !=)` and `slot(f)` is
// the always-propagate pass-through.

#include <lazily/lazily.hpp>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <utility>

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

// A custom significance policy: the derived value carries a `bucket` proxy;
// propagate only when the bucket changes, ignoring the raw payload.
TEST(ripple_when_custom_significance_propagates_on_proxy_change) {
  Context ctx;
  auto input = ctx.source<std::uint64_t>(0);

  using Pair = std::pair<std::uint64_t, std::uint64_t>;  // (payload, bucket)
  auto derived = ctx.computed_ripple_when<Pair>(
      [input](Compute& c) -> Pair {
        auto v = c.get(input);
        return {v, v / 10};
      },
      [](const Pair& old_v, const Pair& new_v) {
        return old_v.second != new_v.second;  // propagate when bucket changed
      });

  auto recomputes = std::make_shared<int>(0);
  auto observer = ctx.computed<std::uint64_t>([derived, recomputes](Compute& c) {
    ++*recomputes;
    return c.get(derived).first;
  });

  REQUIRE(ctx.get(observer) == 0, "initial value");
  int base = *recomputes;

  // Same bucket (0..9): dependent stays cached.
  ctx.set(input, std::uint64_t{3});
  REQUIRE(ctx.get(observer) == 0, "suppressed: proxy bucket unchanged");
  REQUIRE(*recomputes == base, "no dependent recompute within a bucket");

  // Crossing a bucket boundary propagates.
  ctx.set(input, std::uint64_t{12});
  REQUIRE(ctx.get(observer) == 12, "propagated: bucket changed");
  REQUIRE(*recomputes == base + 1, "one dependent recompute across the boundary");
}

// "Propagate every 3rd increment" — the evidence (the counter) is IN the value,
// so the predicate is a pure function of (old, new).
TEST(ripple_when_propagate_every_n_via_value_carried_counter) {
  Context ctx;
  auto input = ctx.source<std::uint64_t>(0);

  auto sampled = ctx.computed_ripple_when<std::uint64_t>(
      [input](Compute& c) { return c.get(input); },
      [](const std::uint64_t& old_v, const std::uint64_t& new_v) {
        return new_v / 3 != old_v / 3;  // cross a size-3 window boundary
      });

  auto seen = std::make_shared<int>(0);
  auto observer = ctx.computed<std::uint64_t>([sampled, seen](Compute& c) {
    ++*seen;
    return c.get(sampled);
  });

  REQUIRE(ctx.get(observer) == 0, "initial value");
  int base = *seen;

  // 0 -> 1 -> 2 stay in window [0,3): suppressed.
  ctx.set(input, std::uint64_t{1});
  ctx.set(input, std::uint64_t{2});
  REQUIRE(ctx.get(observer) == 0, "window not crossed yet");
  REQUIRE(*seen == base, "no recompute inside the window");

  // 3 crosses into [3,6): propagate.
  ctx.set(input, std::uint64_t{3});
  REQUIRE(ctx.get(observer) == 3, "propagated across the window boundary");
  REQUIRE(*seen == base + 1, "exactly one recompute on the crossing");
}

// `computed(f)` behaves exactly as `computed_ripple_when(f, !=)`.
TEST(computed_is_ripple_when_not_equal) {
  Context ctx;
  auto input = ctx.source<long long>(0);

  auto via_computed =
      ctx.computed<long long>([input](Compute& c) { return std::min(c.get(input), 1LL); });
  auto via_when = ctx.computed_ripple_when<long long>(
      [input](Compute& c) { return std::min(c.get(input), 1LL); },
      [](const long long& o, const long long& n) { return o != n; });

  auto ca = std::make_shared<int>(0);
  auto cb = std::make_shared<int>(0);
  auto obs_a = ctx.computed<long long>([via_computed, ca](Compute& c) {
    ++*ca;
    return c.get(via_computed);
  });
  auto obs_b = ctx.computed<long long>([via_when, cb](Compute& c) {
    ++*cb;
    return c.get(via_when);
  });
  REQUIRE(ctx.get(obs_a) == 0, "obs_a initial");
  REQUIRE(ctx.get(obs_b) == 0, "obs_b initial");
  int base_a = *ca, base_b = *cb;

  // 0 -> 5 both clamp to 1: both guards suppress identically (propagate once).
  ctx.set(input, 5LL);
  REQUIRE(ctx.get(obs_a) == 1, "computed clamps to 1");
  REQUIRE(ctx.get(obs_b) == 1, "ripple_when(!=) clamps to 1");
  REQUIRE(*ca == base_a + 1, "computed propagated the 0->1 change");
  REQUIRE(*cb == base_b + 1, "ripple_when(!=) matches computed");

  // 5 -> 9 both stay 1: both suppress the dependent.
  ctx.set(input, 9LL);
  REQUIRE(ctx.get(obs_a) == 1, "still 1");
  REQUIRE(ctx.get(obs_b) == 1, "still 1");
  REQUIRE(*ca == base_a + 1, "computed suppressed the equal recompute");
  REQUIRE(*cb == base_b + 1, "ripple_when(!=) matches computed under suppression");
}

// `slot(f)` installs no guard: even an equal recompute propagates.
TEST(slot_is_pass_through_always_propagates) {
  Context ctx;
  auto input = ctx.source<std::uint64_t>(0);
  auto passthrough = ctx.slot<std::uint64_t>([input](Compute& c) {
    (void)c.get(input);  // depend on input, but always yield the same value
    return std::uint64_t{0};
  });

  auto recomputes = std::make_shared<int>(0);
  auto observer = ctx.computed<std::uint64_t>([passthrough, recomputes](Compute& c) {
    ++*recomputes;
    return c.get(passthrough);
  });

  REQUIRE(ctx.get(observer) == 0, "initial value");
  int base = *recomputes;

  // Value stays 0, but the pass-through slot has no guard, so the dependent re-fires.
  ctx.set(input, std::uint64_t{5});
  REQUIRE(ctx.get(observer) == 0, "value unchanged");
  REQUIRE(*recomputes > base,
          "pass-through slot propagates even when the value is unchanged");
}

int main() {
  std::cout << "lazily-cpp computed_ripple_when tests: " << test_passed << "/"
            << test_count << " passed" << std::endl;
  return test_passed == test_count ? 0 : 1;
}
