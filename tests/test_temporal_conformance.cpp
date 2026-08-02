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
// Every scenario parses its initial state, operations, and expectations from the
// canonical fixture so corpus changes cannot pass against a stale transcription.

#include <lazily/temporal.hpp>

#include "test_assertion_keys.hpp"
#include "test_json.hpp"
#include "test_spec_fixture.hpp"
#include <cassert>
#include <optional>
#include <string>
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

static lazily_test::JsonPtr fixture(const std::string& file) {
  return lazily_test::parse_json(lazily_test::spec_fixture_text("temporal", file));
}

// timer_single_shot.json — initial { fire_at: 3 }.
// Steps: now=1 -> edge false, fired false, value null, next_fire 3, inval false
//        now=3 -> edge true,  fired true,  value (), next_fire null, inval true
//        now=5 -> edge false, fired true,  value (), next_fire null, inval false
TEST(test_timer_single_shot) {
  const auto fx = fixture("timer_single_shot.json");
  const auto& initial = lazily_test::json_member(*fx, "initial");
  Context ctx;
  TimerCell timer(ctx, lazily_test::json_u64(lazily_test::json_member(initial, "fire_at")));
  auto fired = timer.fired_cell();
  auto observed = ctx.computed<bool>([&](Compute& c) { return fired.get(c); });
  (void)ctx.get(observed); // prime the cache

  for (const auto& step_ptr : lazily_test::json_array(lazily_test::json_member(*fx, "steps"))) {
    const auto& item = *step_ptr;
    const auto& op = lazily_test::json_member(item, "op");
    lazily_test::AssertionKeys expected(std::string(__func__) + " expected",
                                        lazily_test::json_member(item, "expected"));
    // Fail closed (#lzscenariobodyskip): this runner replays every step as a
    // tick, so a step of any other type must fail rather than be replayed as
    // one.
    const auto type = lazily_test::json_string(lazily_test::json_member(op, "type"));
    REQUIRE(type == "tick", "unknown temporal op in fixture: " + type);
    const auto now = lazily_test::json_u64(lazily_test::json_member(op, "now"));
    assert(timer.tick(ctx, now) ==
           lazily_test::json_bool(lazily_test::json_member(item, "returns")));
    expected.assert_key("fired", timer.has_fired(ctx));
    // The fixture spells the unit payload as `null` before the fire edge and
    // `{}` after it, so presence is the whole claim it carries.
    expected.assert_key_with("value", [&](const lazily_test::Json& want) {
      return timer.value(ctx).has_value() == !want.is_null();
    });
    expected.assert_key("next_fire", timer.next_fire(), lazily_test::json_optional_u64);

    const bool was_cached = ctx.is_set(observed);
    (void)ctx.get(observed);
    expected.assert_key_with("invalidates", [&](const lazily_test::Json& want) {
      return lazily_test::json_bool(lazily_test::json_member(want, "fired")) == !was_cached;
    });
  }
}

// interval_periodic.json — initial { period: 2 }.
TEST(test_interval_periodic) {
  const auto fx = fixture("interval_periodic.json");
  const auto& initial = lazily_test::json_member(*fx, "initial");
  Context ctx;
  IntervalCell iv(ctx, lazily_test::json_u64(lazily_test::json_member(initial, "period")));
  auto count = iv.count_cell();
  auto observed = ctx.computed<uint64_t>([&](Compute& c) { return count.get(c); });
  (void)ctx.get(observed);

  for (const auto& step_ptr : lazily_test::json_array(lazily_test::json_member(*fx, "steps"))) {
    const auto& item = *step_ptr;
    const auto& op = lazily_test::json_member(item, "op");
    lazily_test::AssertionKeys expected(std::string(__func__) + " expected",
                                        lazily_test::json_member(item, "expected"));
    const auto now = lazily_test::json_u64(lazily_test::json_member(op, "now"));
    assert(iv.tick(ctx, now) == lazily_test::json_bool(lazily_test::json_member(item, "returns")));
    expected.assert_key("count", iv.count(ctx));
    expected.assert_key("next_fire", iv.next_fire(), lazily_test::json_optional_u64);

    const bool was_cached = ctx.is_set(observed);
    (void)ctx.get(observed);
    expected.assert_key_with("invalidates", [&](const lazily_test::Json& want) {
      return lazily_test::json_bool(lazily_test::json_member(want, "count")) == !was_cached;
    });
  }
}

// cron_pattern.json — initial { cycle: 5, offsets: [0, 3] }.
TEST(test_cron_pattern) {
  const auto fx = fixture("cron_pattern.json");
  const auto& initial = lazily_test::json_member(*fx, "initial");
  std::vector<uint64_t> offsets;
  for (const auto& value : lazily_test::json_array(lazily_test::json_member(initial, "offsets"))) {
    offsets.push_back(lazily_test::json_u64(*value));
  }
  Context ctx;
  CronCell cron(ctx, lazily_test::json_u64(lazily_test::json_member(initial, "cycle")), offsets);
  auto count = cron.count_cell();
  auto observed = ctx.computed<uint64_t>([&](Compute& c) { return count.get(c); });
  (void)ctx.get(observed);

  for (const auto& step_ptr : lazily_test::json_array(lazily_test::json_member(*fx, "steps"))) {
    const auto& item = *step_ptr;
    const auto& op = lazily_test::json_member(item, "op");
    lazily_test::AssertionKeys expected(std::string(__func__) + " expected",
                                        lazily_test::json_member(item, "expected"));
    const auto now = lazily_test::json_u64(lazily_test::json_member(op, "now"));
    assert(cron.tick(ctx, now) ==
           lazily_test::json_bool(lazily_test::json_member(item, "returns")));
    expected.assert_key("count", cron.count(ctx));
    expected.assert_key("next_fire", cron.next_fire(), lazily_test::json_optional_u64);

    const bool was_cached = ctx.is_set(observed);
    (void)ctx.get(observed);
    expected.assert_key_with("invalidates", [&](const lazily_test::Json& want) {
      return lazily_test::json_bool(lazily_test::json_member(want, "count")) == !was_cached;
    });
  }
}

// deadline_expiry.json — initial { value: "payload", deadline: 4 }.
TEST(test_deadline_expiry) {
  const auto fx = fixture("deadline_expiry.json");
  const auto& initial = lazily_test::json_member(*fx, "initial");
  Context ctx;
  const std::string value = lazily_test::json_string(lazily_test::json_member(initial, "value"));
  DeadlineCell<std::string> d(ctx, value,
                              lazily_test::json_u64(lazily_test::json_member(initial, "deadline")));
  auto expired = d.expired_cell();
  auto observed = ctx.computed<bool>([&](Compute& c) { return expired.get(c); });
  (void)ctx.get(observed);

  for (const auto& step_ptr : lazily_test::json_array(lazily_test::json_member(*fx, "steps"))) {
    const auto& item = *step_ptr;
    const auto& op = lazily_test::json_member(item, "op");
    lazily_test::AssertionKeys expected(std::string(__func__) + " expected",
                                        lazily_test::json_member(item, "expected"));
    const auto now = lazily_test::json_u64(lazily_test::json_member(op, "now"));
    assert(d.tick(ctx, now) == lazily_test::json_bool(lazily_test::json_member(item, "returns")));
    Deadlined<std::string> state = d.state(ctx);
    // The fixture names the state (`Live` / `Expired`); both the projected
    // wrapper and the reader cell must agree with it.
    expected.assert_key_with("state", [&](const lazily_test::Json& want) {
      const bool want_expired = lazily_test::json_string(want) == "Expired";
      return state.is_expired() == want_expired && d.is_expired(ctx) == want_expired;
    });
    expected.assert_key("value", state.value(), lazily_test::json_string);

    const bool was_cached = ctx.is_set(observed);
    (void)ctx.get(observed);
    expected.assert_key_with("invalidates", [&](const lazily_test::Json& want) {
      return lazily_test::json_bool(lazily_test::json_member(want, "state")) == !was_cached;
    });
  }
}

// Pure-core sanity (mirrors the Rust unit tests): ManualClock monotonicity and
// idempotent single-shot fire.
TEST(test_manual_clock_and_core_idempotence) {
  ManualClock clock;
  assert(clock.now() == 0);
  assert(clock.advance(5) == 5);
  assert(clock.advance(3) == 5); // clamped, monotone

  TimerCore t(3);
  assert(!t.tick(1));
  assert(t.next_fire() == std::optional<uint64_t>(3));
  assert(t.tick(3));
  assert(!t.tick(5)); // idempotent
  assert(t.next_fire() == std::nullopt);
  assert(t.fired());
}

int main() {
  REQUIRE_FIXTURES_LOADED(4);
  return test_count == test_passed ? 0 : 1;
}
