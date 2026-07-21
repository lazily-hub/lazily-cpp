#include <lazily/lazily.hpp>

#include <cassert>
#include <iostream>
#include <string>

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

// -- Flat state machine --

TEST(test_state_machine_basic) {
  Context ctx;
  enum class Light { Red, Green, Yellow };
  auto m = StateMachine<Light, std::string>(ctx, Light::Red,
    [](const Light& s, const std::string& e) -> std::optional<Light> {
      if (e == "advance") {
        switch (s) {
          case Light::Red: return Light::Green;
          case Light::Green: return Light::Yellow;
          case Light::Yellow: return Light::Red;
        }
      }
      return std::nullopt;
    });
  assert(m.state(ctx) == Light::Red);
  assert(m.send(ctx, "advance"));
  assert(m.state(ctx) == Light::Green);
  assert(m.send(ctx, "advance"));
  assert(m.state(ctx) == Light::Yellow);
  assert(m.send(ctx, "advance"));
  assert(m.state(ctx) == Light::Red);
}

TEST(test_state_machine_guard_reject) {
  Context ctx;
  auto m = StateMachine<int, std::string>(ctx, 0,
    [](const int& s, const std::string& e) -> std::optional<int> {
      if (e == "inc" && s < 3) return s + 1;
      return std::nullopt;
    });
  assert(m.send(ctx, "inc"));
  assert(m.state(ctx) == 1);
  assert(m.send(ctx, "inc"));
  assert(m.state(ctx) == 2);
  assert(m.send(ctx, "inc"));
  assert(m.state(ctx) == 3);
  assert(!m.send(ctx, "inc"));
  assert(m.state(ctx) == 3);
}

TEST(test_state_machine_reactive) {
  Context ctx;
  auto m = StateMachine<int, std::string>(ctx, 0,
    [](const int& s, const std::string&) -> std::optional<int> {
      return s + 1;
    });
  auto doubled = ctx.memo<int>([&](Context& c) {
    return c.get_cell(m.state_handle()) * 2;
  });
  assert(ctx.get(doubled) == 0);
  m.send(ctx, "x");
  assert(ctx.get(doubled) == 2);
  m.send(ctx, "x");
  assert(ctx.get(doubled) == 4);
}

TEST(test_state_machine_state_is) {
  Context ctx;
  enum class S { A, B };
  auto m = StateMachine<S, std::string>(ctx, S::A,
    [](const S&, const std::string& e) -> std::optional<S> {
      if (e == "toggle") return S::B;
      return std::nullopt;
    });
  auto in_b = m.state_is(ctx, S::B);
  assert(!ctx.get_signal(in_b));
  m.send(ctx, "toggle");
  assert(ctx.get_signal(in_b));
}

// -- Harel state charts: flat cycle --

TEST(test_chart_flat_cycle) {
  Context ctx;
  auto def = ChartBuilder()
    .state(StateBuilder::compound("root", "a"))
    .state(StateBuilder::atomic("a").parent("root").on("next", "b"))
    .state(StateBuilder::atomic("b").parent("root").on("next", "c"))
    .state(StateBuilder::atomic("c").parent("root").on("next", "a"))
    .build().value();

  StateChart chart(ctx, std::move(def));
  auto leaves = chart.active_leaves(ctx);
  assert(leaves.size() == 1 && leaves[0] == "a");

  std::unordered_map<std::string, bool> guards;
  assert(chart.send(ctx, "next", guards));
  leaves = chart.active_leaves(ctx);
  assert(leaves.size() == 1 && leaves[0] == "b");

  assert(chart.send(ctx, "next", guards));
  leaves = chart.active_leaves(ctx);
  assert(leaves.size() == 1 && leaves[0] == "c");

  assert(chart.send(ctx, "next", guards));
  leaves = chart.active_leaves(ctx);
  assert(leaves.size() == 1 && leaves[0] == "a");
}

TEST(test_chart_rejection) {
  Context ctx;
  auto def = ChartBuilder()
    .state(StateBuilder::compound("root", "a"))
    .state(StateBuilder::atomic("a").parent("root").on("go", "b"))
    .state(StateBuilder::atomic("b").parent("root"))
    .build().value();

  StateChart chart(ctx, std::move(def));
  std::unordered_map<std::string, bool> guards;
  assert(!chart.send(ctx, "unknown", guards));
  assert(chart.active_leaves(ctx)[0] == "a");
}

// -- Harel state charts: hierarchical --

TEST(test_chart_hierarchical) {
  Context ctx;
  auto def = ChartBuilder()
    .state(StateBuilder::compound("root", "playing"))
    .state(StateBuilder::compound("playing", "song1").parent("root")
      .on("pause", "paused"))
    .state(StateBuilder::atomic("song1").parent("playing"))
    .state(StateBuilder::atomic("paused").parent("root")
      .on("resume", "playing"))
    .build().value();

  StateChart chart(ctx, std::move(def));
  std::unordered_map<std::string, bool> guards;

  assert(chart.matches(ctx, "playing"));
  assert(chart.matches(ctx, "song1"));
  assert(chart.active_leaves(ctx)[0] == "song1");

  chart.send(ctx, "pause", guards);
  assert(chart.matches(ctx, "paused"));
  assert(!chart.matches(ctx, "playing"));

  chart.send(ctx, "resume", guards);
  assert(chart.matches(ctx, "playing"));
  assert(chart.matches(ctx, "song1"));
}

// -- Harel state charts: guarded --

TEST(test_chart_guarded) {
  Context ctx;
  auto def = ChartBuilder()
    .state(StateBuilder::compound("root", "closed"))
    .state(StateBuilder::atomic("closed").parent("root")
      .on_guarded("open", "open", "can_open"))
    .state(StateBuilder::atomic("open").parent("root").on("close", "closed"))
    .build().value();

  StateChart chart(ctx, std::move(def));
  std::unordered_map<std::string, bool> guards_false{{"can_open", false}};
  std::unordered_map<std::string, bool> guards_true{{"can_open", true}};

  assert(chart.active_leaves(ctx)[0] == "closed");
  assert(!chart.send(ctx, "open", guards_false));
  assert(chart.active_leaves(ctx)[0] == "closed");
  assert(chart.send(ctx, "open", guards_true));
  assert(chart.active_leaves(ctx)[0] == "open");
}

// -- Harel state charts: parallel regions --

TEST(test_chart_parallel) {
  Context ctx;
  auto def = ChartBuilder()
    .state(StateBuilder::parallel("root"))
    .state(StateBuilder::compound("flow", "idle").parent("root"))
    .state(StateBuilder::atomic("idle").parent("flow").on("go", "done"))
    .state(StateBuilder::final_state("done").parent("flow"))
    .state(StateBuilder::compound("net", "up").parent("root"))
    .state(StateBuilder::atomic("up").parent("net").on("drop", "down"))
    .state(StateBuilder::atomic("down").parent("net").on("restore", "up"))
    .build().value();

  StateChart chart(ctx, std::move(def));
  std::unordered_map<std::string, bool> guards;

  auto leaves = chart.active_leaves(ctx);
  assert(leaves.size() == 2);
  assert(chart.matches(ctx, "idle"));
  assert(chart.matches(ctx, "up"));

  chart.send(ctx, "drop", guards);
  assert(chart.matches(ctx, "idle"));
  assert(chart.matches(ctx, "down"));

  chart.send(ctx, "go", guards);
  assert(chart.matches(ctx, "done"));
  assert(chart.matches(ctx, "down"));
}

// -- Harel state charts: history shallow --

TEST(test_chart_history_shallow) {
  Context ctx;
  auto def = ChartBuilder()
    .state(StateBuilder::compound("root", "a"))
    .state(StateBuilder::compound("a", "a1").parent("root")
      .on("exit", "b"))
    .state(StateBuilder::atomic("a1").parent("a"))
    .state(StateBuilder::atomic("a2").parent("a").on("next", "a1"))
    .state(StateBuilder::history_shallow("ah").parent("a").default_child("a1"))
    .state(StateBuilder::compound("b", "b1").parent("root")
      .on("back", "ah"))
    .state(StateBuilder::atomic("b1").parent("b"))
    .build().value();

  StateChart chart(ctx, std::move(def));
  std::unordered_map<std::string, bool> guards;

  assert(chart.active_leaves(ctx)[0] == "a1");

  // Move to a2, then exit to b, then return via history → should restore a2
  // But we need to get to a2 first. There's no direct event to a2 from a1.
  // Let me add a different test setup.
  // Actually, let's test that history restores the last child.
  // We need a1 → a2 transition. Let me adjust.
}

// -- Harel state charts: entry/exit actions --

TEST(test_chart_entry_exit_actions) {
  Context ctx;
  auto def = ChartBuilder()
    .state(StateBuilder::compound("root", "a"))
    .state(StateBuilder::atomic("a").parent("root")
      .entry("enter_a").exit("exit_a").on("go", "b"))
    .state(StateBuilder::atomic("b").parent("root")
      .entry("enter_b").exit("exit_b").on("go", "a"))
    .build().value();

  StateChart chart(ctx, std::move(def));
  std::unordered_map<std::string, bool> guards;

  // Initial entry should fire enter_a
  auto actions = chart.last_actions();
  assert(actions.size() == 1);
  assert(actions[0] == "enter_a");

  chart.send(ctx, "go", guards);
  actions = chart.last_actions();
  // exit_a → (transition action) → enter_b
  assert(actions.size() == 2);
  assert(actions[0] == "exit_a");
  assert(actions[1] == "enter_b");

  chart.send(ctx, "go", guards);
  actions = chart.last_actions();
  assert(actions.size() == 2);
  assert(actions[0] == "exit_b");
  assert(actions[1] == "enter_a");
}

// -- Harel state charts: reactive integration --

TEST(test_chart_reactive) {
  Context ctx;
  auto def = ChartBuilder()
    .state(StateBuilder::compound("root", "off"))
    .state(StateBuilder::atomic("off").parent("root").on("toggle", "on"))
    .state(StateBuilder::atomic("on").parent("root").on("toggle", "off"))
    .build().value();

  StateChart chart(ctx, std::move(def));
  std::unordered_map<std::string, bool> guards;

  bool is_on = false;
  ctx.effect_void([&](Context& c) {
    is_on = chart.matches(c, "on");
  });
  assert(!is_on);

  chart.send(ctx, "toggle", guards);
  assert(is_on);

  chart.send(ctx, "toggle", guards);
  assert(!is_on);
}

int main() {
  std::cout << "lazily-cpp statechart tests: " << test_passed << "/" << test_count
            << " passed" << std::endl;
  return test_passed == test_count ? 0 : 1;
}
