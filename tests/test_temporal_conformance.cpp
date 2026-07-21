// Temporal source conformance tests (`#lztime`).
//
// A C++17 port of `lazily-rs/tests/temporal_conformance.rs`. These are **compute**
// fixtures (`lazily-spec/conformance/temporal/*.json`, read from the sibling
// lazily-spec checkout): load the `initial` state, replay each
// `tick(now)` op, and assert the fire edge (`returns`), the projected reader
// values, and — the core of the spec — that the primary reader invalidates
// exactly on the fire edge. Invalidation is observed by wrapping the reader cell
// in a `computed` and checking whether its cached value survives the tick
// (`ctx.is_set`).
//
// Each scenario is transcribed directly from its fixture JSON (initial + steps),
// mirroring the repo's other conformance tests; the canonical fixture text is also
// read from the sibling checkout and asserted to carry its `"model"` marker so the test
// stays coupled to the fixture.

#include <lazily/temporal.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include "test_spec_fixture.hpp"

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

static std::string fixture_text(const std::string& file) {
  return lazily_test::spec_fixture_text("temporal", file);
}

// timer_single_shot.json — initial { fire_at: 3 }.
// Steps: now=1 -> edge false, fired false, value null, next_fire 3, inval false
//        now=3 -> edge true,  fired true,  value (), next_fire null, inval true
//        now=5 -> edge false, fired true,  value (), next_fire null, inval false
TEST(test_timer_single_shot) {
  assert(fixture_text("timer_single_shot.json").find("\"model\": \"TimerCell\"") !=
         std::string::npos);

  Context ctx;
  TimerCell timer(ctx, 3);
  auto fired = timer.fired_cell();
  auto observed = ctx.memo<bool>([&](Context& c) { return fired.get(c); });
  (void)ctx.get(observed);  // prime the cache

  struct Step {
    uint64_t now;
    bool edge;
    bool fired;
    bool has_value;
    std::optional<uint64_t> next_fire;
    bool invalidates;
  };
  const Step steps[] = {
      {1, false, false, false, std::optional<uint64_t>(3), false},
      {3, true, true, true, std::nullopt, true},
      {5, false, true, true, std::nullopt, false},
  };

  for (const auto& s : steps) {
    bool edge = timer.tick(ctx, s.now);
    assert(edge == s.edge);
    assert(timer.has_fired(ctx) == s.fired);
    assert(timer.value(ctx).has_value() == s.has_value);
    assert(timer.next_fire() == s.next_fire);

    bool was_cached = ctx.is_set(observed);
    (void)ctx.get(observed);
    assert((!was_cached) == s.invalidates);
  }
}

// interval_periodic.json — initial { period: 2 }.
TEST(test_interval_periodic) {
  assert(fixture_text("interval_periodic.json").find("\"model\": \"IntervalCell\"") !=
         std::string::npos);

  Context ctx;
  IntervalCell iv(ctx, 2);
  auto count = iv.count_cell();
  auto observed = ctx.memo<uint64_t>([&](Context& c) { return count.get(c); });
  (void)ctx.get(observed);

  struct Step {
    uint64_t now;
    bool edge;
    uint64_t count;
    std::optional<uint64_t> next_fire;
    bool invalidates;
  };
  const Step steps[] = {
      {1, false, 0, std::optional<uint64_t>(2), false},
      {2, true, 1, std::optional<uint64_t>(4), true},
      {4, true, 2, std::optional<uint64_t>(6), true},
      {5, false, 2, std::optional<uint64_t>(6), false},
      {8, true, 4, std::optional<uint64_t>(10), true},
  };

  for (const auto& s : steps) {
    bool edge = iv.tick(ctx, s.now);
    assert(edge == s.edge);
    assert(iv.count(ctx) == s.count);
    assert(iv.next_fire() == s.next_fire);

    bool was_cached = ctx.is_set(observed);
    (void)ctx.get(observed);
    assert((!was_cached) == s.invalidates);
  }
}

// cron_pattern.json — initial { cycle: 5, offsets: [0, 3] }.
TEST(test_cron_pattern) {
  assert(fixture_text("cron_pattern.json").find("\"model\": \"CronCell\"") !=
         std::string::npos);

  Context ctx;
  CronCell cron(ctx, 5, {0, 3});
  auto count = cron.count_cell();
  auto observed = ctx.memo<uint64_t>([&](Context& c) { return count.get(c); });
  (void)ctx.get(observed);

  struct Step {
    uint64_t now;
    bool edge;
    uint64_t count;
    std::optional<uint64_t> next_fire;
    bool invalidates;
  };
  const Step steps[] = {
      {2, false, 0, std::optional<uint64_t>(3), false},
      {3, true, 1, std::optional<uint64_t>(5), true},
      {5, true, 2, std::optional<uint64_t>(8), true},
      {8, true, 3, std::optional<uint64_t>(10), true},
      {10, true, 4, std::optional<uint64_t>(13), true},
  };

  for (const auto& s : steps) {
    bool edge = cron.tick(ctx, s.now);
    assert(edge == s.edge);
    assert(cron.count(ctx) == s.count);
    assert(cron.next_fire() == s.next_fire);

    bool was_cached = ctx.is_set(observed);
    (void)ctx.get(observed);
    assert((!was_cached) == s.invalidates);
  }
}

// deadline_expiry.json — initial { value: "payload", deadline: 4 }.
TEST(test_deadline_expiry) {
  assert(fixture_text("deadline_expiry.json").find("\"model\": \"DeadlineCell\"") !=
         std::string::npos);

  Context ctx;
  const std::string value = "payload";
  DeadlineCell<std::string> d(ctx, value, 4);
  auto expired = d.expired_cell();
  auto observed = ctx.memo<bool>([&](Context& c) { return expired.get(c); });
  (void)ctx.get(observed);

  struct Step {
    uint64_t now;
    bool edge;
    bool expired;
    bool invalidates;
  };
  const Step steps[] = {
      {2, false, false, false},
      {4, true, true, true},
      {9, false, true, false},
  };

  for (const auto& s : steps) {
    bool edge = d.tick(ctx, s.now);
    assert(edge == s.edge);
    Deadlined<std::string> state = d.state(ctx);
    assert(state.is_expired() == s.expired);
    assert(state.value() == value);  // value preserved across the flip
    assert(d.is_expired(ctx) == s.expired);

    bool was_cached = ctx.is_set(observed);
    (void)ctx.get(observed);
    assert((!was_cached) == s.invalidates);
  }
}

// Pure-core sanity (mirrors the Rust unit tests): ManualClock monotonicity and
// idempotent single-shot fire.
TEST(test_manual_clock_and_core_idempotence) {
  ManualClock clock;
  assert(clock.now() == 0);
  assert(clock.advance(5) == 5);
  assert(clock.advance(3) == 5);  // clamped, monotone

  TimerCore t(3);
  assert(!t.tick(1));
  assert(t.next_fire() == std::optional<uint64_t>(3));
  assert(t.tick(3));
  assert(!t.tick(5));  // idempotent
  assert(t.next_fire() == std::nullopt);
  assert(t.fired());
}

int main() {
  REQUIRE_FIXTURES_LOADED(4); return test_count == test_passed ? 0 : 1; }
