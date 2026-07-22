// Embedded-service plane conformance (`#lzservice`).
//
// Mirrors the Rust reference test `lazily-rs/tests/service_conformance.rs` and
// replays the shared lazily-spec fixtures in `conformance/service/*.json`:
//   - health.json           ("model": "HealthCell")
//   - readiness.json        ("model": "ReadinessCell")
//   - discovery.json        ("model": "DiscoveryCell")
//   - service_registry.json ("model": "ServiceRegistry")
//
// Each fixture's steps are transcribed into C++ below. Per step we assert the
// op result + projected reader value (`expected.*`) and the reader INVALIDATION
// (`expected.invalidates`) via a `computed` slot + `is_set` cache-survival
// probe: after an op, a real projection change marks the observing slot dirty
// (is_set == false) while a deduped no-op leaves it cached (is_set == true).

#include <lazily/lazily.hpp>
#include <lazily/service.hpp>

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>
#include "test_spec_fixture.hpp"

using namespace lazily;

static int test_count = 0;
static int test_passed = 0;

#define TEST(name)                                        \
  static void name();                                     \
  struct name##_runner {                                  \
    name##_runner() {                                     \
      ++test_count;                                       \
      name();                                             \
      ++test_passed;                                      \
    }                                                     \
  } name##_instance;                                      \
  static void name()

static std::string fixture_text(const std::string& file) {
  return lazily_test::spec_fixture_text("service", file);
}

// After an op, `observed` is dirty (is_set == false) iff the projection changed.
template <typename T>
static void check_inval(Context& ctx, const Computed<T>& observed,
                        bool expect_inval, const char* msg) {
  bool was = ctx.is_set(observed);
  (void)ctx.get(observed);
  assert(((!was) == expect_inval) && msg);
}

using Map = std::map<std::string, std::string>;

// -- health.json --
TEST(test_health) {
  const auto fx = fixture_text("health.json");
  assert(fx.find("\"model\": \"HealthCell\"") != std::string::npos);

  Context ctx;
  HealthCell h(ctx);
  auto hc = h.health_cell();
  auto observed = ctx.computed<Health>([hc](Compute& c) { return c.get(hc); });
  (void)ctx.get(observed);

  struct Step {
    const char* name;
    bool up;
    bool critical;
    Health want;
    bool inval;
  };
  const std::vector<Step> steps = {
      {"cache", true, false, Health::Healthy, false},
      {"cache", false, false, Health::Degraded, true},
      {"db", false, true, Health::Unhealthy, true},
      {"db", true, true, Health::Degraded, true},
      {"cache", true, false, Health::Healthy, true},
  };
  for (const auto& s : steps) {
    h.set(ctx, s.name, s.up, s.critical);
    assert(h.health() == s.want);
    check_inval(ctx, observed, s.inval, "health inval");
  }
}

// -- readiness.json --
TEST(test_readiness) {
  const auto fx = fixture_text("readiness.json");
  assert(fx.find("\"model\": \"ReadinessCell\"") != std::string::npos);

  Context ctx;
  ReadinessCell r(ctx);
  auto rc = r.ready_cell();
  auto observed = ctx.computed<bool>([rc](Compute& c) { return c.get(rc); });
  (void)ctx.get(observed);

  struct Step {
    const char* name;
    bool ready;
    bool want;
    bool inval;
  };
  const std::vector<Step> steps = {
      {"deps_ready", false, false, true},
      {"leader_known", false, false, false},
      {"deps_ready", true, false, false},
      {"leader_known", true, true, true},
      {"lease_valid", false, false, true},
  };
  for (const auto& s : steps) {
    r.set(ctx, s.name, s.ready);
    assert(r.ready() == s.want);
    check_inval(ctx, observed, s.inval, "ready inval");
  }
}

// -- discovery.json --
TEST(test_discovery) {
  const auto fx = fixture_text("discovery.json");
  assert(fx.find("\"model\": \"DiscoveryCell\"") != std::string::npos);

  Context ctx;
  DiscoveryCell<uint64_t> d(ctx);
  auto dc = d.discovery_cell();
  auto observed = ctx.computed<Map>([dc](Compute& c) { return c.get(dc); });
  (void)ctx.get(observed);

  // step 1: register api -> {api}
  d.register_(ctx, "api", "10.0.0.1", 1);
  assert(d.discovery(ctx) == (Map{{"api", "10.0.0.1"}}));
  check_inval(ctx, observed, true, "discovery inval s1");

  // step 2: register db -> {api, db}
  d.register_(ctx, "db", "10.0.0.2", 2);
  assert(d.discovery(ctx) == (Map{{"api", "10.0.0.1"}, {"db", "10.0.0.2"}}));
  check_inval(ctx, observed, true, "discovery inval s2");

  // step 3: resolve api -> read-only, map unchanged
  assert(d.resolve("api") == std::optional<std::string>("10.0.0.1"));
  assert(d.discovery(ctx) == (Map{{"api", "10.0.0.1"}, {"db", "10.0.0.2"}}));
  check_inval(ctx, observed, false, "discovery inval s3");

  // step 4: evict peer 2 -> {api}
  d.evict(ctx, 2);
  assert(d.discovery(ctx) == (Map{{"api", "10.0.0.1"}}));
  check_inval(ctx, observed, true, "discovery inval s4");

  // step 5: deregister api -> {}
  d.deregister(ctx, "api");
  assert(d.discovery(ctx) == Map{});
  check_inval(ctx, observed, true, "discovery inval s5");
}

// -- service_registry.json --
TEST(test_service_registry) {
  const auto fx = fixture_text("service_registry.json");
  assert(fx.find("\"model\": \"ServiceRegistry\"") != std::string::npos);

  Context ctx;
  ServiceRegistry reg(ctx);
  auto pc = reg.projection_cell();
  auto observed = ctx.computed<Map>([pc](Compute& c) { return c.get(pc); });
  (void)ctx.get(observed);

  // step 1: register api v1
  reg.register_(ctx, "api", "v1");
  assert(reg.projection(ctx) == (Map{{"api", "v1"}}));
  check_inval(ctx, observed, true, "projection inval s1");

  // step 2: register db v1
  reg.register_(ctx, "db", "v1");
  assert(reg.projection(ctx) == (Map{{"api", "v1"}, {"db", "v1"}}));
  check_inval(ctx, observed, true, "projection inval s2");

  // step 3: register api v2 (overwrite)
  reg.register_(ctx, "api", "v2");
  assert(reg.projection(ctx) == (Map{{"api", "v2"}, {"db", "v1"}}));
  check_inval(ctx, observed, true, "projection inval s3");

  // step 4: deregister db
  reg.deregister(ctx, "db");
  assert(reg.projection(ctx) == (Map{{"api", "v2"}}));
  check_inval(ctx, observed, true, "projection inval s4");

  // step 5: replay rebuilds identical projection -> no change, no invalidation
  reg.replay(ctx);
  assert(reg.projection(ctx) == (Map{{"api", "v2"}}));
  check_inval(ctx, observed, false, "projection inval s5");
}

int main() {
  REQUIRE_FIXTURES_LOADED(4); return test_count == test_passed ? 0 : 1; }
