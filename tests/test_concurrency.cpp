// Concurrency smoke tests for the three lock policies. Bounded iterations (no
// long sleeps) so this is cheap to run in the default suite. For exhaustive
// race detection, build tests/tsan_stress.cpp under -fsanitize=thread.
#include <lazily/lazily.hpp>

#include <atomic>
#include <thread>
#include <vector>

#include <cassert>

using namespace lazily;

// Each policy must keep a read-modify-write counter consistent under N threads.
template <typename Ctx>
static void run_rmw(const char* /*name*/) {
  Ctx ctx;
  auto counter = ctx.source(0);
  std::vector<std::thread> ts;
  for (int t = 0; t < 4; ++t) {
    ts.emplace_back([&]() {
      for (int i = 0; i < 1000; ++i) {
        ctx.batch([&](Context& c) {
          int v = c.get(counter);
          c.set(counter, v + 1);
        });
      }
    });
  }
  for (auto& th : ts) th.join();
  assert(ctx.get(counter) == 4000);
}

// Concurrent readers must observe a consistent (stable) cached value.
template <typename Ctx>
static void run_reads(const char* /*name*/) {
  Ctx ctx;
  auto cell = ctx.source(42);
  auto slot = ctx.template slot<int>([&](Context& c) { return c.get(cell) * 2; });
  (void)ctx.get(slot);

  std::atomic<int> bad{0};
  std::vector<std::thread> ts;
  for (int t = 0; t < 8; ++t) {
    ts.emplace_back([&]() {
      for (int i = 0; i < 20000; ++i) {
        if (ctx.get(cell) != 42) ++bad;
        if (ctx.get(slot) != 84) ++bad;
      }
    });
  }
  for (auto& th : ts) th.join();
  assert(bad.load() == 0);
}

int main() {
  run_rmw<ThreadSafeContext>("recursive");
  run_rmw<RwThreadSafeContext>("rw");
  run_rmw<ScalableThreadSafeContext>("scalable");

  run_reads<ThreadSafeContext>("recursive");
  run_reads<RwThreadSafeContext>("rw");
  run_reads<ScalableThreadSafeContext>("scalable");
  return 0;
}
