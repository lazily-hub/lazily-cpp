// Stream windowing conformance tests (`#lzwindow`).
//
// A C++17 port of `lazily-rs/tests/windowing_conformance.rs`. These are
// **compute** fixtures (`lazily-spec/conformance/windowing/*.json`, read from
// the sibling lazily-spec checkout, all using the `Sum` u64 aggregate):
// replay each `push`/`tick`/`flush` op and assert (1) the emit edge
// (`returns`), (2) the projected reader value (`expected.output`), and — the
// core of the spec — that the output reader invalidates exactly on an emit
// (`expected.invalidates.output`). Invalidation is observed by wrapping the
// output cell in a `computed` and checking whether its cached value survives the
// op (`ctx.is_set`).
//
// Every scenario parses its config, operations, and expectations from the
// canonical fixture.

#include <lazily/windowing.hpp>
#include <lazily/merge.hpp>

#include <cassert>
#include <cstdint>
#include <optional>
#include <string>
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

static lazily_test::JsonPtr fixture(const std::string& file) {
  return lazily_test::parse_json(
      lazily_test::spec_fixture_text("windowing", file));
}

using OptU = std::optional<uint64_t>;

// tumbling_count.json — "model": "TumblingWindow", n=3, Sum aggregate.
TEST(test_tumbling_count) {
  const auto fx = fixture("tumbling_count.json");
  const auto& config = lazily_test::json_member(*fx, "config");
  Context ctx;
  TumblingCountWindow<uint64_t, Sum> w(
      ctx, lazily_test::json_u64(lazily_test::json_member(config, "n")));
  auto oc = w.output_cell();
  auto observed = ctx.computed<OptU>([&](Compute& c) { return oc.get(c); });
  (void)ctx.get(observed);  // prime the cache

  for (const auto& step_ptr :
       lazily_test::json_array(lazily_test::json_member(*fx, "steps"))) {
    const auto& item = *step_ptr;
    const auto& op = lazily_test::json_member(item, "op");
    lazily_test::AssertionKeys expected(
        std::string(__func__) + " expected", lazily_test::json_member(item, "expected"));
    assert(lazily_test::json_string(lazily_test::json_member(op, "type")) == "push");
    const OptU emit =
        w.push(ctx, lazily_test::json_u64(lazily_test::json_member(op, "value")));
    assert(emit == lazily_test::json_optional_u64(
                       lazily_test::json_member(item, "returns")));
    assert(w.output(ctx) == lazily_test::json_optional_u64(
                                lazily_test::json_member(expected, "output")));

    bool was_cached = ctx.is_set(observed);
    (void)ctx.get(observed);
    assert((!was_cached) == lazily_test::json_bool(lazily_test::json_member(
                                lazily_test::json_member(expected, "invalidates"),
                                "output")));
  }
}

// tumbling_time.json — "model": "TumblingWindow", mode time, period=2, Sum.
TEST(test_tumbling_time) {
  const auto fx = fixture("tumbling_time.json");
  const auto& config = lazily_test::json_member(*fx, "config");
  Context ctx;
  TumblingTimeWindow<uint64_t, Sum> w(
      ctx, lazily_test::json_u64(lazily_test::json_member(config, "period")));
  auto oc = w.output_cell();
  auto observed = ctx.computed<OptU>([&](Compute& c) { return oc.get(c); });
  (void)ctx.get(observed);

  for (const auto& step_ptr :
       lazily_test::json_array(lazily_test::json_member(*fx, "steps"))) {
    const auto& item = *step_ptr;
    const auto& op = lazily_test::json_member(item, "op");
    lazily_test::AssertionKeys expected(
        std::string(__func__) + " expected", lazily_test::json_member(item, "expected"));
    const auto type = lazily_test::json_string(lazily_test::json_member(op, "type"));
    OptU emit;
    if (type == "push") {
      w.push(ctx,
             lazily_test::json_u64(lazily_test::json_member(op, "now")),
             lazily_test::json_u64(lazily_test::json_member(op, "value")));
      emit = std::nullopt;
    } else {
      assert(type == "tick");
      emit = w.tick(
          ctx, lazily_test::json_u64(lazily_test::json_member(op, "now")));
    }
    assert(emit == lazily_test::json_optional_u64(
                       lazily_test::json_member(item, "returns")));
    assert(w.output(ctx) == lazily_test::json_optional_u64(
                                lazily_test::json_member(expected, "output")));

    bool was_cached = ctx.is_set(observed);
    (void)ctx.get(observed);
    assert((!was_cached) == lazily_test::json_bool(lazily_test::json_member(
                                lazily_test::json_member(expected, "invalidates"),
                                "output")));
  }
}

// sliding_count.json — "model": "SlidingWindow", size=3, slide=1, Sum.
TEST(test_sliding_count) {
  const auto fx = fixture("sliding_count.json");
  const auto& config = lazily_test::json_member(*fx, "config");
  Context ctx;
  SlidingWindow<uint64_t, Sum> w(
      ctx, lazily_test::json_u64(lazily_test::json_member(config, "size")),
      lazily_test::json_u64(lazily_test::json_member(config, "slide")));
  auto oc = w.output_cell();
  auto observed = ctx.computed<OptU>([&](Compute& c) { return oc.get(c); });
  (void)ctx.get(observed);

  for (const auto& step_ptr :
       lazily_test::json_array(lazily_test::json_member(*fx, "steps"))) {
    const auto& item = *step_ptr;
    const auto& op = lazily_test::json_member(item, "op");
    lazily_test::AssertionKeys expected(
        std::string(__func__) + " expected", lazily_test::json_member(item, "expected"));
    const OptU emit =
        w.push(ctx, lazily_test::json_u64(lazily_test::json_member(op, "value")));
    assert(emit == lazily_test::json_optional_u64(
                       lazily_test::json_member(item, "returns")));
    assert(w.output(ctx) == lazily_test::json_optional_u64(
                                lazily_test::json_member(expected, "output")));

    bool was_cached = ctx.is_set(observed);
    (void)ctx.get(observed);
    assert((!was_cached) == lazily_test::json_bool(lazily_test::json_member(
                                lazily_test::json_member(expected, "invalidates"),
                                "output")));
  }
}

// session.json — "model": "SessionWindow", gap=3, Sum.
TEST(test_session) {
  const auto fx = fixture("session.json");
  const auto& config = lazily_test::json_member(*fx, "config");
  Context ctx;
  SessionWindow<uint64_t, Sum> w(
      ctx, lazily_test::json_u64(lazily_test::json_member(config, "gap")));
  auto oc = w.output_cell();
  auto observed = ctx.computed<OptU>([&](Compute& c) { return oc.get(c); });
  (void)ctx.get(observed);

  for (const auto& step_ptr :
       lazily_test::json_array(lazily_test::json_member(*fx, "steps"))) {
    const auto& item = *step_ptr;
    const auto& op = lazily_test::json_member(item, "op");
    lazily_test::AssertionKeys expected(
        std::string(__func__) + " expected", lazily_test::json_member(item, "expected"));
    const auto type = lazily_test::json_string(lazily_test::json_member(op, "type"));
    const auto now = lazily_test::json_u64(lazily_test::json_member(op, "now"));
    const OptU emit =
        type == "push"
            ? w.push(ctx, now,
                     lazily_test::json_u64(lazily_test::json_member(op, "value")))
            : w.flush(ctx, now);
    assert(type == "push" || type == "flush");
    assert(emit == lazily_test::json_optional_u64(
                       lazily_test::json_member(item, "returns")));
    assert(w.output(ctx) == lazily_test::json_optional_u64(
                                lazily_test::json_member(expected, "output")));

    bool was_cached = ctx.is_set(observed);
    (void)ctx.get(observed);
    assert((!was_cached) == lazily_test::json_bool(lazily_test::json_member(
                                lazily_test::json_member(expected, "invalidates"),
                                "output")));
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

int main() {
  REQUIRE_FIXTURES_LOADED(4); return test_count == test_passed ? 0 : 1; }
