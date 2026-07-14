// Cross-language conformance for distributed coordination (`#lzcoord`).
//
// A C++17 port of `lazily-rs/tests/coordination_conformance.rs`. Replays each
// primitive's op sequence transcribed from the shared fixtures in
// `lazily-spec/conformance/coordination/*.json` (vendored under
// tests/conformance/coordination/), asserting the returned value, the projected
// readers (`expected.*`), and reader invalidation.
//
// Invalidation is checked with the cache-survival technique: a `computed` slot
// observes the projected cell; after each op `was = ctx.is_set(observed)` is the
// pre-read cache state, so `!was == expected.invalidates.<reader>` (the reader
// was dropped iff the op invalidated it). The slot is re-read to re-prime.

#include <lazily/coordination.hpp>

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

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

// Read a vendored fixture's raw text (used to assert the `"model"` marker).
static std::string fixture_text(const std::string& name) {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "conformance/coordination" / name;
  std::ifstream input(path);
  assert(input && "vendored coordination fixture present");
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

static void assert_model(const std::string& name, const std::string& model) {
  const auto text = fixture_text(name);
  assert(text.find("\"model\": \"" + model + "\"") != std::string::npos &&
         "fixture model marker present");
}

// LeaseCell — grant/renew/expire + fence monotonicity; holder invalidation.
TEST(test_lease) {
  assert_model("lease.json", "LeaseCell");
  Context ctx;
  LeaseCell<uint64_t> lease(ctx);
  auto hc = lease.holder_cell();
  auto observed =
      ctx.computed<std::optional<uint64_t>>([hc](Context& c) { return hc.get(c); });
  (void)ctx.get(observed);

  auto step = [&](bool inval) {
    bool was = ctx.is_set(observed);
    (void)ctx.get(observed);
    assert(!was == inval && "holder invalidation");
  };

  // acquire peer=1 now=0 ttl=10 -> fence 1
  assert(lease.acquire(ctx, 1, 0, 10) == std::optional<uint64_t>(1));
  assert(lease.holder(0) == std::optional<uint64_t>(1));
  assert(lease.is_held(0) && lease.fence() == 1);
  step(true);

  // acquire peer=2 now=1 -> rejected (held)
  assert(lease.acquire(ctx, 2, 1, 10) == std::nullopt);
  assert(lease.holder(1) == std::optional<uint64_t>(1));
  assert(lease.is_held(1) && lease.fence() == 1);
  step(false);

  // renew peer=1 now=5 -> true, keeps fence
  assert(lease.renew(ctx, 1, 5, 10) == true);
  assert(lease.holder(5) == std::optional<uint64_t>(1));
  assert(lease.is_held(5) && lease.fence() == 1);
  step(false);

  // tick now=10 -> not yet expired (now < expiry 15)
  assert(lease.tick(ctx, 10) == false);
  assert(lease.holder(10) == std::optional<uint64_t>(1));
  assert(lease.is_held(10) && lease.fence() == 1);
  step(false);

  // tick now=15 -> expired
  assert(lease.tick(ctx, 15) == true);
  assert(lease.holder(15) == std::nullopt);
  assert(!lease.is_held(15) && lease.fence() == 1);
  step(true);

  // acquire peer=2 now=16 -> new grant, fence 2
  assert(lease.acquire(ctx, 2, 16, 10) == std::optional<uint64_t>(2));
  assert(lease.holder(16) == std::optional<uint64_t>(2));
  assert(lease.is_held(16) && lease.fence() == 2);
  step(true);
}

// LeaderCell (me=1) — campaign/expire/contend handover; current_leader inval.
TEST(test_leader) {
  assert_model("leader.json", "LeaderCell");
  Context ctx;
  LeaderCell<uint64_t> leader(ctx, 1);
  auto lc = leader.current_leader_cell();
  auto observed =
      ctx.computed<std::optional<uint64_t>>([lc](Context& c) { return lc.get(c); });
  (void)ctx.get(observed);

  auto step = [&](bool inval) {
    bool was = ctx.is_set(observed);
    (void)ctx.get(observed);
    assert(!was == inval && "current_leader invalidation");
  };

  // campaign now=0 ttl=10 -> Leader
  assert(leader.campaign(ctx, 0, 10) == LeaderRole::Leader);
  assert(leader.current_leader(0) == std::optional<uint64_t>(1));
  step(true);

  // tick now=5 -> still Leader
  assert(leader.tick(ctx, 5) == LeaderRole::Leader);
  assert(leader.current_leader(5) == std::optional<uint64_t>(1));
  step(false);

  // tick now=10 -> expiry -> Candidate
  assert(leader.tick(ctx, 10) == LeaderRole::Candidate);
  assert(leader.current_leader(10) == std::nullopt);
  step(true);

  // contend peer=2 now=11 ttl=10 -> Follower
  assert(leader.contend(ctx, 2, 11, 10) == LeaderRole::Follower);
  assert(leader.current_leader(11) == std::optional<uint64_t>(2));
  step(true);

  // tick now=15 -> still Follower
  assert(leader.tick(ctx, 15) == LeaderRole::Follower);
  assert(leader.current_leader(15) == std::optional<uint64_t>(2));
  step(false);
}

// LockCell — mutex + fencing validate; is_locked inval on state flip.
TEST(test_lock) {
  assert_model("lock.json", "LockCell");
  Context ctx;
  LockCell<uint64_t> lock(ctx);
  auto lc = lock.is_locked_cell();
  auto observed = ctx.computed<bool>([lc](Context& c) { return lc.get(c); });
  (void)ctx.get(observed);

  auto step = [&](bool inval) {
    bool was = ctx.is_set(observed);
    (void)ctx.get(observed);
    assert(!was == inval && "is_locked invalidation");
  };

  // acquire peer=1 now=0 ttl=10 -> fence 1
  assert(lock.acquire(ctx, 1, 0, 10) == std::optional<uint64_t>(1));
  assert(lock.is_locked(0) && lock.fence() == 1);
  step(true);

  // acquire peer=2 now=1 -> rejected
  assert(lock.acquire(ctx, 2, 1, 10) == std::nullopt);
  assert(lock.is_locked(1) && lock.fence() == 1);
  step(false);

  // validate fence=1 now=2 -> true (current)
  assert(lock.validate(1) == true);
  assert(lock.is_locked(2) && lock.fence() == 1);
  step(false);

  // tick now=10 -> expired -> unlocked
  assert(lock.tick(ctx, 10) == true);
  assert(!lock.is_locked(10) && lock.fence() == 1);
  step(true);

  // acquire peer=2 now=11 -> fence 2
  assert(lock.acquire(ctx, 2, 11, 10) == std::optional<uint64_t>(2));
  assert(lock.is_locked(11) && lock.fence() == 2);
  step(true);

  // validate fence=1 now=12 -> false (stale)
  assert(lock.validate(1) == false);
  assert(lock.is_locked(12) && lock.fence() == 2);
  step(false);

  // validate fence=2 now=12 -> true (current)
  assert(lock.validate(2) == true);
  assert(lock.is_locked(12) && lock.fence() == 2);
  step(false);
}

// SemaphoreCell(capacity=2) — bounded permits; permits_available inval on change.
TEST(test_semaphore) {
  assert_model("semaphore.json", "SemaphoreCell");
  Context ctx;
  SemaphoreCell sem(ctx, 2);
  auto pc = sem.permits_available_cell();
  auto observed = ctx.computed<uint64_t>([pc](Context& c) { return pc.get(c); });
  (void)ctx.get(observed);

  auto step = [&](bool inval) {
    bool was = ctx.is_set(observed);
    (void)ctx.get(observed);
    assert(!was == inval && "permits_available invalidation");
  };

  assert(sem.acquire(ctx) == true);
  assert(sem.permits_available(ctx) == 1);
  step(true);

  assert(sem.acquire(ctx) == true);
  assert(sem.permits_available(ctx) == 0);
  step(true);

  // full
  assert(sem.acquire(ctx) == false);
  assert(sem.permits_available(ctx) == 0);
  step(false);

  sem.release(ctx);
  assert(sem.permits_available(ctx) == 1);
  step(true);

  sem.release(ctx);
  assert(sem.permits_available(ctx) == 2);
  step(true);

  // saturates at capacity
  sem.release(ctx);
  assert(sem.permits_available(ctx) == 2);
  step(false);
}

// QuorumCell(total=5) — barrier, required = 5/2+1 = 3; is_open inval on flip.
TEST(test_quorum) {
  assert_model("quorum.json", "QuorumCell");
  Context ctx;
  auto q = BarrierCell<uint64_t>::quorum(ctx, 5);
  auto oc = q.is_open_cell();
  auto observed = ctx.computed<bool>([oc](Context& c) { return oc.get(c); });
  (void)ctx.get(observed);

  auto step = [&](bool inval) {
    bool was = ctx.is_set(observed);
    (void)ctx.get(observed);
    assert(!was == inval && "is_open invalidation");
  };

  assert(q.arrive(ctx, 1) == false);
  assert(q.count() == 1 && q.is_open(ctx) == false);
  step(false);

  assert(q.arrive(ctx, 2) == false);
  assert(q.count() == 2 && q.is_open(ctx) == false);
  step(false);

  assert(q.arrive(ctx, 3) == true);
  assert(q.count() == 3 && q.is_open(ctx) == true);
  step(true);

  assert(q.arrive(ctx, 4) == true);
  assert(q.count() == 4 && q.is_open(ctx) == true);
  step(false);

  // duplicate vote is idempotent
  assert(q.arrive(ctx, 1) == true);
  assert(q.count() == 4 && q.is_open(ctx) == true);
  step(false);
}

int main() { return test_count == test_passed ? 0 : 1; }
