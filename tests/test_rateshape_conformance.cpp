// Cross-language conformance for the rate-shaping source operators
// (`#lzrateshape`) — port of `lazily-rs/tests/rateshape_conformance.rs`.
//
// Each fixture's `initial` + `steps` are transcribed from the vendored JSON in
// `tests/conformance/rateshape/*.json`. Per step we assert: the emitted value
// (`returns`), the projected `output` reader, and that the `output` reader
// invalidates exactly on an emit — observed via `ctx.is_set` on a wrapping
// `computed` (the cache-survival technique). We also assert each fixture's
// `"model"` marker string is present in the vendored text.

#include <lazily/rateshape.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>
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

using OptS = std::optional<std::string>;

static std::string fixture_text(const std::string& file) {
  const auto path = std::filesystem::path(__FILE__).parent_path() /
                    "conformance/rateshape" / file;
  std::ifstream input(path);
  REQUIRE(input, "rateshape conformance fixture missing — a conformance test must not pass without its fixture");
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

static void assert_model(const std::string& file, const std::string& model) {
  const auto text = fixture_text(file);
  assert(text.find("\"model\": \"" + model + "\"") != std::string::npos &&
         "fixture model marker present");
}

// One transcribed step: op fields + expectations. `is_input` distinguishes the
// `input`/`tick` op type; unused fields (e.g. `now` for count sampling, `value`
// for a tick) are ignored by the per-fixture driver.
struct Step {
  bool is_input;
  uint64_t now;
  std::string value;
  double draw;
  OptS returns;       // expected emit (`returns`)
  OptS output;        // expected projected output
  bool invalidates;   // expected output-reader invalidation
};

// Shared per-step assertion harness: given the emitted value + current output
// for this step, check emit, output, and cache-survival invalidation.
template <typename Drive>
static void run(Context& ctx, const std::vector<Step>& steps,
                const SlotHandle<OptS>& observed, Drive drive) {
  (void)ctx.get(observed);
  for (const auto& step : steps) {
    OptS emitted;
    OptS output;
    drive(step, emitted, output);
    assert(emitted == step.returns && "emit");
    assert(output == step.output && "output");

    bool was_cached = ctx.is_set(observed);
    (void)ctx.get(observed);
    assert((!was_cached) == step.invalidates && "invalidation");
  }
}

static Step in(uint64_t now, const char* v, OptS ret, OptS out, bool inval) {
  return Step{true, now, v, 0.0, ret, out, inval};
}
static Step tick(uint64_t now, OptS ret, OptS out, bool inval) {
  return Step{false, now, "", 0.0, ret, out, inval};
}

// -- Debounce --

TEST(debounce) {
  assert_model("debounce.json", "DebounceCell");
  Context ctx;
  const uint64_t quiet = 3;  // initial.quiet
  DebounceCell<std::string> cell(ctx, quiet);
  auto out = cell.output_cell();
  auto observed = ctx.computed<OptS>([out](Context& c) { return out.get(c); });

  std::vector<Step> steps = {
      in(0, "a", std::nullopt, std::nullopt, false),
      in(1, "b", std::nullopt, std::nullopt, false),
      tick(3, std::nullopt, std::nullopt, false),
      tick(4, OptS("b"), OptS("b"), true),
      tick(5, std::nullopt, OptS("b"), false),
      in(6, "c", std::nullopt, OptS("b"), false),
      tick(9, OptS("c"), OptS("c"), true),
  };
  run(ctx, steps, observed, [&](const Step& s, OptS& emitted, OptS& output) {
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

static void run_throttle(const std::string& file, const std::string& model,
                         ThrottleEdge edge, const std::vector<Step>& steps) {
  assert_model(file, model);
  Context ctx;
  const uint64_t window = 5;  // initial.window
  ThrottleCell<std::string> cell(ctx, edge, window);
  auto out = cell.output_cell();
  auto observed = ctx.computed<OptS>([out](Context& c) { return out.get(c); });
  run(ctx, steps, observed, [&](const Step& s, OptS& emitted, OptS& output) {
    if (s.is_input) {
      emitted = cell.input(ctx, s.now, s.value);
    } else {
      emitted = cell.tick(ctx, s.now);
    }
    output = cell.output(ctx);
  });
}

TEST(throttle_leading) {
  run_throttle("throttle_leading.json", "ThrottleCell", ThrottleEdge::Leading,
               {
                   in(0, "a", OptS("a"), OptS("a"), true),
                   in(2, "b", std::nullopt, OptS("a"), false),
                   in(5, "c", OptS("c"), OptS("c"), true),
                   in(6, "d", std::nullopt, OptS("c"), false),
               });
}

TEST(throttle_trailing) {
  run_throttle("throttle_trailing.json", "ThrottleCell", ThrottleEdge::Trailing,
               {
                   in(0, "a", std::nullopt, std::nullopt, false),
                   in(2, "b", std::nullopt, std::nullopt, false),
                   tick(5, OptS("b"), OptS("b"), true),
                   tick(6, std::nullopt, OptS("b"), false),
                   in(7, "c", std::nullopt, OptS("b"), false),
                   tick(12, OptS("c"), OptS("c"), true),
               });
}

// -- Sample (Count) --

TEST(sample_count) {
  assert_model("sample_count.json", "SampleCell");
  Context ctx;
  const uint64_t n = 3;  // initial.n
  SampleCell<std::string> cell(ctx, SampleMode::Count(n));
  auto out = cell.output_cell();
  auto observed = ctx.computed<OptS>([out](Context& c) { return out.get(c); });

  std::vector<Step> steps = {
      in(0, "a", std::nullopt, std::nullopt, false),
      in(0, "b", std::nullopt, std::nullopt, false),
      in(0, "c", OptS("c"), OptS("c"), true),
      in(0, "d", std::nullopt, OptS("c"), false),
      in(0, "e", std::nullopt, OptS("c"), false),
      in(0, "f", OptS("f"), OptS("f"), true),
  };
  run(ctx, steps, observed, [&](const Step& s, OptS& emitted, OptS& output) {
    emitted = cell.input(ctx, s.value);
    output = cell.output(ctx);
  });
}

// -- Sample (Time) --

TEST(sample_time) {
  assert_model("sample_time.json", "SampleCell");
  Context ctx;
  const uint64_t period = 2;  // initial.period
  SampleCell<std::string> cell(ctx, SampleMode::Time(period));
  auto out = cell.output_cell();
  auto observed = ctx.computed<OptS>([out](Context& c) { return out.get(c); });

  std::vector<Step> steps = {
      in(0, "a", std::nullopt, std::nullopt, false),
      in(1, "b", std::nullopt, std::nullopt, false),
      tick(2, OptS("b"), OptS("b"), true),
      in(3, "c", std::nullopt, OptS("b"), false),
      tick(4, OptS("c"), OptS("c"), true),
      tick(5, std::nullopt, OptS("c"), false),
  };
  run(ctx, steps, observed, [&](const Step& s, OptS& emitted, OptS& output) {
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
  assert_model("probabilistic_sample.json", "ProbabilisticSampleCell");
  Context ctx;
  const double rate = 0.5;  // initial.rate
  // Draws are injected per step via `input_with_draw`; the owned RNG is unused,
  // a deterministic `Lcg` satisfies the type bound.
  ProbabilisticSampleCell<std::string, Lcg> cell(ctx, rate, Lcg(0));
  auto out = cell.output_cell();
  auto observed = ctx.computed<OptS>([out](Context& c) { return out.get(c); });

  struct PStep {
    std::string value;
    double draw;
    OptS returns;
    OptS output;
    bool invalidates;
  };
  std::vector<PStep> steps = {
      {"a", 0.2, OptS("a"), OptS("a"), true},
      {"b", 0.7, std::nullopt, OptS("a"), false},
      {"c", 0.5, std::nullopt, OptS("a"), false},
      {"d", 0.49, OptS("d"), OptS("d"), true},
  };
  (void)ctx.get(observed);
  for (const auto& s : steps) {
    OptS emitted = cell.input_with_draw(ctx, s.value, s.draw);
    OptS output = cell.output(ctx);
    assert(emitted == s.returns && "emit");
    assert(output == s.output && "output");
    bool was_cached = ctx.is_set(observed);
    (void)ctx.get(observed);
    assert((!was_cached) == s.invalidates && "invalidation");
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

int main() { return test_count == test_passed ? 0 : 1; }
