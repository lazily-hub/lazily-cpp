// Fault-tolerance conformance (`#lzresilience`).
//
// Replays the shared cross-language fixtures in
// `lazily-spec/conformance/resilience/{circuit_breaker,retry,bulkhead,timeout}.json`
// (read from the sibling lazily-spec checkout). Mirrors the Rust reference
// `lazily-rs/tests/resilience_conformance.rs`: per step assert the op result +
// projected reader value and the reader's INVALIDATION via a `computed` +
// `is_set` cache-survival probe.

#include <lazily/resilience.hpp>

#include "test_assertion_keys.hpp"
#include "test_json.hpp"
#include "test_spec_fixture.hpp"
#include <cassert>
#include <cstdint>
#include <string>

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

static lazily_test::JsonPtr fixture(const char* file) {
  return lazily_test::parse_json(lazily_test::spec_fixture_text("resilience", file));
}

// -- circuit_breaker.json --

TEST(test_circuit_breaker) {
  const auto fx = fixture("circuit_breaker.json");
  const auto& config = lazily_test::json_member(*fx, "config");
  Context ctx;
  CircuitBreakerCell cb(
      ctx, lazily_test::json_u64(lazily_test::json_member(config, "window")),
      lazily_test::json_u64(lazily_test::json_member(config, "failure_threshold")),
      lazily_test::json_u64(lazily_test::json_member(config, "reset_timeout")));
  auto sc = cb.state_cell();
  auto observed = ctx.computed<BreakerState>([sc](Compute& c) { return sc.get(c); });
  (void)ctx.get(observed);

  for (const auto& step_ptr : lazily_test::json_array(lazily_test::json_member(*fx, "steps"))) {
    const auto& item = *step_ptr;
    const auto& op = lazily_test::json_member(item, "op");
    lazily_test::AssertionKeys expected(std::string(__func__) + " expected",
                                        lazily_test::json_member(item, "expected"));
    const auto type = lazily_test::json_string(lazily_test::json_member(op, "type"));
    const auto now = lazily_test::json_u64(lazily_test::json_member(op, "now"));
    if (type == "record") {
      cb.record(ctx, lazily_test::json_bool(lazily_test::json_member(op, "success")), now);
    } else if (type == "allow") {
      assert(cb.allow(ctx, now) ==
             lazily_test::json_bool(lazily_test::json_member(item, "returns")));
    } else {
      // Fail closed (#lzscenariobodyskip). An unnamed op must not replay as the
      // last arm — the ledger books the step either way.
      REQUIRE(false, "unknown circuit-breaker op in fixture: " + type);
    }
    expected.assert_key_with("state", [&](const lazily_test::Json& value) {
      const auto state = lazily_test::json_string(value);
      // Fail closed (#lzscenariobodyskip). This ended in a bare
      // `: BreakerState::HalfOpen`, so any unrecognised spelling silently became
      // the HalfOpen expectation instead of failing.
      BreakerState want = BreakerState::HalfOpen;
      if (state == "Closed")
        want = BreakerState::Closed;
      else if (state == "Open")
        want = BreakerState::Open;
      else
        REQUIRE(state == "HalfOpen", "unknown breaker state in fixture: " + state);
      return cb.state() == want;
    });
    const bool was = ctx.is_set(observed);
    (void)ctx.get(observed);
    expected.with_sub("invalidates", [&](lazily_test::AssertionKeys& invalidates) {
      invalidates.assert_key("state", !was);
    });
  }
}

// -- retry.json --

TEST(test_retry) {
  const auto fx = fixture("retry.json");
  const auto& config = lazily_test::json_member(*fx, "config");
  Context ctx;
  RetryPolicyCell r(ctx, lazily_test::json_u64(lazily_test::json_member(config, "base")),
                    lazily_test::json_u64(lazily_test::json_member(config, "cap")));
  auto dc = r.delay_cell();
  auto observed = ctx.computed<uint64_t>([dc](Compute& c) { return dc.get(c); });
  (void)ctx.get(observed);

  for (const auto& step_ptr : lazily_test::json_array(lazily_test::json_member(*fx, "steps"))) {
    const auto& item = *step_ptr;
    lazily_test::AssertionKeys expected(std::string(__func__) + " expected",
                                        lazily_test::json_member(item, "expected"));
    assert(r.next_delay(ctx) == lazily_test::json_u64(lazily_test::json_member(item, "returns")));
    expected.assert_key("delay", r.delay(ctx));
    const bool was = ctx.is_set(observed);
    (void)ctx.get(observed);
    expected.with_sub("invalidates", [&](lazily_test::AssertionKeys& invalidates) {
      invalidates.assert_key("delay", !was);
    });
  }
}

// -- bulkhead.json --

TEST(test_bulkhead) {
  const auto fx = fixture("bulkhead.json");
  const auto& config = lazily_test::json_member(*fx, "config");
  Context ctx;
  BulkheadCell b(ctx, lazily_test::json_u64(lazily_test::json_member(config, "capacity")));
  auto uc = b.permits_in_use_cell();
  auto observed = ctx.computed<uint64_t>([uc](Compute& c) { return uc.get(c); });
  (void)ctx.get(observed);

  for (const auto& step_ptr : lazily_test::json_array(lazily_test::json_member(*fx, "steps"))) {
    const auto& item = *step_ptr;
    const auto& op = lazily_test::json_member(item, "op");
    lazily_test::AssertionKeys expected(std::string(__func__) + " expected",
                                        lazily_test::json_member(item, "expected"));
    const auto type = lazily_test::json_string(lazily_test::json_member(op, "type"));
    if (type == "acquire") {
      assert(b.acquire(ctx) == lazily_test::json_bool(lazily_test::json_member(item, "returns")));
    } else if (type == "release") {
      assert(lazily_test::json_member(item, "returns").is_null());
      b.release(ctx);
    } else {
      // Fail closed (#lzscenariobodyskip).
      REQUIRE(false, "unknown bulkhead op in fixture: " + type);
    }
    expected.assert_key("in_use", b.permits_in_use(ctx));
    const bool was = ctx.is_set(observed);
    (void)ctx.get(observed);
    expected.with_sub("invalidates", [&](lazily_test::AssertionKeys& invalidates) {
      invalidates.assert_key("in_use", !was);
    });
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

  for (const auto& step_ptr : lazily_test::json_array(lazily_test::json_member(*fx, "steps"))) {
    const auto& item = *step_ptr;
    const auto& op = lazily_test::json_member(item, "op");
    lazily_test::AssertionKeys expected(std::string(__func__) + " expected",
                                        lazily_test::json_member(item, "expected"));
    const auto type = lazily_test::json_string(lazily_test::json_member(op, "type"));
    const auto now = lazily_test::json_u64(lazily_test::json_member(op, "now"));
    bool got;
    if (type == "arm") {
      t.arm(ctx, now, lazily_test::json_u64(lazily_test::json_member(op, "timeout")));
      got = false;
    } else if (type == "tick") {
      got = t.tick(ctx, now);
    } else {
      // Fail closed (#lzscenariobodyskip).
      REQUIRE(false, "unknown timeout op in fixture: " + type);
      got = false;
    }
    assert(got == lazily_test::json_bool(lazily_test::json_member(item, "returns")));
    expected.assert_key("is_timed_out", t.is_timed_out(ctx));
    const bool was = ctx.is_set(observed);
    (void)ctx.get(observed);
    expected.with_sub("invalidates", [&](lazily_test::AssertionKeys& invalidates) {
      invalidates.assert_key("is_timed_out", !was);
    });
  }
}

int main() {
  REQUIRE_FIXTURES_LOADED(4);
  return test_count == test_passed ? 0 : 1;
}
