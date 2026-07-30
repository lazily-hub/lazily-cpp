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
#include <optional>
#include <set>
#include <string>
#include <vector>
#include "test_assertion_keys.hpp"
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

static lazily_test::JsonPtr fixture() {
  return lazily_test::parse_json(lazily_test::spec_fixture_text(
      "membership", "membership_lifecycle.json"));
}

// Full lifecycle replay — parses `config` and `steps` from the fixture.
// Each step asserts per-peer state, the alive set, and PeerSet invalidation.
TEST(test_membership_lifecycle) {
  const auto fx = fixture();
  const auto& config_json = lazily_test::json_member(*fx, "config");
  MembershipConfig config;
  config.phi_threshold = lazily_test::json_number(
      lazily_test::json_member(config_json, "phi_threshold"));
  config.suspect_timeout = lazily_test::json_u64(
      lazily_test::json_member(config_json, "suspect_timeout"));
  config.max_samples = lazily_test::json_u64(
      lazily_test::json_member(config_json, "max_samples"));
  config.min_std =
      lazily_test::json_number(lazily_test::json_member(config_json, "min_std"));

  Context ctx;
  MembershipCell<uint64_t> m(ctx, config);
  auto set = m.peer_set();
  auto observed =
      ctx.computed<std::set<uint64_t>>([set](Compute& c) { return c.get(set); });
  (void)ctx.get(observed);

  for (const auto& step_ptr :
       lazily_test::json_array(lazily_test::json_member(*fx, "steps"))) {
    const auto& item = *step_ptr;
    const auto& op_json = lazily_test::json_member(item, "op");
    lazily_test::AssertionKeys expected(
        std::string(__func__) + " expected", lazily_test::json_member(item, "expected"));
    const auto op =
        lazily_test::json_string(lazily_test::json_member(op_json, "type"));
    const auto now =
        lazily_test::json_u64(lazily_test::json_member(op_json, "now"));
    const auto* peer_json = op_json.find("peer");
    const auto peer = peer_json == nullptr ? 0 : lazily_test::json_u64(*peer_json);
    if (op == "join") {
      m.join(ctx, peer, now);
    } else if (op == "heartbeat") {
      m.heartbeat(ctx, peer, now);
    } else if (op == "leave") {
      m.leave(ctx, peer, now);
    } else if (op == "tick") {
      m.tick(ctx, now);
    } else {
      assert(false && "unknown op");
    }

    // Per-peer state.
    expected.assert_key_with("states", [&](const lazily_test::Json& states) {
      assert(states.is_object());
      for (const auto& want : states.object) {
        const auto state = lazily_test::json_string(*want.second);
        const auto want_state =
            state == "Alive"
                ? PeerState::Alive
                : (state == "Suspect"
                       ? PeerState::Suspect
                       : (state == "Dead" ? PeerState::Dead : PeerState::Left));
        const auto got = m.state(std::stoull(want.first));
        if (!got || *got != want_state) return false;
      }
      return true;
    });
    // Alive set (the reactive PeerSet).
    expected.assert_key(
        "alive_set", m.peer_set(ctx),
        [](const lazily_test::Json& value) {
          std::set<uint64_t> alive_set;
          for (const auto& alive : lazily_test::json_array(value))
            alive_set.insert(lazily_test::json_u64(*alive));
          return alive_set;
        });

    // PeerSet invalidation: the observed reader is dropped from cache exactly
    // when the alive set changed.
    const bool was_cached = ctx.is_set(observed);
    (void)ctx.get(observed);
    expected.assert_key("invalidates", !was_cached);
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
