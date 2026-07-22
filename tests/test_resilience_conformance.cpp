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
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
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

static std::string fixture_text(const char* file) {
  return lazily_test::spec_fixture_text("resilience", file);
}

// -- circuit_breaker.json --

TEST(test_circuit_breaker) {
  const auto fx = fixture_text("circuit_breaker.json");
  assert(fx.find("\"model\": \"CircuitBreakerCell\"") != std::string::npos);

  // config: window=3, failure_threshold=2, reset_timeout=5.
  Context ctx;
  CircuitBreakerCell cb(ctx, 3, 2, 5);
  auto sc = cb.state_cell();
  auto observed = ctx.computed<BreakerState>([sc](Context& c) { return sc.get(c); });
  (void)ctx.get(observed);

  struct Step {
    bool is_record;
    bool success;   // record only
    uint64_t now;
    bool returns;   // allow only
    BreakerState state;
    bool invalidates;
  };
  const std::vector<Step> steps = {
      {true, false, 0, false, BreakerState::Closed, false},
      {true, false, 1, false, BreakerState::Open, true},
      {false, false, 2, false, BreakerState::Open, false},
      {false, false, 6, true, BreakerState::HalfOpen, true},
      {true, true, 6, false, BreakerState::Closed, true},
  };

  for (const auto& s : steps) {
    if (s.is_record) {
      cb.record(ctx, s.success, s.now);
    } else {
      assert(cb.allow(ctx, s.now) == s.returns && "allow result");
    }
    assert(cb.state() == s.state && "breaker state");
    bool was = ctx.is_set(observed);
    (void)ctx.get(observed);
    assert((!was) == s.invalidates && "state invalidation");
  }
}

// -- retry.json --

TEST(test_retry) {
  const auto fx = fixture_text("retry.json");
  assert(fx.find("\"model\": \"RetryPolicyCell\"") != std::string::npos);

  // config: base=100, cap=2000.
  Context ctx;
  RetryPolicyCell r(ctx, 100, 2000);
  auto dc = r.delay_cell();
  auto observed = ctx.computed<uint64_t>([dc](Context& c) { return dc.get(c); });
  (void)ctx.get(observed);

  struct Step {
    uint64_t returns;
    uint64_t delay;
    bool invalidates;
  };
  const std::vector<Step> steps = {
      {100, 100, true},   {200, 200, true},   {400, 400, true},
      {800, 800, true},   {1600, 1600, true}, {2000, 2000, true},
      {2000, 2000, false},
  };

  for (const auto& s : steps) {
    assert(r.next_delay(ctx) == s.returns && "next delay");
    assert(r.delay(ctx) == s.delay && "delay reader");
    bool was = ctx.is_set(observed);
    (void)ctx.get(observed);
    assert((!was) == s.invalidates && "delay invalidation");
  }
}

// -- bulkhead.json --

TEST(test_bulkhead) {
  const auto fx = fixture_text("bulkhead.json");
  assert(fx.find("\"model\": \"BulkheadCell\"") != std::string::npos);

  // config: capacity=2.
  Context ctx;
  BulkheadCell b(ctx, 2);
  auto uc = b.permits_in_use_cell();
  auto observed = ctx.computed<uint64_t>([uc](Context& c) { return uc.get(c); });
  (void)ctx.get(observed);

  struct Step {
    bool is_acquire;
    bool returns;  // acquire only
    uint64_t in_use;
    bool invalidates;
  };
  const std::vector<Step> steps = {
      {true, true, 1, true},  {true, true, 2, true}, {true, false, 2, false},
      {false, false, 1, true}, {false, false, 0, true},
  };

  for (const auto& s : steps) {
    if (s.is_acquire) {
      assert(b.acquire(ctx) == s.returns && "acquire result");
    } else {
      b.release(ctx);
    }
    assert(b.permits_in_use(ctx) == s.in_use && "permits_in_use");
    bool was = ctx.is_set(observed);
    (void)ctx.get(observed);
    assert((!was) == s.invalidates && "in_use invalidation");
  }
}

// -- timeout.json --

TEST(test_timeout) {
  const auto fx = fixture_text("timeout.json");
  assert(fx.find("\"model\": \"TimeoutCell\"") != std::string::npos);

  Context ctx;
  TimeoutCell t(ctx);
  auto tc = t.is_timed_out_cell();
  auto observed = ctx.computed<bool>([tc](Context& c) { return tc.get(c); });
  (void)ctx.get(observed);

  struct Step {
    bool is_arm;
    uint64_t now;
    uint64_t timeout;  // arm only
    bool returns;
    bool is_timed_out;
    bool invalidates;
  };
  const std::vector<Step> steps = {
      {true, 0, 5, false, false, false},
      {false, 3, 0, false, false, false},
      {false, 5, 0, true, true, true},
      {false, 9, 0, false, true, false},
  };

  for (const auto& s : steps) {
    bool got;
    if (s.is_arm) {
      t.arm(ctx, s.now, s.timeout);
      got = false;
    } else {
      got = t.tick(ctx, s.now);
    }
    assert(got == s.returns && "timeout edge");
    assert(t.is_timed_out(ctx) == s.is_timed_out && "is_timed_out reader");
    bool was = ctx.is_set(observed);
    (void)ctx.get(observed);
    assert((!was) == s.invalidates && "is_timed_out invalidation");
  }
}

int main() {
  REQUIRE_FIXTURES_LOADED(4);
  return test_count == test_passed ? 0 : 1;
}
