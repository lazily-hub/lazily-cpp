// Cross-language conformance for membership + failure detection (`#lzmemb`).
//
// C++ port of `lazily-rs/tests/membership_conformance.rs`. Replays the SWIM
// lifecycle fixture `lazily-spec/conformance/membership/membership_lifecycle.json`
// (read from the sibling lazily-spec checkout): each op asserts the acted
// peers' `state`, the `alive_set` (the reactive `PeerSet`), and that the
// `PeerSet` reader invalidates exactly when the alive set changes (via
// `ctx.is_set`). Also replays the Rust unit tests (`PhiAccrual` thresholds +
// SWIM event stream) for phi-formula parity.

#include <lazily/membership.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
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

static std::string fixture_text() {
  return lazily_test::spec_fixture_text("membership", "membership_lifecycle.json");
}

// The canonical fixture carries the `"model": "MembershipCell"` marker and the
// canonical config replayed below.
TEST(test_fixture_present_and_model_marker) {
  const auto fixture = fixture_text();
  assert(fixture.find("\"model\": \"MembershipCell\"") != std::string::npos);
  assert(fixture.find("\"kind\": \"Membership\"") != std::string::npos);
  assert(fixture.find("\"phi_threshold\": 8.0") != std::string::npos);
}

// Full lifecycle replay — transcribes `steps` from membership_lifecycle.json.
// Each step asserts per-peer state, the alive set, and PeerSet invalidation.
TEST(test_membership_lifecycle) {
  // config: phi_threshold 8.0, suspect_timeout 5, max_samples 100, min_std 0.1.
  MembershipConfig config;
  config.phi_threshold = 8.0;
  config.suspect_timeout = 5;
  config.max_samples = 100;
  config.min_std = 0.1;

  Context ctx;
  MembershipCell<uint64_t> m(ctx, config);
  auto set = m.peer_set();
  auto observed =
      ctx.computed<std::set<uint64_t>>([set](Context& c) { return c.get(set); });
  (void)ctx.get(observed);

  struct Step {
    // op
    const char* op;
    uint64_t peer;  // ignored for tick
    uint64_t now;
    // expected
    std::vector<std::pair<uint64_t, PeerState>> states;
    std::set<uint64_t> alive_set;
    bool invalidates;
  };

  const std::vector<Step> steps = {
      {"join", 1, 0, {{1, PeerState::Alive}}, {1}, true},
      {"heartbeat", 1, 1, {{1, PeerState::Alive}}, {1}, false},
      {"heartbeat", 1, 2, {{1, PeerState::Alive}}, {1}, false},
      {"heartbeat", 1, 3, {{1, PeerState::Alive}}, {1}, false},
      {"tick", 0, 3, {{1, PeerState::Alive}}, {1}, false},
      {"tick", 0, 100, {{1, PeerState::Suspect}}, {}, true},
      {"tick", 0, 106, {{1, PeerState::Dead}}, {}, false},
      {"join", 2, 106, {{1, PeerState::Dead}, {2, PeerState::Alive}}, {2}, true},
      {"leave", 2, 107, {{1, PeerState::Dead}, {2, PeerState::Left}}, {}, true},
  };

  for (const auto& step : steps) {
    std::string op = step.op;
    if (op == "join") {
      m.join(ctx, step.peer, step.now);
    } else if (op == "heartbeat") {
      m.heartbeat(ctx, step.peer, step.now);
    } else if (op == "leave") {
      m.leave(ctx, step.peer, step.now);
    } else if (op == "tick") {
      m.tick(ctx, step.now);
    } else {
      assert(false && "unknown op");
    }

    // Per-peer state.
    for (const auto& want : step.states) {
      auto got = m.state(want.first);
      assert(got && *got == want.second && "peer state after op");
    }
    // Alive set (the reactive PeerSet).
    assert(m.peer_set(ctx) == step.alive_set && "alive_set after op");

    // PeerSet invalidation: the observed reader is dropped from cache exactly
    // when the alive set changed.
    bool was_cached = ctx.is_set(observed);
    (void)ctx.get(observed);
    assert((!was_cached) == step.invalidates && "invalidation after op");
  }
}

// Rust unit test `phi_low_when_current_high_after_gap` — phi-formula parity.
TEST(test_phi_low_current_high_after_gap) {
  PhiAccrual d(100, 0.1);
  d.heartbeat(0);
  d.heartbeat(1);
  d.heartbeat(2);
  d.heartbeat(3);
  assert(d.phi(3) < 8.0 && "phi at last heartbeat should be low");
  assert(d.phi(100) > 8.0 && "phi after a long gap should be high");
}

// Rust unit test `lifecycle_transitions` — core state machine + event stream.
TEST(test_core_lifecycle_transitions) {
  using Event = PeerChangeEvent<uint64_t>;
  MembershipCore<uint64_t> core(MembershipConfig{});
  assert((core.join(1, 0) == std::vector<Event>{Event::joined(1)}));
  core.heartbeat(1, 1);
  core.heartbeat(1, 2);
  core.heartbeat(1, 3);
  assert(core.tick(3).empty());
  assert(core.state(1) == std::optional<PeerState>(PeerState::Alive));
  assert((core.tick(100) ==
          std::vector<Event>{
              Event::state_changed(1, PeerState::Alive, PeerState::Suspect)}));
  assert((core.tick(106) ==
          std::vector<Event>{
              Event::state_changed(1, PeerState::Suspect, PeerState::Dead)}));
  assert(core.alive_set().empty());
}

// Rust unit test `heartbeat_refutes_suspicion`.
TEST(test_core_heartbeat_refutes_suspicion) {
  using Event = PeerChangeEvent<uint64_t>;
  MembershipCore<uint64_t> core(MembershipConfig{});
  core.join(1, 0);
  core.heartbeat(1, 1);
  core.heartbeat(1, 2);
  core.tick(100);  // -> Suspect
  assert(core.state(1) == std::optional<PeerState>(PeerState::Suspect));
  auto ev = core.heartbeat(1, 101);  // refute
  assert(core.state(1) == std::optional<PeerState>(PeerState::Alive));
  assert((ev == std::vector<Event>{Event::state_changed(1, PeerState::Suspect,
                                                        PeerState::Alive)}));
}

int main() {
  REQUIRE_FIXTURES_LOADED(1); return test_count == test_passed ? 0 : 1; }
