// Embedded-service plane conformance (`#lzservice`).
//
// Mirrors the Rust reference test `lazily-rs/tests/service_conformance.rs` and
// replays the shared lazily-spec fixtures in `conformance/service/*.json`:
//   - health.json           ("model": "HealthCell")
//   - readiness.json        ("model": "ReadinessCell")
//   - discovery.json        ("model": "DiscoveryCell")
//   - service_registry.json ("model": "ServiceRegistry")
//
// Each fixture's operations and expectations are parsed directly. Per step we
// assert the op result + projected reader value (`expected.*`) and INVALIDATION
// (`expected.invalidates`) via a `computed` slot + `is_set` cache-survival
// probe: after an op, a real projection change marks the observing slot dirty
// (is_set == false) while a deduped no-op leaves it cached (is_set == true).

#include <lazily/lazily.hpp>
#include <lazily/service.hpp>

#include "test_assertion_keys.hpp"
#include "test_json.hpp"
#include "test_spec_fixture.hpp"
#include <cassert>
#include <cstdint>
#include <map>
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

static lazily_test::JsonPtr fixture(const std::string& file) {
  return lazily_test::parse_json(lazily_test::spec_fixture_text("service", file));
}

// After an op, `observed` is dirty (is_set == false) iff the projection
// changed. The claim is asserted against `expected.invalidates.<field>` so the
// fixture's own value reaches the comparison (#lzconsumednotasserted).
template <typename T>
static void assert_inval(Context& ctx, const Computed<T>& observed,
                         lazily_test::AssertionKeys& expected, const std::string& field) {
  const bool was = ctx.is_set(observed);
  (void)ctx.get(observed);
  expected.assert_key_with("invalidates", [&](const lazily_test::Json& want) {
    return lazily_test::json_bool(lazily_test::json_member(want, field)) == !was;
  });
}

using Map = std::map<std::string, std::string>;

static Map json_map(const lazily_test::Json& value) {
  assert(value.is_object());
  Map result;
  for (const auto& entry : value.object) {
    result.emplace(entry.first, lazily_test::json_string(*entry.second));
  }
  return result;
}

// -- health.json --
TEST(test_health) {
  const auto fx = fixture("health.json");
  Context ctx;
  HealthCell h(ctx);
  auto hc = h.health_cell();
  auto observed = ctx.computed<Health>([hc](Compute& c) { return c.get(hc); });
  (void)ctx.get(observed);

  for (const auto& step_ptr : lazily_test::json_array(lazily_test::json_member(*fx, "steps"))) {
    const auto& item = *step_ptr;
    const auto& op = lazily_test::json_member(item, "op");
    lazily_test::AssertionKeys expected(std::string(__func__) + " expected",
                                        lazily_test::json_member(item, "expected"));
    h.set(ctx, lazily_test::json_string(lazily_test::json_member(op, "name")),
          lazily_test::json_bool(lazily_test::json_member(op, "up")),
          lazily_test::json_bool(lazily_test::json_member(op, "critical")));
    expected.assert_key_with("health", [&](const lazily_test::Json& value) {
      const auto health = lazily_test::json_string(value);
      // Fail closed (#lzscenariobodyskip). This ended in a bare
      // `: Health::Unhealthy`, so any unrecognised spelling silently became the
      // Unhealthy expectation instead of failing.
      Health want = Health::Unhealthy;
      if (health == "Healthy")
        want = Health::Healthy;
      else if (health == "Degraded")
        want = Health::Degraded;
      else
        REQUIRE(health == "Unhealthy", "unknown health state in fixture: " + health);
      return h.health() == want;
    });
    assert_inval(ctx, observed, expected, "health");
  }
}

// -- readiness.json --
TEST(test_readiness) {
  const auto fx = fixture("readiness.json");
  Context ctx;
  ReadinessCell r(ctx);
  auto rc = r.ready_cell();
  auto observed = ctx.computed<bool>([rc](Compute& c) { return c.get(rc); });
  (void)ctx.get(observed);

  for (const auto& step_ptr : lazily_test::json_array(lazily_test::json_member(*fx, "steps"))) {
    const auto& item = *step_ptr;
    const auto& op = lazily_test::json_member(item, "op");
    lazily_test::AssertionKeys expected(std::string(__func__) + " expected",
                                        lazily_test::json_member(item, "expected"));
    r.set(ctx, lazily_test::json_string(lazily_test::json_member(op, "name")),
          lazily_test::json_bool(lazily_test::json_member(op, "ready")));
    expected.assert_key("ready", r.ready());
    assert_inval(ctx, observed, expected, "ready");
  }
}

// -- discovery.json --
TEST(test_discovery) {
  const auto fx = fixture("discovery.json");
  Context ctx;
  DiscoveryCell<uint64_t> d(ctx);
  auto dc = d.discovery_cell();
  auto observed = ctx.computed<Map>([dc](Compute& c) { return c.get(dc); });
  (void)ctx.get(observed);

  for (const auto& step_ptr : lazily_test::json_array(lazily_test::json_member(*fx, "steps"))) {
    const auto& item = *step_ptr;
    const auto& op = lazily_test::json_member(item, "op");
    lazily_test::AssertionKeys expected(std::string(__func__) + " expected",
                                        lazily_test::json_member(item, "expected"));
    const auto type = lazily_test::json_string(lazily_test::json_member(op, "type"));
    if (type == "register") {
      d.register_(ctx, lazily_test::json_string(lazily_test::json_member(op, "service")),
                  lazily_test::json_string(lazily_test::json_member(op, "endpoint")),
                  lazily_test::json_u64(lazily_test::json_member(op, "peer")));
    } else if (type == "resolve") {
      assert(d.resolve(lazily_test::json_string(lazily_test::json_member(op, "service"))) ==
             lazily_test::json_optional_string(lazily_test::json_member(item, "returns")));
    } else if (type == "evict") {
      d.evict(ctx, lazily_test::json_u64(lazily_test::json_member(op, "peer")));
    } else if (type == "deregister") {
      d.deregister(ctx, lazily_test::json_string(lazily_test::json_member(op, "service")));
    } else {
      // Fail closed (#lzscenariobodyskip). An unnamed op must not replay as the
      // last arm — the ledger books the step either way.
      REQUIRE(false, "unknown discovery op in fixture: " + type);
    }
    expected.assert_key("discovery", d.discovery(ctx), json_map);
    assert_inval(ctx, observed, expected, "discovery");
  }
}

// -- service_registry.json --
TEST(test_service_registry) {
  const auto fx = fixture("service_registry.json");
  Context ctx;
  ServiceRegistry reg(ctx);
  auto pc = reg.projection_cell();
  auto observed = ctx.computed<Map>([pc](Compute& c) { return c.get(pc); });
  (void)ctx.get(observed);

  for (const auto& step_ptr : lazily_test::json_array(lazily_test::json_member(*fx, "steps"))) {
    const auto& item = *step_ptr;
    const auto& op = lazily_test::json_member(item, "op");
    lazily_test::AssertionKeys expected(std::string(__func__) + " expected",
                                        lazily_test::json_member(item, "expected"));
    const auto type = lazily_test::json_string(lazily_test::json_member(op, "type"));
    if (type == "register") {
      reg.register_(ctx, lazily_test::json_string(lazily_test::json_member(op, "service")),
                    lazily_test::json_string(lazily_test::json_member(op, "endpoint")));
    } else if (type == "deregister") {
      reg.deregister(ctx, lazily_test::json_string(lazily_test::json_member(op, "service")));
    } else if (type == "replay") {
      reg.replay(ctx);
    } else {
      // Fail closed (#lzscenariobodyskip).
      REQUIRE(false, "unknown registry op in fixture: " + type);
    }
    expected.assert_key("projection", reg.projection(ctx), json_map);
    assert_inval(ctx, observed, expected, "projection");
  }
}

int main() {
  REQUIRE_FIXTURES_LOADED(4);
  return test_count == test_passed ? 0 : 1;
}
