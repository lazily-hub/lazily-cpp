// Cross-language conformance for the rate-shaping source operators
// (`#lzrateshape`) — port of `lazily-rs/tests/rateshape_conformance.rs`.
//
// Each fixture's `initial`, operations, and expectations are parsed from the
// canonical JSON in `lazily-spec/conformance/rateshape/*.json`. Per step we
// assert the emitted value (`returns`), projected `output`, and output-reader
// invalidation via `ctx.is_set` on a wrapping `computed`.

#include <lazily/rateshape.hpp>

#include "test_assertion_keys.hpp"
#include "test_json.hpp"
#include "test_spec_fixture.hpp"
#include <cassert>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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

using OptS = std::optional<std::string>;

static std::string fixture_text(const std::string& file) {
  return lazily_test::spec_fixture_text("rateshape", file);
}

// One canonical step: op fields + the step's assertion block. `is_input`
// distinguishes the `input`/`tick` op type; unused fields (e.g. `now` for count
// sampling, `value` for a tick) are ignored by the per-fixture driver.
//
// The expectation values are deliberately NOT copied out here. Parsing happens
// before the library is driven, so a copied-out expectation could be bound and
// silently never compared; keeping the live `AssertionKeys` means the fixture's
// own value reaches the comparison in `run` and is recorded as asserted there
// (#lzconsumednotasserted).
struct Step {
  bool is_input;
  uint64_t now;
  std::string value;
  double draw;
  OptS returns;                     // expected emit (top-level `returns`)
  lazily_test::AssertionKeys* keys; // this step's `expected` block
};

struct Fixture {
  lazily_test::JsonPtr root;
  std::vector<std::unique_ptr<lazily_test::AssertionKeys>> keys;
  std::vector<Step> steps;
};

static Fixture fixture(const std::string& file) {
  using namespace lazily_test;
  Fixture result;
  result.root = parse_json(fixture_text(file));
  const auto& steps = json_array(json_member(*result.root, "steps"));
  result.steps.reserve(steps.size());
  for (const auto& step_ptr : steps) {
    const auto& step = *step_ptr;
    const auto& op = json_member(step, "op");
    result.keys.push_back(std::make_unique<AssertionKeys>(std::string(__func__) + " expected",
                                                          json_member(step, "expected")));
    const auto type = json_string(json_member(op, "type"));
    // Fail closed (#lzscenariobodyskip). `is_input` below is just
    // `type == "input"`, so before this guard EVERY unrecognised op replayed as
    // a `tick` — the step was booked as replayed while driving something the
    // fixture never named.
    REQUIRE(type == "input" || type == "tick", "unknown rateshape op in fixture: " + type);
    const Json* now = op.find("now");
    const Json* value = op.find("value");
    const Json* draw = op.find("draw");
    result.steps.push_back(Step{
        type == "input",
        now == nullptr ? 0 : json_u64(*now),
        value == nullptr ? std::string{} : json_string(*value),
        draw == nullptr ? 0.0 : json_number(*draw),
        json_optional_string(json_member(step, "returns")),
        result.keys.back().get(),
    });
  }
  return result;
}

// Shared per-step assertion harness: given the emitted value + current output
// for this step, check emit, output, and cache-survival invalidation.
template <typename Drive>
static void run(Context& ctx, const std::vector<Step>& steps, const Computed<OptS>& observed,
                Drive drive) {
  (void)ctx.get(observed);
  for (const auto& step : steps) {
    OptS emitted;
    OptS output;
    drive(step, emitted, output);
    assert(emitted == step.returns && "emit");
    step.keys->assert_key("output", output, lazily_test::json_optional_string);

    const bool was_cached = ctx.is_set(observed);
    (void)ctx.get(observed);
    step.keys->assert_key_with("invalidates", [&](const lazily_test::Json& want) {
      return lazily_test::json_bool(lazily_test::json_member(want, "output")) == !was_cached;
    });
  }
}

// -- Debounce --

TEST(debounce) {
  const auto fx = fixture("debounce.json");
  const auto& initial = lazily_test::json_member(*fx.root, "initial");
  Context ctx;
  const uint64_t quiet = lazily_test::json_u64(lazily_test::json_member(initial, "quiet"));
  DebounceCell<std::string> cell(ctx, quiet);
  auto out = cell.output_cell();
  auto observed = ctx.computed<OptS>([out](Compute& c) { return out.get(c); });

  run(ctx, fx.steps, observed, [&](const Step& s, OptS& emitted, OptS& output) {
    if (s.is_input) {
      cell.input(ctx, s.now, s.value);
      emitted = std::nullopt;
    } else {
      emitted = cell.tick(ctx, s.now);
    }
    output = cell.output(ctx);
  });
}

// -- Throttle --

static void run_throttle(const std::string& file) {
  const auto fx = fixture(file);
  const auto& initial = lazily_test::json_member(*fx.root, "initial");
  const auto edge_name = lazily_test::json_string(lazily_test::json_member(initial, "edge"));
  // Fail closed (#lzscenariobodyskip): a bare `: ThrottleEdge::Trailing` made
  // any unrecognised edge spelling silently configure a trailing throttle.
  REQUIRE(edge_name == "Leading" || edge_name == "Trailing",
          "unknown throttle edge in fixture: " + edge_name);
  const auto edge = edge_name == "Leading" ? ThrottleEdge::Leading : ThrottleEdge::Trailing;
  Context ctx;
  const uint64_t window = lazily_test::json_u64(lazily_test::json_member(initial, "window"));
  ThrottleCell<std::string> cell(ctx, edge, window);
  auto out = cell.output_cell();
  auto observed = ctx.computed<OptS>([out](Compute& c) { return out.get(c); });
  run(ctx, fx.steps, observed, [&](const Step& s, OptS& emitted, OptS& output) {
    if (s.is_input) {
      emitted = cell.input(ctx, s.now, s.value);
    } else {
      emitted = cell.tick(ctx, s.now);
    }
    output = cell.output(ctx);
  });
}

TEST(throttle_leading) { run_throttle("throttle_leading.json"); }

TEST(throttle_trailing) { run_throttle("throttle_trailing.json"); }

// -- Sample (Count) --

TEST(sample_count) {
  const auto fx = fixture("sample_count.json");
  const auto& initial = lazily_test::json_member(*fx.root, "initial");
  Context ctx;
  const uint64_t n = lazily_test::json_u64(lazily_test::json_member(initial, "n"));
  SampleCell<std::string> cell(ctx, SampleMode::Count(n));
  auto out = cell.output_cell();
  auto observed = ctx.computed<OptS>([out](Compute& c) { return out.get(c); });

  run(ctx, fx.steps, observed, [&](const Step& s, OptS& emitted, OptS& output) {
    emitted = cell.input(ctx, s.value);
    output = cell.output(ctx);
  });
}

// -- Sample (Time) --

TEST(sample_time) {
  const auto fx = fixture("sample_time.json");
  const auto& initial = lazily_test::json_member(*fx.root, "initial");
  Context ctx;
  const uint64_t period = lazily_test::json_u64(lazily_test::json_member(initial, "period"));
  SampleCell<std::string> cell(ctx, SampleMode::Time(period));
  auto out = cell.output_cell();
  auto observed = ctx.computed<OptS>([out](Compute& c) { return out.get(c); });

  run(ctx, fx.steps, observed, [&](const Step& s, OptS& emitted, OptS& output) {
    if (s.is_input) {
      cell.input(ctx, s.value);
      emitted = std::nullopt;
    } else {
      emitted = cell.tick(ctx, s.now);
    }
    output = cell.output(ctx);
  });
}

// -- Probabilistic sample --

TEST(probabilistic_sample) {
  const auto fx = fixture("probabilistic_sample.json");
  const auto& initial = lazily_test::json_member(*fx.root, "initial");
  Context ctx;
  const double rate = lazily_test::json_number(lazily_test::json_member(initial, "rate"));
  // Draws are injected per step via `input_with_draw`; the owned RNG is unused,
  // a deterministic `Lcg` satisfies the type bound.
  ProbabilisticSampleCell<std::string, Lcg> cell(ctx, rate, Lcg(0));
  auto out = cell.output_cell();
  auto observed = ctx.computed<OptS>([out](Compute& c) { return out.get(c); });

  (void)ctx.get(observed);
  for (const auto& s : fx.steps) {
    OptS emitted = cell.input_with_draw(ctx, s.value, s.draw);
    OptS output = cell.output(ctx);
    assert(emitted == s.returns && "emit");
    s.keys->assert_key("output", output, lazily_test::json_optional_string);
    const bool was_cached = ctx.is_set(observed);
    (void)ctx.get(observed);
    s.keys->assert_key_with("invalidates", [&](const lazily_test::Json& want) {
      return lazily_test::json_bool(lazily_test::json_member(want, "output")) == !was_cached;
    });
  }

  // Core threshold: strict `<`.
  ProbabilisticSampleCore core(0.5);
  assert(core.decide(0.2));
  assert(!core.decide(0.7));
  assert(!core.decide(0.5));

  // Deterministic Lcg distribution stays near the configured rate.
  Context ctx2;
  ProbabilisticSampleCell<int, Lcg> dist(ctx2, 0.3, Lcg(42));
  int passed = 0;
  const int trials = 20000;
  for (int i = 0; i < trials; ++i)
    if (dist.input(ctx2, i)) ++passed;
  double frac = static_cast<double>(passed) / trials;
  assert(frac > 0.28 && frac < 0.32 && "empirical rate near target");
}

int main() {
  REQUIRE_FIXTURES_LOADED(6);
  return test_count == test_passed ? 0 : 1;
}
