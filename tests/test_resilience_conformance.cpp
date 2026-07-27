// Fault-tolerance conformance (`#lzresilience`).
//
// Replays the shared cross-language fixtures in
// `lazily-spec/conformance/resilience/{circuit_breaker,retry,bulkhead,timeout}.json`
// (read from the sibling lazily-spec checkout). Mirrors the Rust reference
// `lazily-rs/tests/resilience_conformance.rs`: per step assert the op result +
// projected reader value and the reader's INVALIDATION via a `computed` +
// `is_set` cache-survival probe.

#include <lazily/resilience.hpp>

#include <cassert>
#include <cstdint>
#include <string>
#include "test_json.hpp"
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

static lazily_test::JsonPtr fixture(const char* file) {
  return lazily_test::parse_json(
      lazily_test::spec_fixture_text("resilience", file));
}

// -- circuit_breaker.json --

TEST(test_circuit_breaker) {
  const auto fx = fixture("circuit_breaker.json");
  const auto& config = lazily_test::json_member(*fx, "config");
  Context ctx;
  CircuitBreakerCell cb(
      ctx, lazily_test::json_u64(lazily_test::json_member(config, "window")),
      lazily_test::json_u64(
          lazily_test::json_member(config, "failure_threshold")),
      lazily_test::json_u64(
          lazily_test::json_member(config, "reset_timeout")));
  auto sc = cb.state_cell();
  auto observed = ctx.computed<BreakerState>([sc](Compute& c) { return sc.get(c); });
  (void)ctx.get(observed);

  for (const auto& step_ptr :
       lazily_test::json_array(lazily_test::json_member(*fx, "steps"))) {
    const auto& item = *step_ptr;
    const auto& op = lazily_test::json_member(item, "op");
    const auto& expected = lazily_test::json_member(item, "expected");
    const auto type = lazily_test::json_string(lazily_test::json_member(op, "type"));
    const auto now = lazily_test::json_u64(lazily_test::json_member(op, "now"));
    if (type == "record") {
      cb.record(ctx,
                lazily_test::json_bool(lazily_test::json_member(op, "success")),
                now);
    } else {
      assert(type == "allow");
      assert(cb.allow(ctx, now) ==
             lazily_test::json_bool(lazily_test::json_member(item, "returns")));
    }
    const auto state =
        lazily_test::json_string(lazily_test::json_member(expected, "state"));
    const auto want = state == "Closed"
                          ? BreakerState::Closed
                          : (state == "Open" ? BreakerState::Open
                                             : BreakerState::HalfOpen);
    assert(cb.state() == want && "breaker state");
    bool was = ctx.is_set(observed);
    (void)ctx.get(observed);
    assert((!was) == lazily_test::json_bool(lazily_test::json_member(
                         lazily_test::json_member(expected, "invalidates"),
                         "state")) &&
           "state invalidation");
  }
}

// -- retry.json --

TEST(test_retry) {
  const auto fx = fixture("retry.json");
  const auto& config = lazily_test::json_member(*fx, "config");
  Context ctx;
  RetryPolicyCell r(
      ctx, lazily_test::json_u64(lazily_test::json_member(config, "base")),
      lazily_test::json_u64(lazily_test::json_member(config, "cap")));
  auto dc = r.delay_cell();
  auto observed = ctx.computed<uint64_t>([dc](Compute& c) { return dc.get(c); });
  (void)ctx.get(observed);

  for (const auto& step_ptr :
       lazily_test::json_array(lazily_test::json_member(*fx, "steps"))) {
    const auto& item = *step_ptr;
    const auto& expected = lazily_test::json_member(item, "expected");
    assert(r.next_delay(ctx) ==
           lazily_test::json_u64(lazily_test::json_member(item, "returns")));
    assert(r.delay(ctx) ==
           lazily_test::json_u64(lazily_test::json_member(expected, "delay")));
    bool was = ctx.is_set(observed);
    (void)ctx.get(observed);
    assert((!was) == lazily_test::json_bool(lazily_test::json_member(
                         lazily_test::json_member(expected, "invalidates"),
                         "delay")) &&
           "delay invalidation");
  }
}

// -- bulkhead.json --

TEST(test_bulkhead) {
  const auto fx = fixture("bulkhead.json");
  const auto& config = lazily_test::json_member(*fx, "config");
  Context ctx;
  BulkheadCell b(
      ctx, lazily_test::json_u64(lazily_test::json_member(config, "capacity")));
  auto uc = b.permits_in_use_cell();
  auto observed = ctx.computed<uint64_t>([uc](Compute& c) { return uc.get(c); });
  (void)ctx.get(observed);

  for (const auto& step_ptr :
       lazily_test::json_array(lazily_test::json_member(*fx, "steps"))) {
    const auto& item = *step_ptr;
    const auto& op = lazily_test::json_member(item, "op");
    const auto& expected = lazily_test::json_member(item, "expected");
    const auto type = lazily_test::json_string(lazily_test::json_member(op, "type"));
    if (type == "acquire") {
      assert(b.acquire(ctx) ==
             lazily_test::json_bool(lazily_test::json_member(item, "returns")));
    } else {
      assert(type == "release");
      assert(lazily_test::json_member(item, "returns").is_null());
      b.release(ctx);
    }
    assert(b.permits_in_use(ctx) ==
           lazily_test::json_u64(lazily_test::json_member(expected, "in_use")));
    bool was = ctx.is_set(observed);
    (void)ctx.get(observed);
    assert((!was) == lazily_test::json_bool(lazily_test::json_member(
                         lazily_test::json_member(expected, "invalidates"),
                         "in_use")) &&
           "in_use invalidation");
  }
}

// -- timeout.json --

TEST(test_timeout) {
  const auto fx = fixture("timeout.json");
  Context ctx;
  TimeoutCell t(ctx);
  auto tc = t.is_timed_out_cell();
  auto observed = ctx.computed<bool>([tc](Compute& c) { return tc.get(c); });
  (void)ctx.get(observed);

  for (const auto& step_ptr :
       lazily_test::json_array(lazily_test::json_member(*fx, "steps"))) {
    const auto& item = *step_ptr;
    const auto& op = lazily_test::json_member(item, "op");
    const auto& expected = lazily_test::json_member(item, "expected");
    const auto type = lazily_test::json_string(lazily_test::json_member(op, "type"));
    const auto now = lazily_test::json_u64(lazily_test::json_member(op, "now"));
    bool got;
    if (type == "arm") {
      t.arm(ctx, now,
            lazily_test::json_u64(lazily_test::json_member(op, "timeout")));
      got = false;
    } else {
      assert(type == "tick");
      got = t.tick(ctx, now);
    }
    assert(got ==
           lazily_test::json_bool(lazily_test::json_member(item, "returns")));
    assert(t.is_timed_out(ctx) == lazily_test::json_bool(
                                      lazily_test::json_member(
                                          expected, "is_timed_out")));
    bool was = ctx.is_set(observed);
    (void)ctx.get(observed);
    assert((!was) == lazily_test::json_bool(lazily_test::json_member(
                         lazily_test::json_member(expected, "invalidates"),
                         "is_timed_out")) &&
           "is_timed_out invalidation");
  }
}

int main() {
  REQUIRE_FIXTURES_LOADED(4);
  return test_count == test_passed ? 0 : 1;
}
