// Stream windowing conformance tests (`#lzwindow`).
//
// A C++17 port of `lazily-rs/tests/windowing_conformance.rs`. These are
// **compute** fixtures (`lazily-spec/conformance/windowing/*.json`, vendored
// under `tests/conformance/windowing/`, all using the `Sum` u64 aggregate):
// replay each `push`/`tick`/`flush` op and assert (1) the emit edge
// (`returns`), (2) the projected reader value (`expected.output`), and — the
// core of the spec — that the output reader invalidates exactly on an emit
// (`expected.invalidates.output`). Invalidation is observed by wrapping the
// output cell in a `computed` and checking whether its cached value survives the
// op (`ctx.is_set`).
//
// Each scenario is transcribed directly from its fixture JSON (config + steps);
// the vendored fixture text is also read via `__FILE__` and asserted to carry
// its `"model"` marker so the test stays coupled to the fixture.

#include <lazily/windowing.hpp>
#include <lazily/merge.hpp>

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include "test_require.hpp"

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

static std::string fixture_text(const std::string& file) {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "conformance/windowing" / file;
  std::ifstream input(path);
  REQUIRE(input, "windowing conformance fixture missing — a conformance test must not pass without its fixture");
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

using OptU = std::optional<uint64_t>;

// tumbling_count.json — "model": "TumblingWindow", n=3, Sum aggregate.
TEST(test_tumbling_count) {
  assert(fixture_text("tumbling_count.json").find("\"model\": \"TumblingWindow\"") !=
         std::string::npos);

  Context ctx;
  TumblingCountWindow<uint64_t, Sum> w(ctx, 3);
  auto oc = w.output_cell();
  auto observed = ctx.computed<OptU>([&](Context& c) { return oc.get(c); });
  (void)ctx.get(observed);  // prime the cache

  struct Step {
    uint64_t value;
    OptU emit;
    OptU output;
    bool invalidates;
  };
  const Step steps[] = {
      {1, std::nullopt, std::nullopt, false},
      {2, std::nullopt, std::nullopt, false},
      {3, OptU(6), OptU(6), true},
      {4, std::nullopt, OptU(6), false},
      {5, std::nullopt, OptU(6), false},
      {6, OptU(15), OptU(15), true},
  };

  for (const auto& s : steps) {
    OptU emit = w.push(ctx, s.value);
    assert(emit == s.emit);
    assert(w.output(ctx) == s.output);

    bool was_cached = ctx.is_set(observed);
    (void)ctx.get(observed);
    assert((!was_cached) == s.invalidates);
  }
}

// tumbling_time.json — "model": "TumblingWindow", mode time, period=2, Sum.
TEST(test_tumbling_time) {
  assert(fixture_text("tumbling_time.json").find("\"model\": \"TumblingWindow\"") !=
         std::string::npos);

  Context ctx;
  TumblingTimeWindow<uint64_t, Sum> w(ctx, 2);
  auto oc = w.output_cell();
  auto observed = ctx.computed<OptU>([&](Context& c) { return oc.get(c); });
  (void)ctx.get(observed);

  enum class Kind { Push, Tick };
  struct Step {
    Kind kind;
    uint64_t now;
    uint64_t value;  // ignored for Tick
    OptU emit;
    OptU output;
    bool invalidates;
  };
  const Step steps[] = {
      {Kind::Push, 0, 1, std::nullopt, std::nullopt, false},
      {Kind::Push, 1, 2, std::nullopt, std::nullopt, false},
      {Kind::Tick, 2, 0, OptU(3), OptU(3), true},
      {Kind::Push, 3, 4, std::nullopt, OptU(3), false},
      {Kind::Tick, 4, 0, OptU(4), OptU(4), true},
      {Kind::Tick, 6, 0, std::nullopt, OptU(4), false},
  };

  for (const auto& s : steps) {
    OptU emit;
    if (s.kind == Kind::Push) {
      w.push(ctx, s.now, s.value);
      emit = std::nullopt;
    } else {
      emit = w.tick(ctx, s.now);
    }
    assert(emit == s.emit);
    assert(w.output(ctx) == s.output);

    bool was_cached = ctx.is_set(observed);
    (void)ctx.get(observed);
    assert((!was_cached) == s.invalidates);
  }
}

// sliding_count.json — "model": "SlidingWindow", size=3, slide=1, Sum.
TEST(test_sliding_count) {
  assert(fixture_text("sliding_count.json").find("\"model\": \"SlidingWindow\"") !=
         std::string::npos);

  Context ctx;
  SlidingWindow<uint64_t, Sum> w(ctx, 3, 1);
  auto oc = w.output_cell();
  auto observed = ctx.computed<OptU>([&](Context& c) { return oc.get(c); });
  (void)ctx.get(observed);

  struct Step {
    uint64_t value;
    OptU emit;
    OptU output;
    bool invalidates;
  };
  const Step steps[] = {
      {1, OptU(1), OptU(1), true},
      {2, OptU(3), OptU(3), true},
      {3, OptU(6), OptU(6), true},
      {4, OptU(9), OptU(9), true},
      {5, OptU(12), OptU(12), true},
  };

  for (const auto& s : steps) {
    OptU emit = w.push(ctx, s.value);
    assert(emit == s.emit);
    assert(w.output(ctx) == s.output);

    bool was_cached = ctx.is_set(observed);
    (void)ctx.get(observed);
    assert((!was_cached) == s.invalidates);
  }
}

// session.json — "model": "SessionWindow", gap=3, Sum.
TEST(test_session) {
  assert(fixture_text("session.json").find("\"model\": \"SessionWindow\"") !=
         std::string::npos);

  Context ctx;
  SessionWindow<uint64_t, Sum> w(ctx, 3);
  auto oc = w.output_cell();
  auto observed = ctx.computed<OptU>([&](Context& c) { return oc.get(c); });
  (void)ctx.get(observed);

  enum class Kind { Push, Flush };
  struct Step {
    Kind kind;
    uint64_t now;
    uint64_t value;  // ignored for Flush
    OptU emit;
    OptU output;
    bool invalidates;
  };
  const Step steps[] = {
      {Kind::Push, 0, 1, std::nullopt, std::nullopt, false},
      {Kind::Push, 1, 2, std::nullopt, std::nullopt, false},
      {Kind::Push, 10, 5, OptU(3), OptU(3), true},
      {Kind::Flush, 20, 0, OptU(5), OptU(5), true},
      {Kind::Push, 21, 7, std::nullopt, OptU(5), false},
      {Kind::Flush, 30, 0, OptU(7), OptU(7), true},
  };

  for (const auto& s : steps) {
    OptU emit = s.kind == Kind::Push ? w.push(ctx, s.now, s.value)
                                     : w.flush(ctx, s.now);
    assert(emit == s.emit);
    assert(w.output(ctx) == s.output);

    bool was_cached = ctx.is_set(observed);
    (void)ctx.get(observed);
    assert((!was_cached) == s.invalidates);
  }
}

// Pure-core sanity (mirrors the Rust unit tests in windowing.rs).
TEST(test_pure_cores) {
  {
    TumblingCountCore<uint64_t, Sum> c(3);
    assert(c.push(1) == std::nullopt);
    assert(c.push(2) == std::nullopt);
    assert(c.push(3) == OptU(6));
    assert(c.push(4) == std::nullopt);
    assert(c.push(5) == std::nullopt);
    assert(c.push(6) == OptU(15));
  }
  {
    TumblingTimeCore<uint64_t, Sum> c(2);
    c.push(0, 1);
    c.push(1, 2);
    assert(c.tick(2) == OptU(3));
    c.push(3, 4);
    assert(c.tick(4) == OptU(4));
    assert(c.tick(6) == std::nullopt);  // empty window
  }
  {
    SlidingCore<uint64_t, Sum> c(3, 1);
    assert(c.push(1) == OptU(1));
    assert(c.push(2) == OptU(3));
    assert(c.push(3) == OptU(6));
    assert(c.push(4) == OptU(9));
    assert(c.push(5) == OptU(12));
  }
  {
    SessionCore<uint64_t, Sum> c(3);
    assert(c.push(0, 1) == std::nullopt);
    assert(c.push(1, 2) == std::nullopt);
    assert(c.push(10, 5) == OptU(3));  // gap closes previous
    assert(c.flush(20) == OptU(5));
    assert(c.push(21, 7) == std::nullopt);
    assert(c.flush(30) == OptU(7));
  }
}

int main() { return test_count == test_passed ? 0 : 1; }
