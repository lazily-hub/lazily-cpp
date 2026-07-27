// Cross-language conformance for distributed coordination (`#lzcoord`).
//
// A C++17 port of `lazily-rs/tests/coordination_conformance.rs`. Parses and
// replays each primitive's operations and expectations directly from the shared
// fixtures in `lazily-spec/conformance/coordination/*.json`.
//
// Invalidation is checked with the cache-survival technique: a `computed` slot
// observes the projected cell; after each op `was = ctx.is_set(observed)` is the
// pre-read cache state, so `!was == expected.invalidates.<reader>` (the reader
// was dropped iff the op invalidated it). The slot is re-read to re-prime.

#include <lazily/coordination.hpp>

#include <cassert>
#include <cstdint>
#include <optional>
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

static lazily_test::JsonPtr fixture(const std::string& name) {
  return lazily_test::parse_json(
      lazily_test::spec_fixture_text("coordination", name));
}

// LeaseCell — grant/renew/expire + fence monotonicity; holder invalidation.
TEST(test_lease) {
  const auto fx = fixture("lease.json");
  Context ctx;
  LeaseCell<uint64_t> lease(ctx);
  auto hc = lease.holder_cell();
  auto observed =
      ctx.computed<std::optional<uint64_t>>([hc](Compute& c) { return hc.get(c); });
  (void)ctx.get(observed);

  auto step = [&](bool inval) {
    bool was = ctx.is_set(observed);
    (void)ctx.get(observed);
    assert(!was == inval && "holder invalidation");
  };

  for (const auto& step_ptr :
       lazily_test::json_array(lazily_test::json_member(*fx, "steps"))) {
    const auto& item = *step_ptr;
    const auto& op = lazily_test::json_member(item, "op");
    const auto& expected = lazily_test::json_member(item, "expected");
    const auto type = lazily_test::json_string(lazily_test::json_member(op, "type"));
    const auto now = lazily_test::json_u64(lazily_test::json_member(op, "now"));
    if (type == "acquire") {
      assert(lease.acquire(
                 ctx,
                 lazily_test::json_u64(lazily_test::json_member(op, "peer")),
                 now,
                 lazily_test::json_u64(lazily_test::json_member(op, "ttl"))) ==
             lazily_test::json_optional_u64(
                 lazily_test::json_member(item, "returns")));
    } else if (type == "renew") {
      assert(lease.renew(
                 ctx,
                 lazily_test::json_u64(lazily_test::json_member(op, "peer")),
                 now,
                 lazily_test::json_u64(lazily_test::json_member(op, "ttl"))) ==
             lazily_test::json_bool(lazily_test::json_member(item, "returns")));
    } else {
      assert(type == "tick");
      assert(lease.tick(ctx, now) ==
             lazily_test::json_bool(lazily_test::json_member(item, "returns")));
    }
    assert(lease.holder(now) == lazily_test::json_optional_u64(
                                     lazily_test::json_member(expected, "holder")));
    assert(lease.is_held(now) ==
           lazily_test::json_bool(lazily_test::json_member(expected, "held")));
    assert(lease.fence() ==
           lazily_test::json_u64(lazily_test::json_member(expected, "fence")));
    step(lazily_test::json_bool(lazily_test::json_member(
        lazily_test::json_member(expected, "invalidates"), "holder")));
  }
}

// LeaderCell (me=1) — campaign/expire/contend handover; current_leader inval.
TEST(test_leader) {
  const auto fx = fixture("leader.json");
  Context ctx;
  LeaderCell<uint64_t> leader(
      ctx, lazily_test::json_u64(lazily_test::json_member(
               lazily_test::json_member(*fx, "config"), "me")));
  auto lc = leader.current_leader_cell();
  auto observed =
      ctx.computed<std::optional<uint64_t>>([lc](Compute& c) { return lc.get(c); });
  (void)ctx.get(observed);

  auto step = [&](bool inval) {
    bool was = ctx.is_set(observed);
    (void)ctx.get(observed);
    assert(!was == inval && "current_leader invalidation");
  };

  for (const auto& step_ptr :
       lazily_test::json_array(lazily_test::json_member(*fx, "steps"))) {
    const auto& item = *step_ptr;
    const auto& op = lazily_test::json_member(item, "op");
    const auto& expected = lazily_test::json_member(item, "expected");
    const auto type = lazily_test::json_string(lazily_test::json_member(op, "type"));
    const auto now = lazily_test::json_u64(lazily_test::json_member(op, "now"));
    LeaderRole got;
    if (type == "campaign") {
      got = leader.campaign(
          ctx, now, lazily_test::json_u64(lazily_test::json_member(op, "ttl")));
    } else if (type == "contend") {
      got = leader.contend(
          ctx, lazily_test::json_u64(lazily_test::json_member(op, "peer")),
          now, lazily_test::json_u64(lazily_test::json_member(op, "ttl")));
    } else {
      assert(type == "tick");
      got = leader.tick(ctx, now);
    }
    const auto role =
        lazily_test::json_string(lazily_test::json_member(expected, "role"));
    const auto want = role == "Leader"
                          ? LeaderRole::Leader
                          : (role == "Follower" ? LeaderRole::Follower
                                                : LeaderRole::Candidate);
    assert(got == want);
    assert(leader.current_leader(now) == lazily_test::json_optional_u64(
                                              lazily_test::json_member(
                                                  expected, "current_leader")));
    step(lazily_test::json_bool(lazily_test::json_member(
        lazily_test::json_member(expected, "invalidates"), "current_leader")));
  }
}

// LockCell — mutex + fencing validate; is_locked inval on state flip.
TEST(test_lock) {
  const auto fx = fixture("lock.json");
  Context ctx;
  LockCell<uint64_t> lock(ctx);
  auto lc = lock.is_locked_cell();
  auto observed = ctx.computed<bool>([lc](Compute& c) { return lc.get(c); });
  (void)ctx.get(observed);

  auto step = [&](bool inval) {
    bool was = ctx.is_set(observed);
    (void)ctx.get(observed);
    assert(!was == inval && "is_locked invalidation");
  };

  for (const auto& step_ptr :
       lazily_test::json_array(lazily_test::json_member(*fx, "steps"))) {
    const auto& item = *step_ptr;
    const auto& op = lazily_test::json_member(item, "op");
    const auto& expected = lazily_test::json_member(item, "expected");
    const auto type = lazily_test::json_string(lazily_test::json_member(op, "type"));
    const auto now = lazily_test::json_u64(lazily_test::json_member(op, "now"));
    if (type == "acquire") {
      assert(lock.acquire(
                 ctx,
                 lazily_test::json_u64(lazily_test::json_member(op, "peer")),
                 now,
                 lazily_test::json_u64(lazily_test::json_member(op, "ttl"))) ==
             lazily_test::json_optional_u64(
                 lazily_test::json_member(item, "returns")));
    } else if (type == "validate") {
      assert(lock.validate(
                 lazily_test::json_u64(lazily_test::json_member(op, "fence"))) ==
             lazily_test::json_bool(lazily_test::json_member(item, "returns")));
    } else {
      assert(type == "tick");
      assert(lock.tick(ctx, now) ==
             lazily_test::json_bool(lazily_test::json_member(item, "returns")));
    }
    assert(lock.is_locked(now) ==
           lazily_test::json_bool(lazily_test::json_member(expected, "is_locked")));
    assert(lock.fence() ==
           lazily_test::json_u64(lazily_test::json_member(expected, "fence")));
    step(lazily_test::json_bool(lazily_test::json_member(
        lazily_test::json_member(expected, "invalidates"), "is_locked")));
  }
}

// SemaphoreCell(capacity=2) — bounded permits; permits_available inval on change.
TEST(test_semaphore) {
  const auto fx = fixture("semaphore.json");
  Context ctx;
  SemaphoreCell sem(
      ctx, lazily_test::json_u64(lazily_test::json_member(
               lazily_test::json_member(*fx, "config"), "capacity")));
  auto pc = sem.permits_available_cell();
  auto observed = ctx.computed<uint64_t>([pc](Compute& c) { return pc.get(c); });
  (void)ctx.get(observed);

  auto step = [&](bool inval) {
    bool was = ctx.is_set(observed);
    (void)ctx.get(observed);
    assert(!was == inval && "permits_available invalidation");
  };

  for (const auto& step_ptr :
       lazily_test::json_array(lazily_test::json_member(*fx, "steps"))) {
    const auto& item = *step_ptr;
    const auto& op = lazily_test::json_member(item, "op");
    const auto& expected = lazily_test::json_member(item, "expected");
    const auto type = lazily_test::json_string(lazily_test::json_member(op, "type"));
    if (type == "acquire") {
      assert(sem.acquire(ctx) ==
             lazily_test::json_bool(lazily_test::json_member(item, "returns")));
    } else {
      assert(type == "release");
      assert(lazily_test::json_member(item, "returns").is_null());
      sem.release(ctx);
    }
    assert(sem.permits_available(ctx) == lazily_test::json_u64(
                                              lazily_test::json_member(
                                                  expected, "permits_available")));
    step(lazily_test::json_bool(lazily_test::json_member(
        lazily_test::json_member(expected, "invalidates"), "permits_available")));
  }
}

// QuorumCell(total=5) — barrier, required = 5/2+1 = 3; is_open inval on flip.
TEST(test_quorum) {
  const auto fx = fixture("quorum.json");
  Context ctx;
  auto q = BarrierCell<uint64_t>::quorum(
      ctx, lazily_test::json_u64(lazily_test::json_member(
               lazily_test::json_member(*fx, "config"), "total")));
  auto oc = q.is_open_cell();
  auto observed = ctx.computed<bool>([oc](Compute& c) { return oc.get(c); });
  (void)ctx.get(observed);

  auto step = [&](bool inval) {
    bool was = ctx.is_set(observed);
    (void)ctx.get(observed);
    assert(!was == inval && "is_open invalidation");
  };

  for (const auto& step_ptr :
       lazily_test::json_array(lazily_test::json_member(*fx, "steps"))) {
    const auto& item = *step_ptr;
    const auto& op = lazily_test::json_member(item, "op");
    const auto& expected = lazily_test::json_member(item, "expected");
    assert(lazily_test::json_string(lazily_test::json_member(op, "type")) == "vote");
    assert(q.arrive(
               ctx, lazily_test::json_u64(lazily_test::json_member(op, "peer"))) ==
           lazily_test::json_bool(lazily_test::json_member(item, "returns")));
    assert(q.count() ==
           lazily_test::json_u64(lazily_test::json_member(expected, "votes")));
    assert(q.is_open(ctx) ==
           lazily_test::json_bool(lazily_test::json_member(expected, "is_open")));
    step(lazily_test::json_bool(lazily_test::json_member(
        lazily_test::json_member(expected, "invalidates"), "is_open")));
  }
}

int main() {
  REQUIRE_FIXTURES_LOADED(5); return test_count == test_passed ? 0 : 1; }
