// Cross-language conformance for the presence + ephemeral plane
// (`#lzpresence`) — port of `lazily-rs/tests/presence_conformance.rs`.
//
// Config, operations, and expectations are parsed directly from the shared fixtures
// (`lazily-spec/conformance/presence/{presence,awareness,ephemeral}.json`,
// read from the sibling lazily-spec checkout). Each step asserts the op's
// projected reader value(s) (`expected.*`) and INVALIDATION (`expected.
// invalidates.*`) via the `computed` + `is_set` cache-survival technique:
// before re-reading the observer slot, `is_set` is true iff the cached value
// survived (no invalidation), so `!was == invalidates`.

#include <lazily/core.hpp>
#include <lazily/presence.hpp>

#include "test_assertion_keys.hpp"
#include "test_json.hpp"
#include "test_spec_fixture.hpp"
#include <cassert>
#include <map>
#include <optional>
#include <set>
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

using PresenceMap = std::map<uint64_t, std::string>;

static lazily_test::JsonPtr fixture(const std::string& name) {
  return lazily_test::parse_json(lazily_test::spec_fixture_text("presence", name));
}

static PresenceMap json_presence_map(const lazily_test::Json& value) {
  assert(value.is_object());
  PresenceMap result;
  for (const auto& entry : value.object) {
    result.emplace(std::stoull(entry.first), lazily_test::json_string(*entry.second));
  }
  return result;
}

// -- PresenceCell: heartbeat / evict / TTL tick; live-view invalidation --
TEST(test_presence) {
  const auto fx = fixture("presence.json");
  Context ctx;
  const uint64_t ttl = lazily_test::json_u64(
      lazily_test::json_member(lazily_test::json_member(*fx, "config"), "ttl"));
  PresenceCell<uint64_t, std::string> cell(ctx, ttl);
  auto pc = cell.present_cell();
  auto observed = ctx.computed<PresenceMap>([pc](Compute& c) { return c.get(pc); });
  (void)ctx.get(observed); // prime the observer cache

  for (const auto& step_ptr : lazily_test::json_array(lazily_test::json_member(*fx, "steps"))) {
    const auto& item = *step_ptr;
    const auto& op = lazily_test::json_member(item, "op");
    lazily_test::AssertionKeys expected(std::string(__func__) + " expected",
                                        lazily_test::json_member(item, "expected"));
    const auto type = lazily_test::json_string(lazily_test::json_member(op, "type"));
    const auto now = lazily_test::json_u64(lazily_test::json_member(op, "now"));
    if (type == "heartbeat") {
      cell.heartbeat(ctx, lazily_test::json_u64(lazily_test::json_member(op, "peer")),
                     lazily_test::json_string(lazily_test::json_member(op, "value")), now);
    } else if (type == "evict") {
      cell.evict(ctx, lazily_test::json_u64(lazily_test::json_member(op, "peer")), now);
    } else if (type == "tick") {
      cell.tick(ctx, now);
    } else {
      // Fail closed (#lzscenariobodyskip). An unnamed op must not replay as the
      // last arm — the ledger books the step either way.
      REQUIRE(false, "unknown presence op in fixture: " + type);
    }
    // Whole-map equality, with the KEY SET routed through the tracker so the
    // completeness of the comparison is visible from outside this site
    // (`#lzsubblockkeyset`).
    const PresenceMap present = cell.present(ctx);
    std::set<std::string> present_peers;
    for (const auto& entry : present)
      present_peers.insert(std::to_string(entry.first));
    expected.assert_key_set_with("present", present_peers, [&](const lazily_test::Json& want) {
      return present == json_presence_map(want);
    });
    // INVALIDATION via the `computed` + `is_set` cache-survival technique: the
    // cached value survived iff nothing invalidated the observer slot.
    const bool was = ctx.is_set(observed);
    (void)ctx.get(observed);
    expected.with_sub("invalidates", [&](lazily_test::AssertionKeys& invalidates) {
      invalidates.assert_key("present", !was);
    });
  }
}

// -- AwarenessCell: last-writer-per-peer, no merge; TTL tick --
TEST(test_awareness) {
  const auto fx = fixture("awareness.json");
  Context ctx;
  const uint64_t ttl = lazily_test::json_u64(
      lazily_test::json_member(lazily_test::json_member(*fx, "config"), "ttl"));
  AwarenessCell<uint64_t, std::string> cell(ctx, ttl);
  auto pc = cell.present_cell();
  auto observed = ctx.computed<PresenceMap>([pc](Compute& c) { return c.get(pc); });
  (void)ctx.get(observed);

  for (const auto& step_ptr : lazily_test::json_array(lazily_test::json_member(*fx, "steps"))) {
    const auto& item = *step_ptr;
    const auto& op = lazily_test::json_member(item, "op");
    lazily_test::AssertionKeys expected(std::string(__func__) + " expected",
                                        lazily_test::json_member(item, "expected"));
    const auto type = lazily_test::json_string(lazily_test::json_member(op, "type"));
    const auto now = lazily_test::json_u64(lazily_test::json_member(op, "now"));
    if (type == "set") {
      cell.set(ctx, lazily_test::json_u64(lazily_test::json_member(op, "peer")),
               lazily_test::json_string(lazily_test::json_member(op, "value")), now);
    } else if (type == "tick") {
      cell.tick(ctx, now);
    } else {
      // Fail closed (#lzscenariobodyskip).
      REQUIRE(false, "unknown presence-map op in fixture: " + type);
    }
    // Whole-map equality, with the KEY SET routed through the tracker so the
    // completeness of the comparison is visible from outside this site
    // (`#lzsubblockkeyset`).
    const PresenceMap present = cell.present(ctx);
    std::set<std::string> present_peers;
    for (const auto& entry : present)
      present_peers.insert(std::to_string(entry.first));
    expected.assert_key_set_with("present", present_peers, [&](const lazily_test::Json& want) {
      return present == json_presence_map(want);
    });
    const bool was = ctx.is_set(observed);
    (void)ctx.get(observed);
    expected.with_sub("invalidates", [&](lazily_test::AssertionKeys& invalidates) {
      invalidates.assert_key("present", !was);
    });
  }

  // last-writer-per-peer visible via non-reactive get
  assert(cell.get(1, 6) == std::nullopt); // expired by now 7 already ticked
}

// -- EphemeralCell: single value auto-expiry; value invalidation --
TEST(test_ephemeral) {
  const auto fx = fixture("ephemeral.json");
  Context ctx;
  EphemeralCell<std::string> cell(ctx);
  auto vc = cell.value_cell();
  auto observed = ctx.computed<std::optional<std::string>>([vc](Compute& c) { return c.get(vc); });
  (void)ctx.get(observed);

  for (const auto& step_ptr : lazily_test::json_array(lazily_test::json_member(*fx, "steps"))) {
    const auto& item = *step_ptr;
    const auto& op = lazily_test::json_member(item, "op");
    lazily_test::AssertionKeys expected(std::string(__func__) + " expected",
                                        lazily_test::json_member(item, "expected"));
    const auto type = lazily_test::json_string(lazily_test::json_member(op, "type"));
    const auto now = lazily_test::json_u64(lazily_test::json_member(op, "now"));
    if (type == "set") {
      cell.set(ctx, lazily_test::json_string(lazily_test::json_member(op, "value")), now,
               lazily_test::json_u64(lazily_test::json_member(op, "ttl")));
    } else if (type == "tick") {
      cell.tick(ctx, now);
    } else {
      // Fail closed (#lzscenariobodyskip).
      REQUIRE(false, "unknown ephemeral op in fixture: " + type);
    }
    expected.assert_key("value", cell.value(ctx), lazily_test::json_optional_string);
    const bool was = ctx.is_set(observed);
    (void)ctx.get(observed);
    expected.with_sub("invalidates", [&](lazily_test::AssertionKeys& invalidates) {
      invalidates.assert_key("value", !was);
    });
  }
}

// -- Pure cores mirror the Rust unit tests --
TEST(test_cores) {
  // EphemeralCore expires and overwrites.
  EphemeralCore<std::string> e;
  e.set("a", 0, 5);
  e.tick(3);
  assert(e.value() == std::optional<std::string>("a"));
  e.tick(5);
  assert(e.value() == std::nullopt);
  e.set("b", 6, 5);
  e.set("c", 10, 5); // overwrite before expiry
  assert(e.value() == std::optional<std::string>("c"));

  // EphemeralMapCore presence evict + TTL.
  EphemeralMapCore<uint64_t, std::string> m;
  m.set(1, "online", 0, 5);
  m.set(2, "online", 1, 5);
  m.evict(2);
  assert(m.present(2).size() == 1);
  m.tick(6); // peer 1 expires at 5
  assert(m.present(6).empty());

  // Awareness last-writer.
  EphemeralMapCore<uint64_t, std::string> a;
  a.set(1, "cursor-a", 0, 5);
  a.set(1, "cursor-a2", 2, 5);
  assert(a.get(1, 2) == std::optional<std::string>("cursor-a2"));

  // Durable sink statically rejects ephemeral values (compile-time).
  durable_persist(42); // int is durable-compatible
  static_assert(is_ephemeral_v<EphemeralCore<int>>, "core is ephemeral");
}

int main() {
  REQUIRE_FIXTURES_LOADED(3);
  return test_count == test_passed ? 0 : 1;
}
