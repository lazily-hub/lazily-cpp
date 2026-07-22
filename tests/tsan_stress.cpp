#include <lazily/lazily.hpp>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>

using namespace lazily;

// Direct ScalableRwLock mutual-exclusion test under TSan: readers check a
// multi-field payload stays consistent; writers mutate all fields together.
// Any reader↔writer overlap → TSan data-race report + assertion failure.
void test_scalable_lock_mx() {
  ScalableRwLock lock;
  struct Payload {
    uint64_t a, b, c, d;
  };
  Payload payload{1, 1, 1, 1};

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> reads{0}, writes{0};

  std::vector<std::thread> threads;
  for (int t = 0; t < 14; ++t) {
    threads.emplace_back([&, t]() {
      if (t % 2 == 0) {
        while (!stop.load(std::memory_order_relaxed)) {
          lock.lock_shared();
          Payload p = payload;            // read
          assert(p.a == p.b && p.b == p.c && p.c == p.d);  // consistent
          (void)p;
          lock.unlock_shared();
          reads.fetch_add(1, std::memory_order_relaxed);
        }
      } else {
        for (int i = 0; i < 50000; ++i) {
          lock.lock();
          uint64_t v = payload.a + 1;     // mutate all fields together
          payload = {v, v, v, v};
          lock.unlock();
          writes.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  // let readers run alongside writers for a window
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  stop.store(true, std::memory_order_relaxed);
  for (auto& th : threads) th.join();
  printf("OK scalable lock: %llu reads, %llu writes\n",
         (unsigned long long)reads.load(), (unsigned long long)writes.load());
}

// ScalableThreadSafeContext end-to-end under TSan: concurrent get_cell/set_cell.
void test_scalable_context() {
  ScalableThreadSafeContext ctx;
  auto c = ctx.source(7);
  auto s = ctx.slot<int>([&](Context& cc) { return cc.get(c) * 3; });
  (void)ctx.get(s);

  std::atomic<bool> stop{false};
  std::atomic<int> bad{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 16; ++t) {
    threads.emplace_back([&, t]() {
      if (t % 4 == 0) {
        // writers
        for (int i = 0; i < 20000; ++i) ctx.set(c, 7 + (i % 100));
      } else {
        // readers
        while (!stop.load(std::memory_order_relaxed)) {
          int v = ctx.get(c);          // must be a published value
          if (v < 7 || v > 106) ++bad;      // sanity range
        }
      }
    });
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  stop.store(true, std::memory_order_relaxed);
  for (auto& th : threads) th.join();
  printf("OK scalable context: bad=%d\n", bad.load());
  assert(bad.load() == 0);
}

int main() {
  test_scalable_lock_mx();
  test_scalable_context();
  printf("ALL TSAN CHECKS PASSED\n");
  return 0;
}
