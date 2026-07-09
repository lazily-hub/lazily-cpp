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
  // When non-empty, overrides the duration auto-formatting (e.g. "Mops/s").
  std::string unit_override;
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
  results.push_back({group, case_name, mean, iterations, {}});
}

// Report a derived (non-duration) metric with an explicit unit.
void report(const std::string& group, const std::string& case_name,
            double value, int samples, const std::string& unit) {
  results.push_back({group, case_name, value, samples, unit});
}

void print_results() {
  std::cout << "\n| Group | Case | Mean | Samples |\n";
  std::cout << "|---|---|---:|---:|\n";
  for (auto& r : results) {
    double val = r.mean_ns;
    std::string unit;
    if (!r.unit_override.empty()) {
      unit = r.unit_override;
    } else {
      unit = "ns";
      if (val >= 1e9) { val /= 1e9; unit = "s"; }
      else if (val >= 1e6) { val /= 1e6; unit = "ms"; }
      else if (val >= 1e3) { val /= 1e3; unit = "us"; }
    }
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
//
// Measures real multi-threaded scaling under a ThreadSafeContext variant. Each
// worker thread hammers `set_cell + get` on a shared cell for a fixed wall-clock
// window; we count COMPLETED ops and report total throughput (Mops/s) and
// per-op latency. This is a write-dominated single-cell hotspot: it serializes
// under any single-writer-cell design; the comparison across lock policies
// shows the exclusive-acquire cost trade-off (RW's heavier exclusive acquire
// regresses write-heavy single-thread paths).
template <typename Ctx>
void ts_contention_for(const std::string& policy) {
  constexpr int kWindowMs = 30;
  for (int n : {1, 2, 4, 8, 16}) {
    Ctx ctx;
    auto cell = ctx.cell(0);
    auto slot = ctx.template slot<int>([&](Context& c) { return c.get_cell(cell) * 2; });
    (void)ctx.get(slot);

    std::atomic<int> barrier{n};
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> ops{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < n; ++t) {
      threads.emplace_back([&]() {
        --barrier;
        while (barrier > 0) std::this_thread::yield();
        uint64_t local = 0;
        int v = 0;
        while (!stop.load(std::memory_order_relaxed)) {
          ctx.set_cell(cell, v);
          (void)ctx.get(slot);
          v ^= 1;
          ++local;
        }
        ops.fetch_add(local, std::memory_order_relaxed);
      });
    }

    auto start = clk::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(kWindowMs));
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : threads) t.join();
    auto end = clk::now();

    double window_ns =
        std::chrono::duration<double, std::nano>(end - start).count();
    uint64_t total_ops = ops.load();
    double throughput_mops = (total_ops / (window_ns / 1e9)) / 1e6;  // ops/s → Mops/s
    double per_op_ns = total_ops ? window_ns / static_cast<double>(total_ops) : 0.0;

    std::string nthr = std::to_string(n);
    report("contention/" + policy, "total_throughput / " + nthr,
           throughput_mops, n, "Mops/s");
    report("contention/" + policy, "per_op_latency / " + nthr,
           per_op_ns, n, {});
  }
}

void bench_ts_contention() {
  ts_contention_for<ThreadSafeContext>("recursive");
  ts_contention_for<RwThreadSafeContext>("rw");
  ts_contention_for<ScalableThreadSafeContext>("scalable");
}

// -- Thread-safe read scaling --
//
// Many concurrent CACHED reads with no writes. Recursive policy: reads take an
// exclusive lock and serialize (no scaling). RW policy: cached reads take a
// SHARED lock and scale across cores (~1.7× at 16 threads on this host).
template <typename Ctx>
void read_scaling_for(const std::string& policy) {
  constexpr int kWindowMs = 30;
  for (int n : {1, 2, 4, 8, 16}) {
    Ctx ctx;
    auto cell = ctx.cell(42);
    auto slot = ctx.template slot<int>([&](Context& c) { return c.get_cell(cell) * 2; });
    (void)ctx.get(slot);  // prime cache (clean)

    std::atomic<int> barrier{n};
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> ops{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < n; ++t) {
      threads.emplace_back([&]() {
        --barrier;
        while (barrier > 0) std::this_thread::yield();
        uint64_t local = 0;
        while (!stop.load(std::memory_order_relaxed)) {
          (void)ctx.get_cell(cell);
          (void)ctx.get(slot);
          ++local;
        }
        ops.fetch_add(local, std::memory_order_relaxed);
      });
    }

    auto start = clk::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(kWindowMs));
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : threads) t.join();
    auto end = clk::now();

    double window_ns =
        std::chrono::duration<double, std::nano>(end - start).count();
    uint64_t total_ops = ops.load();
    double throughput_mops = (total_ops / (window_ns / 1e9)) / 1e6;
    double per_op_ns = total_ops ? window_ns / static_cast<double>(total_ops) : 0.0;

    std::string nthr = std::to_string(n);
    report("read_scaling/" + policy, "total_throughput / " + nthr,
           throughput_mops, n, "Mops/s");
    report("read_scaling/" + policy, "per_op_latency / " + nthr,
           per_op_ns, n, {});
  }
}

void bench_read_scaling() {
  read_scaling_for<ThreadSafeContext>("recursive");
  read_scaling_for<RwThreadSafeContext>("rw");
  read_scaling_for<ScalableThreadSafeContext>("scalable");
}

// -- CRDT delta sync (distributed convergence hot path) --
//
// Measures the three core TextCrdt anti-entropy operations at realistic doc
// sizes: version_vector() (full elems_ scan), delta_since(empty) (build all
// TextOps for an initial sync), and apply_delta(all) (insert all into a fresh
// peer). These dominate distributed convergence speed.
void bench_crdt_sync() {
  for (long long n : {1000, 10000, 100000}) {
    TextCrdt a = TextCrdt::from_str(1, std::string(static_cast<size_t>(n), 'x'));
    TextCrdt::VersionVector vv_empty;
    auto delta = a.delta_since(vv_empty);
    std::string label = std::to_string(n);

    bench("crdt_sync", "version_vector / " + label, 20,
          [&]() { (void)a.version_vector(); });
    bench("crdt_sync", "delta_since_empty / " + label, 20,
          [&]() { (void)a.delta_since(vv_empty); });
    bench("crdt_sync", "apply_delta_full / " + label, 20, [&]() {
      TextCrdt fresh(2);
      (void)fresh.apply_delta(delta);
    });
  }
}

// -- msgpack codec (serialization throughput) --
//
// Measures encode/decode of a Snapshot carrying N nodes (each a small payload +
// keyed path). This is the per-message cost of the IPC wire codec — the
// foundational serialization layer for distributed use.
void bench_codec() {
  for (long long n : {100, 1000, 10000}) {
    Snapshot s;
    s.epoch = n;
    for (long long i = 0; i < n; ++i) {
      s.nodes.push_back({static_cast<NodeId>(i), "cell",
                         NodeStatePayload{{0xAB, 0xCD, 0xEF, 0x01}},
                         NodeKey::create("doc/n" + std::to_string(i))});
    }
    IpcMessage m = IpcMessageSnapshot{std::move(s)};
    auto bytes = encode(m);
    std::string label = std::to_string(n) + "n/" + std::to_string(bytes.size()) + "B";

    bench("codec", "encode_snapshot / " + label, 20, [&]() { (void)encode(m); });
    bench("codec", "decode_snapshot / " + label, 20, [&]() { (void)decode(bytes); });
  }
}

// -- ShmBlobArena (IPC shared-memory blob path) --
//
// Measures per-blob write (Entry alloc + payload copy) and per-blob read
// (payload copy + full FNV-1a checksum recompute). The read path currently
// copies AND re-hashes every byte on every read — the benchmark surfaces the
// cost so the zero-copy trade-off can be evaluated against real numbers.
void bench_shm_blob() {
  for (long long sz : {64, 4096, 65536}) {
    ShmBlobArena arena(0);
    std::vector<uint8_t> payload(static_cast<size_t>(sz), 0xAB);
    auto ref = arena.write(payload);
    std::string label = std::to_string(sz);

    bench("shm_blob", "write / " + label + "B", 10000,
          [&]() { (void)arena.write(payload); });
    bench("shm_blob", "read / " + label + "B", 10000, [&]() {
      (void)arena.read(ref);
    });
    bench("shm_blob", "read_view / " + label + "B", 10000, [&]() {
      (void)arena.read_view(ref);
    });
  }
}

// -- Zero-copy transport (write / wire / resolve) --
//
// Splits the transport into its honest components so the trade-off is clear:
//   - write_spill: the one-time producer cost (copy into arena + FNV checksum),
//     amortized over N receivers;
//   - encode_decode_{inline,spilled}: the wire/codec cost — scales with wire
//     bytes, so a spilled message (tiny descriptor) stays ~constant as payload
//     grows, while inline grows with the payload;
//   - resolve: the receiver-side zero-copy read (~constant, no copy, no rehash).
void bench_transport() {
  for (long long sz : {256, 4096, 65536}) {
    std::vector<uint8_t> payload(static_cast<size_t>(sz), 0x5A);
    std::string label = std::to_string(sz);

    bench("transport", "write_spill / " + label + "B", 200, [&]() {
      InProcessBackend b;
      (void)b.write(payload);
    });

    InProcessBackend backend;
    BlobRouter router;
    router.register_backend(backend);
    ShmBlobRef ref = backend.write(payload);

    IpcMessage mi = IpcMessageDelta{Delta{1, 2, {DeltaOpSlotValue{1, IpcValueInline{payload}}}}};
    IpcMessage ms = IpcMessageDelta{Delta{1, 2, {DeltaOpSlotValue{1, IpcValueSharedBlob{ref}}}}};
    size_t inline_wire = encode(mi).size();
    size_t spill_wire = encode(ms).size();

    bench("transport", "encode_decode_inline / " + label + "B", 500, [&]() {
      (void)decode(encode(mi));
    });
    bench("transport", "encode_decode_spilled / " + label + "B", 500, [&]() {
      (void)decode(encode(ms));
    });
    bench("transport", "resolve / " + label + "B", 2000, [&]() {
      (void)backend.read_view(ref);
    });
    report("transport", "wire_inline / " + label + "B", static_cast<double>(inline_wire), 1, "B");
    report("transport", "wire_spilled / " + label + "B", static_cast<double>(spill_wire), 1, "B");
  }
}

// -- Scale benchmark --
void bench_scale() {
  for (long long n : {100000, 1000000, 2000000, 10000000}) {
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
      for (long long i = 0; i < n; ++i) inputs.push_back(ctx.cell(static_cast<int>(i)));
      for (long long i = 0; i < n; ++i) {
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
      for (long long i = 0; i < n; ++i) ctx.set_cell(inputs[i], static_cast<int>(i + 1));
      for (auto& f : formulas) (void)ctx.get(f);
      end = clk::now();
      label = "full_recalc_invalidate_all / " + std::to_string(n);
      results.push_back({"scale", label,
        std::chrono::duration<double, std::nano>(end - start).count(), 1});

      // Viewport recalc (edit 1, read 1000)
      start = clk::now();
      ctx.set_cell(inputs[0], 999);
      for (int i = 0; i < std::min(1000LL, n); ++i) (void)ctx.get(formulas[i]);
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
  bench_read_scaling();
  bench_crdt_sync();
  bench_shm_blob();
  bench_codec();
  bench_transport();
  bench_scale();

  print_results();
  return 0;
}
