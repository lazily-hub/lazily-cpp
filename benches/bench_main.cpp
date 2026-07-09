#include <lazily/lazily.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace lazily;
using clk = std::chrono::high_resolution_clock;

struct BenchResult {
  std::string group;
  std::string case_name;
  double mean_ns;
  int samples;
};

static std::vector<BenchResult> results;

template <typename F>
void bench(const std::string& group, const std::string& case_name,
           int iterations, F&& body) {
  // Warmup
  for (int i = 0; i < std::min(100, iterations / 10); ++i) body();

  std::vector<double> times;
  times.reserve(iterations);
  for (int i = 0; i < iterations; ++i) {
    auto start = clk::now();
    body();
    auto end = clk::now();
    times.push_back(std::chrono::duration<double, std::nano>(end - start).count());
  }
  double sum = 0;
  for (auto t : times) sum += t;
  double mean = sum / times.size();
  results.push_back({group, case_name, mean, iterations});
}

void print_results() {
  std::cout << "\n| Group | Case | Mean | Samples |\n";
  std::cout << "|---|---|---:|---:|\n";
  for (auto& r : results) {
    double val = r.mean_ns;
    std::string unit = "ns";
    if (val >= 1e9) { val /= 1e9; unit = "s"; }
    else if (val >= 1e6) { val /= 1e6; unit = "ms"; }
    else if (val >= 1e3) { val /= 1e3; unit = "us"; }
    std::cout << "| " << r.group << " | " << r.case_name << " | "
              << std::fixed << std::setprecision(3) << val << " " << unit
              << " | " << r.samples << " |\n";
  }
}

// -- Cached reads --
void bench_cached_reads() {
  Context ctx;
  auto c = ctx.cell(42);
  auto s = ctx.computed<int>([c](Context& ctx) { return ctx.get_cell(c) * 2; });
  ctx.get(s);  // prime cache

  bench("cached_reads", "context", 1000000, [&]() {
    (void)ctx.get(s);
  });

  ThreadSafeContext ts_ctx;
  auto tc = ts_ctx.cell(42);
  auto ts = ts_ctx.slot<int>([tc](Context& c) { return c.get_cell(tc) * 2; });
  ts_ctx.get(ts);  // prime cache

  bench("cached_reads", "thread_safe_context", 1000000, [&]() {
    (void)ts_ctx.get(ts);
  });
}

// -- Cold first get --
void bench_cold_first_get() {
  bench("cold_first_get", "context", 100000, [&]() {
    Context ctx;
    auto c = ctx.cell(42);
    auto s = ctx.computed<int>([c](Context& ctx) { return ctx.get_cell(c) * 2; });
    (void)ctx.get(s);
  });

  bench("cold_first_get", "thread_safe_context", 100000, [&]() {
    ThreadSafeContext ctx;
    auto c = ctx.cell(42);
    auto s = ctx.slot<int>([c](Context& ctx) { return ctx.get_cell(c) * 2; });
    (void)ctx.get(s);
  });
}

// -- Dependency fan-out --
void bench_fan_out() {
  for (int n : {32, 256}) {
    Context ctx;
    auto src = ctx.cell(1);
    std::vector<SlotHandle<int>> slots;
    for (int i = 0; i < n; ++i) {
      slots.push_back(ctx.computed<int>([&, i](Context& c) {
        return c.get_cell(src) + i;
      }));
    }
    for (auto& s : slots) (void)ctx.get(s);  // prime
    ctx.set_cell(src, 2);  // invalidate

    std::string label = "context / " + std::to_string(n);
    bench("dependency_fan_out", label, 10000, [&]() {
      for (auto& s : slots) (void)ctx.get(s);
    });

    // Invalidate again for next round
    ctx.set_cell(src, 1);
  }

  for (int n : {32, 256}) {
    ThreadSafeContext ctx;
    auto src = ctx.cell(1);
    std::vector<SlotHandle<int>> slots;
    for (int i = 0; i < n; ++i) {
      slots.push_back(ctx.slot<int>([&, i](Context& c) {
        return c.get_cell(src) + i;
      }));
    }
    for (auto& s : slots) (void)ctx.get(s);
    ctx.set_cell(src, 2);

    std::string label = "thread_safe_context / " + std::to_string(n);
    bench("dependency_fan_out", label, 10000, [&]() {
      for (auto& s : slots) (void)ctx.get(s);
    });
    ctx.set_cell(src, 1);
  }
}

// -- Set cell invalidation --
void bench_set_cell_invalidation() {
  // High fan-out (512 dependents)
  {
    Context ctx;
    auto src = ctx.cell(0);
    std::vector<SlotHandle<int>> slots;
    for (int i = 0; i < 512; ++i) {
      slots.push_back(ctx.computed<int>([&, i](Context& c) {
        return c.get_cell(src) + i;
      }));
    }
    for (auto& s : slots) (void)ctx.get(s);

    bench("set_cell_invalidation", "high_fan_out / 512", 1000, [&]() {
      ctx.set_cell(src, 1);
      ctx.set_cell(src, 0);  // reset
    });
  }

  // Same-slot contention
  for (int n : {1, 2, 4, 8, 16}) {
    Context ctx;
    auto src = ctx.cell(0);
    std::vector<SlotHandle<int>> slots;
    for (int i = 0; i < n; ++i) {
      slots.push_back(ctx.computed<int>([&, i](Context& c) {
        return c.get_cell(src) + i;
      }));
    }
    for (auto& s : slots) (void)ctx.get(s);

    std::string label = "same_slot_contention / " + std::to_string(n);
    bench("set_cell_invalidation", label, 10000, [&]() {
      ctx.set_cell(src, 1);
      ctx.set_cell(src, 0);
    });
  }

  // Independent-slot contention
  for (int n : {1, 2, 4, 8, 16}) {
    Context ctx;
    std::vector<CellHandle<int>> cells;
    std::vector<SlotHandle<int>> slots;
    for (int i = 0; i < n; ++i) {
      cells.push_back(ctx.cell(i));
      slots.push_back(ctx.computed<int>([&, i](Context& c) {
        return c.get_cell(cells[i]) + 1;
      }));
    }
    for (auto& s : slots) (void)ctx.get(s);

    std::string label = "independent_slot_contention / " + std::to_string(n);
    bench("set_cell_invalidation", label, 10000, [&]() {
      for (int i = 0; i < n; ++i) ctx.set_cell(cells[i], i + 100);
      for (int i = 0; i < n; ++i) ctx.set_cell(cells[i], i);
    });
  }
}

// -- Memo equality suppression --
void bench_memo_equality() {
  Context ctx;
  auto src = ctx.cell(2);
  auto m = ctx.memo<int>([&](Context& c) {
    return c.get_cell(src) / 2;  // 2/2=1, 3/2=1 (unchanged)
  });
  auto downstream = ctx.computed<int>([&](Context& c) {
    return c.get(m) + 1;
  });
  (void)ctx.get(downstream);

  bench("memo_equality_suppression", "context", 100000, [&]() {
    ctx.set_cell(src, 3);  // memo unchanged → downstream stays cached
    ctx.set_cell(src, 2);  // reset
  });

  ThreadSafeContext ts_ctx;
  auto ts_src = ts_ctx.cell(2);
  auto ts_m = ts_ctx.memo<int>([&](Context& c) {
    return c.get_cell(ts_src) / 2;
  });
  auto ts_down = ts_ctx.slot<int>([&](Context& c) {
    return c.get(ts_m) + 1;
  });
  (void)ts_ctx.get(ts_down);

  bench("memo_equality_suppression", "thread_safe_context", 100000, [&]() {
    ts_ctx.set_cell(ts_src, 3);
    ts_ctx.set_cell(ts_src, 2);
  });
}

// -- Effect flushing --
void bench_effect_flushing() {
  Context ctx;
  auto src = ctx.cell(0);
  int sink = 0;
  auto eff = ctx.effect_void([&](Context& c) {
    sink = c.get_cell(src);
  });

  bench("effect_flushing", "context", 1000000, [&]() {
    ctx.set_cell(src, 1);
    ctx.set_cell(src, 0);
  });

  ThreadSafeContext ts_ctx;
  auto ts_src = ts_ctx.cell(0);
  int ts_sink = 0;
  auto ts_eff = ts_ctx.effect_void([&](Context& c) {
    ts_sink = c.get_cell(ts_src);
  });

  bench("effect_flushing", "thread_safe_context", 1000000, [&]() {
    ts_ctx.set_cell(ts_src, 1);
    ts_ctx.set_cell(ts_src, 0);
  });
}

// -- Batch storms --
void bench_batch_storms() {
  Context ctx;
  std::vector<CellHandle<int>> cells;
  for (int i = 0; i < 64; ++i) cells.push_back(ctx.cell(i));
  auto sum = ctx.computed<int>([&](Context& c) {
    int s = 0;
    for (auto& cell : cells) s += c.get_cell(cell);
    return s;
  });
  (void)ctx.get(sum);

  bench("batch_storms", "context / 64", 100000, [&]() {
    ctx.batch([&](Context& c) {
      for (int i = 0; i < 64; ++i) c.set_cell(cells[i], i + 1);
    });
    ctx.batch([&](Context& c) {
      for (int i = 0; i < 64; ++i) c.set_cell(cells[i], i);
    });
  });

  ThreadSafeContext ts_ctx;
  std::vector<CellHandle<int>> ts_cells;
  for (int i = 0; i < 64; ++i) ts_cells.push_back(ts_ctx.cell(i));
  auto ts_sum = ts_ctx.slot<int>([&](Context& c) {
    int s = 0;
    for (auto& cell : ts_cells) s += c.get_cell(cell);
    return s;
  });
  (void)ts_ctx.get(ts_sum);

  bench("batch_storms", "thread_safe_context / 64", 100000, [&]() {
    ts_ctx.batch([&](Context& c) {
      for (int i = 0; i < 64; ++i) c.set_cell(ts_cells[i], i + 1);
    });
    ts_ctx.batch([&](Context& c) {
      for (int i = 0; i < 64; ++i) c.set_cell(ts_cells[i], i);
    });
  });
}

// -- Thread-safe contention --
void bench_ts_contention() {
  for (int n : {1, 2, 4, 8, 16}) {
    ThreadSafeContext ctx;
    auto cell = ctx.cell(0);
    auto slot = ctx.slot<int>([&](Context& c) { return c.get_cell(cell) * 2; });
    (void)ctx.get(slot);

    std::atomic<int> barrier{n};
    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < n; ++t) {
      threads.emplace_back([&]() {
      --barrier;
      while (barrier > 0) std::this_thread::yield();
      while (!stop.load()) {
        ctx.set_cell(cell, 1);
        (void)ctx.get(slot);
        ctx.set_cell(cell, 0);
      }
      });
    }

    // Measure for 10ms
    auto start = clk::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    stop.store(true);
    for (auto& t : threads) t.join();
    auto end = clk::now();
    double ns = std::chrono::duration<double, std::nano>(end - start).count();
    // Convert to per-iteration (approximate: just report total)
    std::string label = "same_slot_write_read / " + std::to_string(n);
    results.push_back({"thread_safe_contention", label, ns, n});
  }
}

// -- Scale benchmark --
void bench_scale() {
  for (int n : {100000, 1000000}) {
    // Build
    double build_ns;
    {
      auto start = clk::now();
      Context ctx;
      ctx.reserve(static_cast<size_t>(n) * 2 + 16);
      std::vector<CellHandle<int>> inputs;
      std::vector<SlotHandle<int>> formulas;
      inputs.reserve(n);
      formulas.reserve(n);
      for (int i = 0; i < n; ++i) inputs.push_back(ctx.cell(i));
      for (int i = 0; i < n; ++i) {
        if (i == 0) {
          formulas.push_back(ctx.computed<int>([&](Context& c) {
            return c.get_cell(inputs[0]);
          }));
        } else {
          auto prev = inputs[i - 1];
          auto cur = inputs[i];
          formulas.push_back(ctx.computed<int>([prev, cur](Context& c) {
            return c.get_cell(cur) + c.get_cell(prev);
          }));
        }
      }
      auto end = clk::now();
      build_ns = std::chrono::duration<double, std::nano>(end - start).count();

      std::string label = "build / " + std::to_string(n);
      results.push_back({"scale", label, build_ns, 1});

      // Cold full recalc
      start = clk::now();
      for (auto& f : formulas) (void)ctx.get(f);
      end = clk::now();
      label = "cold_full_recalc / " + std::to_string(n);
      results.push_back({"scale", label,
        std::chrono::duration<double, std::nano>(end - start).count(), 1});

      // Full recalc invalidate all
      start = clk::now();
      for (int i = 0; i < n; ++i) ctx.set_cell(inputs[i], i + 1);
      for (auto& f : formulas) (void)ctx.get(f);
      end = clk::now();
      label = "full_recalc_invalidate_all / " + std::to_string(n);
      results.push_back({"scale", label,
        std::chrono::duration<double, std::nano>(end - start).count(), 1});

      // Viewport recalc (edit 1, read 1000)
      start = clk::now();
      ctx.set_cell(inputs[0], 999);
      for (int i = 0; i < std::min(1000, n); ++i) (void)ctx.get(formulas[i]);
      end = clk::now();
      label = "viewport_recalc / " + std::to_string(n);
      results.push_back({"scale", label,
        std::chrono::duration<double, std::nano>(end - start).count(), 1});
    }
  }
}

int main() {
  std::cout << "lazily-cpp benchmark suite\n";
  std::cout << "==========================\n\n";

  bench_cached_reads();
  bench_cold_first_get();
  bench_fan_out();
  bench_set_cell_invalidation();
  bench_memo_equality();
  bench_effect_flushing();
  bench_batch_storms();
  bench_ts_contention();
  bench_scale();

  print_results();
  return 0;
}
