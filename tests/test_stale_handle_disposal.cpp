#include <lazily/cell.hpp>

#include <iostream>
#include <string>

using namespace lazily;

namespace {

int fail(const std::string& message) {
  std::cerr << "stale-handle disposal failure: " << message << '\n';
  return 1;
}

} // namespace

int main() {
  {
    Context ctx;
    const auto stale = ctx.source(1);
    ctx.dispose_cell(stale);
    const auto successor = ctx.source(2);

    if (stale.id() != successor.id()) return fail("source slot was not recycled");
    if (stale.generation() == successor.generation())
      return fail("source generation did not advance");

    ctx.dispose_cell(stale);
    try {
      if (successor.get(ctx) != 2) return fail("source successor value changed");
    } catch (const DisposedError&) {
      return fail("stale source removed its successor");
    }
    successor.set(ctx, 3);
    if (successor.get(ctx) != 3) return fail("stale source removed its successor");
  }

  {
    Context ctx;
    const auto driver = ctx.source(0);
    const auto stale =
        ctx.computed<int>([driver](Compute& compute) { return driver.get(compute) + 1; });
    if (stale.get(ctx) != 1) return fail("stale computed setup value was wrong");
    ctx.dispose_slot(stale);
    const auto successor =
        ctx.computed<int>([driver](Compute& compute) { return driver.get(compute) + 10; });

    if (stale.id() != successor.id()) return fail("computed slot was not recycled");
    if (stale.generation() == successor.generation())
      return fail("computed generation did not advance");

    ctx.dispose_slot(stale);
    try {
      if (successor.get(ctx) != 10) return fail("computed successor value changed");
    } catch (const DisposedError&) {
      return fail("stale computed removed its successor");
    }
    driver.set(ctx, 1);
    if (successor.get(ctx) != 11) return fail("stale computed removed its successor");
  }

  {
    Context ctx;
    const auto driver = ctx.source(0);
    int stale_cleanups = 0;
    int successor_cleanups = 0;
    const Effect stale = ctx.effect([&](Compute& compute) {
      (void)driver.get(compute);
      return CleanupFn([&]() { ++stale_cleanups; });
    });
    ctx.dispose_effect(stale);
    const Effect successor = ctx.effect([&](Compute& compute) {
      (void)driver.get(compute);
      return CleanupFn([&]() { ++successor_cleanups; });
    });

    if (stale.id != successor.id) return fail("effect slot was not recycled");
    if (stale.generation == successor.generation) return fail("effect generation did not advance");
    if (stale_cleanups != 1 || successor_cleanups != 0)
      return fail("effect setup ran an unexpected cleanup");

    ctx.dispose_effect(stale);
    if (!ctx.is_effect_active(successor)) return fail("stale effect removed its successor");
    if (stale_cleanups != 1 || successor_cleanups != 0)
      return fail("stale effect ran the wrong cleanup");

    driver.set(ctx, 1);
    if (successor_cleanups != 1) return fail("live successor did not clean up before rerun");
    ctx.dispose_effect(successor);
    if (successor_cleanups != 2) return fail("live successor cleanup did not run on disposal");
  }

  {
    Context ctx;
    auto scope = ctx.scope();
    const auto stale_member = scope.source(1);
    ctx.dispose_cell(stale_member);
    const auto successor = ctx.source(2);

    if (stale_member.id() != successor.id()) return fail("scope source slot was not recycled");
    scope.end();
    successor.set(ctx, 3);
    if (successor.get(ctx) != 3) return fail("scope teardown removed a recycled successor");
  }

  {
    Context ctx;
    const auto driver = ctx.source(0);
    int successor_cleanups = 0;
    auto scope = ctx.scope();
    const Effect stale_member = scope.effect([&](Compute& compute) {
      (void)driver.get(compute);
      return CleanupFn{};
    });
    ctx.dispose_effect(stale_member);
    const Effect successor = ctx.effect([&](Compute& compute) {
      (void)driver.get(compute);
      return CleanupFn([&]() { ++successor_cleanups; });
    });

    if (stale_member.id != successor.id) return fail("scope effect slot was not recycled");
    scope.end();
    if (!ctx.is_effect_active(successor))
      return fail("scope teardown removed a recycled effect successor");
    if (successor_cleanups != 0)
      return fail("scope teardown ran a recycled effect successor's cleanup");

    ctx.dispose_effect(successor);
    if (successor_cleanups != 1) return fail("successor cleanup did not run exactly once");
  }

  std::cout << "stale-handle disposal: generation, kind, scope, and cleanup checks passed\n";
  return 0;
}
