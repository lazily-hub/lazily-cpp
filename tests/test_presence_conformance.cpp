// Cross-language conformance for the presence + ephemeral plane
// (`#lzpresence`) — port of `lazily-rs/tests/presence_conformance.rs`.
//
// Steps are transcribed directly from the shared fixtures
// (`lazily-spec/conformance/presence/{presence,awareness,ephemeral}.json`,
// vendored under tests/conformance/presence/). Each step asserts the op's
// projected reader value(s) (`expected.*`) and INVALIDATION (`expected.
// invalidates.*`) via the `computed` + `is_set` cache-survival technique:
// before re-reading the observer slot, `is_set` is true iff the cached value
// survived (no invalidation), so `!was == invalidates`.

#include <lazily/lazily.hpp>
#include <lazily/presence.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
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

using PresenceMap = std::map<uint64_t, std::string>;

static std::string fixture_text(const std::string& name) {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "conformance/presence" / name;
  std::ifstream input(path);
  assert(input && "fixture not found");
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

// -- PresenceCell: heartbeat / evict / TTL tick; live-view invalidation --
TEST(test_presence) {
  const auto fx = fixture_text("presence.json");
  assert(fx.find("\"model\": \"PresenceCell\"") != std::string::npos);

  Context ctx;
  const uint64_t ttl = 5;  // config.ttl
  PresenceCell<uint64_t, std::string> cell(ctx, ttl);
  auto pc = cell.present_cell();
  auto observed =
      ctx.computed<PresenceMap>([pc](Context& c) { return c.get_cell(pc); });
  (void)ctx.get(observed);  // prime the observer cache

  auto step = [&](PresenceMap want, bool invalidates) {
    assert(cell.present(ctx) == want);
    bool was = ctx.is_set(observed);
    (void)ctx.get(observed);
    assert((!was) == invalidates);
  };

  // heartbeat peer 1 "online" now 0
  cell.heartbeat(ctx, 1, "online", 0);
  step({{1, "online"}}, true);
  // heartbeat peer 2 "online" now 1
  cell.heartbeat(ctx, 2, "online", 1);
  step({{1, "online"}, {2, "online"}}, true);
  // heartbeat peer 1 "away" now 3
  cell.heartbeat(ctx, 1, "away", 3);
  step({{1, "away"}, {2, "online"}}, true);
  // evict peer 2 now 4
  cell.evict(ctx, 2, 4);
  step({{1, "away"}}, true);
  // tick now 8 — peer 1 expired at 5
  cell.tick(ctx, 8);
  step({}, true);
}

// -- AwarenessCell: last-writer-per-peer, no merge; TTL tick --
TEST(test_awareness) {
  const auto fx = fixture_text("awareness.json");
  assert(fx.find("\"model\": \"AwarenessCell\"") != std::string::npos);

  Context ctx;
  const uint64_t ttl = 5;  // config.ttl
  AwarenessCell<uint64_t, std::string> cell(ctx, ttl);
  auto pc = cell.present_cell();
  auto observed =
      ctx.computed<PresenceMap>([pc](Context& c) { return c.get_cell(pc); });
  (void)ctx.get(observed);

  auto step = [&](PresenceMap want, bool invalidates) {
    assert(cell.present(ctx) == want);
    bool was = ctx.is_set(observed);
    (void)ctx.get(observed);
    assert((!was) == invalidates);
  };

  // set peer 1 "cursor-a" now 0
  cell.set(ctx, 1, "cursor-a", 0);
  step({{1, "cursor-a"}}, true);
  // set peer 2 "cursor-b" now 1
  cell.set(ctx, 2, "cursor-b", 1);
  step({{1, "cursor-a"}, {2, "cursor-b"}}, true);
  // set peer 1 "cursor-a2" now 2 (overwrite, no merge)
  cell.set(ctx, 1, "cursor-a2", 2);
  step({{1, "cursor-a2"}, {2, "cursor-b"}}, true);
  // tick now 5 — nothing expired yet (expiries 7 and 6)
  cell.tick(ctx, 5);
  step({{1, "cursor-a2"}, {2, "cursor-b"}}, false);
  // tick now 7 — both expired
  cell.tick(ctx, 7);
  step({}, true);

  // last-writer-per-peer visible via non-reactive get
  assert(cell.get(1, 6) == std::nullopt);  // expired by now 7 already ticked
}

// -- EphemeralCell: single value auto-expiry; value invalidation --
TEST(test_ephemeral) {
  const auto fx = fixture_text("ephemeral.json");
  assert(fx.find("\"model\": \"EphemeralCell\"") != std::string::npos);

  Context ctx;
  EphemeralCell<std::string> cell(ctx);
  auto vc = cell.value_cell();
  auto observed = ctx.computed<std::optional<std::string>>(
      [vc](Context& c) { return c.get_cell(vc); });
  (void)ctx.get(observed);

  auto step = [&](std::optional<std::string> want, bool invalidates) {
    assert(cell.value(ctx) == want);
    bool was = ctx.is_set(observed);
    (void)ctx.get(observed);
    assert((!was) == invalidates);
  };

  // set "a" now 0 ttl 5  (expiry 5)
  cell.set(ctx, "a", 0, 5);
  step(std::string("a"), true);
  // tick now 3 — not yet expired
  cell.tick(ctx, 3);
  step(std::string("a"), false);
  // tick now 5 — expired
  cell.tick(ctx, 5);
  step(std::nullopt, true);
  // set "b" now 6 ttl 5 (expiry 11)
  cell.set(ctx, "b", 6, 5);
  step(std::string("b"), true);
  // tick now 10 — not yet expired
  cell.tick(ctx, 10);
  step(std::string("b"), false);
  // set "c" now 10 ttl 5 — overwrite before expiry
  cell.set(ctx, "c", 10, 5);
  step(std::string("c"), true);
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
  e.set("c", 10, 5);  // overwrite before expiry
  assert(e.value() == std::optional<std::string>("c"));

  // EphemeralMapCore presence evict + TTL.
  EphemeralMapCore<uint64_t, std::string> m;
  m.set(1, "online", 0, 5);
  m.set(2, "online", 1, 5);
  m.evict(2);
  assert(m.present(2).size() == 1);
  m.tick(6);  // peer 1 expires at 5
  assert(m.present(6).empty());

  // Awareness last-writer.
  EphemeralMapCore<uint64_t, std::string> a;
  a.set(1, "cursor-a", 0, 5);
  a.set(1, "cursor-a2", 2, 5);
  assert(a.get(1, 2) == std::optional<std::string>("cursor-a2"));

  // Durable sink statically rejects ephemeral values (compile-time).
  durable_persist(42);  // int is durable-compatible
  static_assert(is_ephemeral_v<EphemeralCore<int>>, "core is ephemeral");
}

int main() { return test_count == test_passed ? 0 : 1; }
