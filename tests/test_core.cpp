#include <lazily/lazily.hpp>

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

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

TEST(test_cell_basic) {
  Context ctx;
  auto c = ctx.cell(42);
  assert(ctx.get_cell(c) == 42);
  ctx.set_cell(c, 100);
  assert(ctx.get_cell(c) == 100);
}

TEST(test_slot_lazy) {
  Context ctx;
  auto a = ctx.cell(2);
  auto b = ctx.cell(3);
  auto sum = ctx.computed<int>([&](Context& c) {
    return c.get_cell(a) + c.get_cell(b);
  });
  assert(ctx.get(sum) == 5);
  ctx.set_cell(a, 10);
  assert(ctx.get(sum) == 13);
  ctx.set_cell(b, 7);
  assert(ctx.get(sum) == 17);
}

TEST(test_slot_caching) {
  Context ctx;
  int compute_count = 0;
  auto a = ctx.cell(1);
  auto s = ctx.computed<int>([&](Context& c) {
    compute_count++;
    return c.get_cell(a) * 2;
  });
  assert(ctx.get(s) == 2);
  assert(ctx.get(s) == 2);
  assert(compute_count == 1);
  ctx.set_cell(a, 5);
  assert(ctx.get(s) == 10);
  assert(compute_count == 2);
  ctx.set_cell(a, 5);
  assert(ctx.get(s) == 10);
  assert(compute_count == 2);
}

TEST(test_partial_eq_guard) {
  Context ctx;
  int effect_runs = 0;
  auto a = ctx.cell(1);
  ctx.effect_void([&](Context& c) {
    effect_runs++;
    c.get_cell(a);
  });
  assert(effect_runs == 1);
  ctx.set_cell(a, 1);
  assert(effect_runs == 1);
  ctx.set_cell(a, 2);
  assert(effect_runs == 2);
}

TEST(test_memo_equality_guard) {
  Context ctx;
  int compute_count = 0;
  int downstream_count = 0;
  auto a = ctx.cell(2);
  auto m = ctx.memo<int>([&](Context& c) {
    compute_count++;
    return c.get_cell(a) / 2;
  });
  auto downstream = ctx.computed<int>([&](Context& c) {
    downstream_count++;
    return c.get(m) + 1;
  });
  assert(ctx.get(downstream) == 2);
  assert(compute_count == 1);
  assert(downstream_count == 1);
  // 2 -> 3: 3/2 = 1 (unchanged from 2/2=1) — memo suppresses downstream
  ctx.set_cell(a, 3);
  assert(ctx.get(downstream) == 2);
  assert(compute_count == 2);
  assert(downstream_count == 1);
}

TEST(test_batch_coalesce) {
  Context ctx;
  int compute_count = 0;
  auto a = ctx.cell(1);
  auto b = ctx.cell(2);
  auto sum = ctx.computed<int>([&](Context& c) {
    compute_count++;
    return c.get_cell(a) + c.get_cell(b);
  });
  assert(ctx.get(sum) == 3);
  assert(compute_count == 1);
  ctx.batch([&](Context& c) {
    c.set_cell(a, 10);
    c.set_cell(b, 20);
  });
  assert(ctx.get(sum) == 30);
  assert(compute_count == 2);
}

TEST(test_signal_eager) {
  Context ctx;
  auto a = ctx.cell(1);
  auto b = ctx.cell(2);
  auto sig = ctx.signal<int>([&](Context& c) {
    return c.get_cell(a) + c.get_cell(b);
  });
  assert(ctx.get_signal(sig) == 3);
  ctx.set_cell(a, 10);
  assert(ctx.get_signal(sig) == 12);
}

TEST(test_effect_rerun) {
  Context ctx;
  std::vector<int> log;
  auto a = ctx.cell(0);
  ctx.effect_void([&](Context& c) {
    log.push_back(c.get_cell(a));
  });
  assert(log.size() == 1 && log[0] == 0);
  ctx.set_cell(a, 1);
  assert(log.size() == 2 && log[1] == 1);
  ctx.set_cell(a, 2);
  assert(log.size() == 3 && log[2] == 2);
}

TEST(test_effect_cleanup) {
  Context ctx;
  int cleanup_count = 0;
  auto a = ctx.cell(1);
  auto eff = ctx.effect([&](Context& c) -> CleanupFn {
    c.get_cell(a);
    return [&]() { cleanup_count++; };
  });
  assert(cleanup_count == 0);
  ctx.set_cell(a, 2);
  assert(cleanup_count == 1);
  eff.dispose(ctx);
  assert(cleanup_count == 2);
}

TEST(test_diamond_dependencies) {
  Context ctx;
  auto a = ctx.cell(1);
  auto b = ctx.computed<int>([&](Context& c) { return c.get_cell(a) + 1; });
  auto c2 = ctx.computed<int>([&](Context& c) { return c.get_cell(a) + 2; });
  auto d = ctx.computed<int>([&](Context& c) { return c.get(b) + c.get(c2); });
  assert(ctx.get(d) == (1 + 1) + (1 + 2));
  ctx.set_cell(a, 10);
  assert(ctx.get(d) == (10 + 1) + (10 + 2));
}

TEST(test_clear_slot) {
  Context ctx;
  int compute_count = 0;
  auto a = ctx.cell(5);
  auto s = ctx.computed<int>([&](Context& c) {
    compute_count++;
    return c.get_cell(a) * 2;
  });
  assert(ctx.get(s) == 10);
  assert(compute_count == 1);
  assert(ctx.is_set(s));
  s.clear(ctx);
  assert(!ctx.is_set(s));
  assert(ctx.get(s) == 10);
  assert(compute_count == 2);
}

TEST(test_dynamic_dependencies) {
  Context ctx;
  auto flag = ctx.cell(true);
  auto a = ctx.cell(1);
  auto b = ctx.cell(100);
  auto s = ctx.computed<int>([&](Context& c) {
    if (c.get_cell(flag))
      return c.get_cell(a);
    else
      return c.get_cell(b);
  });
  assert(ctx.get(s) == 1);
  ctx.set_cell(flag, false);
  assert(ctx.get(s) == 100);
  ctx.set_cell(a, 999);
  assert(ctx.get(s) == 100);
}

TEST(test_get_rc_avoids_clone) {
  Context ctx;
  auto s = ctx.computed<std::string>([&](Context& c) {
    return std::string("hello world");
  });
  auto rc = ctx.get_rc<std::string>(s);
  assert(*rc == "hello world");
}

TEST(test_signal_dispose) {
  Context ctx;
  auto a = ctx.cell(1);
  auto sig = ctx.signal<int>([&](Context& c) {
    return c.get_cell(a) * 10;
  });
  assert(ctx.is_signal_active(sig));
  assert(ctx.get_signal(sig) == 10);
  sig.dispose(ctx);
  assert(!ctx.is_signal_active(sig));
}

TEST(test_effect_dispose) {
  Context ctx;
  int run_count = 0;
  auto a = ctx.cell(1);
  auto eff = ctx.effect_void([&](Context& c) {
    run_count++;
    c.get_cell(a);
  });
  assert(run_count == 1);
  assert(eff.is_active(ctx));
  ctx.set_cell(a, 2);
  assert(run_count == 2);
  eff.dispose(ctx);
  assert(!eff.is_active(ctx));
  ctx.set_cell(a, 3);
  assert(run_count == 2);
}

TEST(test_nested_batch) {
  Context ctx;
  int compute_count = 0;
  auto a = ctx.cell(1);
  auto b = ctx.cell(2);
  auto sum = ctx.computed<int>([&](Context& c) {
    compute_count++;
    return c.get_cell(a) + c.get_cell(b);
  });
  ctx.get(sum);
  assert(compute_count == 1);
  ctx.batch([&](Context& c) {
    c.set_cell(a, 10);
    c.batch([&](Context& c2) {
      c2.set_cell(b, 20);
    });
    assert(compute_count == 1);
  });
  assert(ctx.get(sum) == 30);
  assert(compute_count == 2);
}

TEST(test_string_values) {
  Context ctx;
  auto name = ctx.cell<std::string>("alice");
  auto greeting = ctx.computed<std::string>([&](Context& c) {
    return "Hello, " + c.get_cell(name) + "!";
  });
  assert(ctx.get(greeting) == "Hello, alice!");
  ctx.set_cell(name, std::string("bob"));
  assert(ctx.get(greeting) == "Hello, bob!");
}

// SmallAny (optimization B): inline fast path for trivially-copyable values,
// heap fallback for larger/non-trivial ones; move + reset semantics.
TEST(test_small_any) {
  SmallAny<> a = SmallAny<>::make<int>(7);          // inline (int <= 16B, trivial)
  assert(static_cast<bool>(a));
  assert(*a.as<int>() == 7);
  SmallAny<> b = SmallAny<>::make<int>(123);
  a = std::move(b);                                  // move-assign (inline relocate)
  assert(*a.as<int>() == 123);
  assert(!static_cast<bool>(b));                     // moved-from is empty
  SmallAny<> c(std::move(a));                        // move-ctor
  assert(*c.as<int>() == 123);

  SmallAny<> s = SmallAny<>::make<std::string>("hello world");  // heap (>16B)
  assert(*s.as<std::string>() == "hello world");
  SmallAny<> s2 = std::move(s);                      // move heap (pointer relocate)
  assert(*s2.as<std::string>() == "hello world");
  s2.reset();
  assert(!static_cast<bool>(s2));

  // Larger-than-buffer trivially-copyable also goes heap (size > BufSize).
  struct BigPod {
    int xs[8];
  };
  static_assert(sizeof(BigPod) > 16, "should exceed the inline buffer");
  SmallAny<> fit = SmallAny<>::make<BigPod>(BigPod{{1, 2, 3, 4, 5, 6, 7, 8}});
  assert(fit.as<BigPod>()->xs[7] == 8);
}

int main() {
  std::cout << "lazily-cpp core tests: " << test_passed << "/" << test_count
            << " passed" << std::endl;
  return test_passed == test_count ? 0 : 1;
}
