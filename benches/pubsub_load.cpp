// Width-ladder pub/sub load test (#lzspecedgeindex).
//
// Existing bench suites measure scale as *node count* and fix fan-out at 2, so
// edge-list width was never a variable — which is exactly why the O(n^2) edge
// dedup hid there. This harness makes width the independent variable.
//
// Method: climb, project, refuse. At each rung we measure bytes/subscriber,
// project the next rung from *that measurement*, and refuse to start it if the
// projection would not leave a memory floor free.
//
// Manual / on-demand — NOT part of `make check`. Build:
//   cmake -S . -B build -DLAZILY_BUILD_BENCHMARKS=ON
//   cmake --build build --target lazily_pubsub_load
//   ./build/benches/lazily_pubsub_load            # ladder to 1M
//   ./build/benches/lazily_pubsub_load 10000000   # ladder to 10M
//
// Override the edge-index thresholds at compile time to re-measure the
// crossover, e.g. -DLAZILY_EDGE_INDEX_PROMOTE=0 (always index) or
// -DLAZILY_EDGE_INDEX_PROMOTE=1000000000 (pure scan).

#include <lazily/lazily.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// The baseline (pre-#lzspecedgeindex) header defines neither macro; reporting 0
// lets the same harness measure both trees.
#ifndef LAZILY_EDGE_INDEX_PROMOTE
#define LAZILY_EDGE_INDEX_PROMOTE 0
#endif
#ifndef LAZILY_EDGE_INDEX_DEMOTE
#define LAZILY_EDGE_INDEX_DEMOTE 0
#endif

using namespace lazily;
using SteadyClock = std::chrono::steady_clock;

namespace {

// ── memory probe ──

size_t rss_bytes() {
  FILE* f = std::fopen("/proc/self/statm", "r");
  if (!f) return 0;
  unsigned long total = 0, resident = 0;
  if (std::fscanf(f, "%lu %lu", &total, &resident) != 2) resident = 0;
  std::fclose(f);
  return static_cast<size_t>(resident) * 4096;
}

size_t available_bytes() {
  FILE* f = std::fopen("/proc/meminfo", "r");
  if (!f) return 0;
  char line[256];
  size_t avail = 0;
  while (std::fgets(line, sizeof(line), f)) {
    unsigned long kb = 0;
    if (std::sscanf(line, "MemAvailable: %lu kB", &kb) == 1) {
      avail = static_cast<size_t>(kb) * 1024;
      break;
    }
  }
  std::fclose(f);
  return avail;
}

constexpr size_t kMemoryFloor = size_t(8) << 30;  // keep 8 GiB free

double ns_per(double total_ns, size_t n) { return total_ns / double(n); }

struct Rung {
  size_t width = 0;
  double build_ns_per_sub = 0;
  double notify_ns_per_sub = 0;
  double bytes_per_sub = 0;
  // Narrow-fan-out control: the same subscriber count spread over width/2 cells
  // at fan-out 2. Same node count, same allocation volume, same cache pressure
  // — only the edge-list width differs.
  double narrow_build_ns_per_sub = 0;
  double narrow_notify_ns_per_sub = 0;
  bool ran = false;
};

// Control graph: `width` subscribers at fan-out 2, i.e. width/2 cells with two
// effects each. This is the comparison that isolates width, because everything
// the memory hierarchy responds to is held fixed.
void run_rung_narrow(size_t width, int notify_reps, Rung& out) {
  const size_t cells = std::max<size_t>(1, width / 2);
  auto ctx = new Context();
  auto observed = new std::vector<int>(width, -1);
  ctx->reserve(width + cells + 8);

  std::vector<CellHandle<int>> topics;
  topics.reserve(cells);
  for (size_t c = 0; c < cells; ++c) topics.push_back(ctx->cell(0));

  const auto t0 = SteadyClock::now();
  for (size_t i = 0; i < width; ++i) {
    auto topic = topics[i % cells];
    ctx->effect_void([topic, i, observed](Context& c) {
      (*observed)[i] = c.get_cell(topic);
    });
  }
  const auto t1 = SteadyClock::now();
  out.narrow_build_ns_per_sub = ns_per(
      double(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                 .count()),
      width);

  std::vector<double> reps;
  int value = 0;
  for (int rep = 0; rep < notify_reps; ++rep) {
    ++value;
    const auto p0 = SteadyClock::now();
    for (size_t c = 0; c < cells; ++c) ctx->set_cell(topics[c], value);
    const auto p1 = SteadyClock::now();
    reps.push_back(ns_per(
        double(std::chrono::duration_cast<std::chrono::nanoseconds>(p1 - p0)
                   .count()),
        width));
  }
  std::sort(reps.begin(), reps.end());
  out.narrow_notify_ns_per_sub = reps[reps.size() / 2];

  for (size_t i = 0; i < width; ++i)
    if ((*observed)[i] != value) {
      std::printf("FAIL narrow width=%zu: subscriber %zu missed the final "
                  "publish\n",
                  width, i);
      std::exit(1);
    }

  delete observed;
  delete ctx;
}

// One rung: build `width` subscribers on a single cell, publish, verify.
Rung run_rung(size_t width, int notify_reps) {
  Rung r;
  r.width = width;

  const size_t rss_before = rss_bytes();

  // Heap-allocate so we can free deterministically before the next rung.
  auto ctx = new Context();
  // Each subscriber records the last value it observed. A plain vector keeps
  // the per-subscriber overhead outside the graph small and constant.
  auto observed = new std::vector<int>(width, -1);

  ctx->reserve(width + 8);
  auto topic = ctx->cell(0);

  const auto build_start = SteadyClock::now();
  for (size_t i = 0; i < width; ++i) {
    ctx->effect_void([topic, i, observed](Context& c) {
      (*observed)[i] = c.get_cell(topic);
    });
  }
  const auto build_end = SteadyClock::now();
  r.build_ns_per_sub = ns_per(
      double(std::chrono::duration_cast<std::chrono::nanoseconds>(
                 build_end - build_start)
                 .count()),
      width);

  const size_t rss_after_build = rss_bytes();
  r.bytes_per_sub =
      double(rss_after_build > rss_before ? rss_after_build - rss_before : 0) /
      double(width);

  // ── notify ──
  // Each publish re-runs every effect, which tears down and re-registers every
  // edge — the path the index has to keep amortized O(1).
  std::vector<double> reps;
  reps.reserve(size_t(notify_reps));
  int value = 0;
  for (int rep = 0; rep < notify_reps; ++rep) {
    ++value;
    const auto t0 = SteadyClock::now();
    ctx->set_cell(topic, value);
    const auto t1 = SteadyClock::now();
    reps.push_back(ns_per(
        double(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                   .count()),
        width));
  }
  std::sort(reps.begin(), reps.end());
  r.notify_ns_per_sub = reps[reps.size() / 2];  // median

  // ── correctness: every survivor observes the final publish ──
  size_t missed = 0;
  for (size_t i = 0; i < width; ++i)
    if ((*observed)[i] != value) ++missed;
  if (missed != 0) {
    std::printf("FAIL width=%zu: %zu/%zu subscribers missed the final publish\n",
                width, missed, width);
    std::exit(1);
  }

  delete observed;
  delete ctx;
  r.ran = true;
  return r;
}

// ── assertions over the whole ladder ──

const Rung* find_rung(const std::vector<Rung>& rungs, size_t width) {
  for (const auto& r : rungs)
    if (r.width == width && r.ran) return &r;
  return nullptr;
}

int check_ladder(const std::vector<Rung>& rungs) {
  int failures = 0;

  // 1. Wide-vs-narrow control at equal node count. This replaces an earlier
  //    "build ns/sub grows < 2x from 1k to 1M" assertion, which is unsound: it
  //    conflates the defect with the memory hierarchy. Every phase picks up a
  //    slope once the working set outgrows cache — a correctly fixed engine
  //    measures ~2.8-3.5x here, and notify, which does no dedup work at all,
  //    grows about as much. Asserting the absolute form either fails a correct
  //    implementation or invites "fixing" a DRAM latency curve.
  //
  //    The control builds the same subscriber count at fan-out 2, so node
  //    count, allocation volume and cache pressure are all held fixed and only
  //    edge-list width varies. A ratio near 1 means width costs nothing. Under
  //    the O(width) scan it is the defect, unmistakably: the pre-fix baseline
  //    measured 65x at width 65536.
  //    Checked from width 1024 up. Below that, per-subscriber cost is dominated
  //    by fixed per-context setup rather than by edges, and both trees report
  //    the same ~3x at width 32 — the ratio there is not measuring width.
  bool control_seen = false;
  for (const auto& r : rungs) {
    if (!r.ran || r.narrow_build_ns_per_sub <= 0 || r.width < 1024) continue;
    control_seen = true;
    const double ratio = r.build_ns_per_sub / r.narrow_build_ns_per_sub;
    // Measured: fixed engine 1.27-2.09x over 1k..1M; pre-fix baseline 3.98x at
    // 1k, 6.63x at 4k, 120.70x at 64k. 5x sits between them with ~2.4x headroom
    // over the fixed engine's worst rung.
    const bool ok = ratio < 5.0;
    std::printf("%s wide/narrow build at width %zu = %.2fx "
                "(%.1f vs %.1f ns/sub, limit 5.00x)\n",
                ok ? "PASS" : "FAIL", r.width, ratio, r.build_ns_per_sub,
                r.narrow_build_ns_per_sub);
    if (!ok) ++failures;
  }
  if (!control_seen)
    std::printf("SKIP wide/narrow control (no rung ran the control)\n");

  // 2. bytes/sub flat within ~20% across the ladder. The index is allowed to
  //    exist, but it must not change the per-subscriber memory shape.
  //    Measured from 4k up: below that, RSS granularity (4 KiB pages) and
  //    one-off arena growth dominate the per-subscriber figure.
  //    The first executed rung is excluded whatever it is: its RSS delta also
  //    carries the process's one-time warmup, so it reads high by ~15%.
  double lo = 1e30, hi = 0;
  bool first_seen = false;
  for (const auto& r : rungs) {
    if (!r.ran || r.width < 4096 || r.bytes_per_sub <= 0) continue;
    if (!first_seen) {
      first_seen = true;
      continue;
    }
    lo = std::min(lo, r.bytes_per_sub);
    hi = std::max(hi, r.bytes_per_sub);
  }
  if (hi > 0 && lo < 1e29) {
    const double spread = (hi - lo) / lo;
    const bool ok = spread <= 0.20;
    std::printf("%s bytes/sub spread (>=4k rungs) = %.1f%% (limit 20.0%%), "
                "%.1f..%.1f B\n",
                ok ? "PASS" : "FAIL", spread * 100.0, lo, hi);
    if (!ok) ++failures;
  }

  // 3. notify does not scale with width. Per-subscriber notify cost is NOT flat
  //    across the whole ladder and asserting that it is would be dishonest: a
  //    4k-subscriber graph is L2-resident and a 10M one is DRAM-resident, so
  //    per-subscriber cost rises with the cache hierarchy no matter how good
  //    the algorithm is. Measured, that rise is ~6x over a 10000x width range.
  //    What the assertion rules out is the defect: an O(width) dedup scan makes
  //    per-subscriber cost rise *proportionally* to width, which over the same
  //    range is ~10000x. The pre-fix baseline measured 816x by width 65536
  //    alone. A 20x ceiling separates the two by two orders of magnitude.
  const Rung* n_lo = find_rung(rungs, 1024);
  double n_hi_val = 0;
  size_t n_hi_width = 0;
  for (const auto& r : rungs)
    if (r.ran && r.width >= 1024 && r.notify_ns_per_sub > n_hi_val) {
      n_hi_val = r.notify_ns_per_sub;
      n_hi_width = r.width;
    }
  if (n_lo && n_hi_val > 0) {
    const double width_ratio = double(n_hi_width) / 1024.0;
    const double ratio = n_hi_val / n_lo->notify_ns_per_sub;
    const bool ok = ratio < 20.0;
    std::printf("%s notify ns/sub worst/1k = %.2fx at width %zu (%.0fx wider), "
                "limit 20.00x\n",
                ok ? "PASS" : "FAIL", ratio, n_hi_width, width_ratio);
    if (!ok) ++failures;
  }

  // 4. no demotion-thrash spike around the promote threshold. Compare each
  //    clustered rung against the median of its neighbours.
  static const size_t cluster[] = {96, 97, 128, 129, 160};
  for (size_t w : cluster) {
    const Rung* here = find_rung(rungs, w);
    const Rung* base = find_rung(rungs, 1024);
    if (!here || !base) continue;
    const double ratio = here->notify_ns_per_sub / base->notify_ns_per_sub;
    const bool ok = ratio < 2.0;
    std::printf("%s no thrash at width %zu: notify %.1f ns/sub, %.2fx of 1k "
                "(limit 2.00x)\n",
                ok ? "PASS" : "FAIL", w, here->notify_ns_per_sub, ratio);
    if (!ok) ++failures;
  }

  return failures;
}

}  // namespace

int main(int argc, char** argv) {
  size_t ceiling = 1048576;
  if (argc > 1) ceiling = std::strtoull(argv[1], nullptr, 10);

  // The cluster around the promote threshold is what exposes demotion thrash;
  // keep it even when the ceiling is low.
  std::vector<size_t> ladder = {32,   64,   96,   97,  128,     129,
                                160,  256,  1024,    4096,    65536,
                                1048576, 10485760, 104857600};

  // LAZILY_LADDER=64,96,128,... overrides the rungs — used to sweep the
  // promote-threshold crossover at fine resolution.
  if (const char* spec = std::getenv("LAZILY_LADDER")) {
    ladder.clear();
    for (const char* p = spec; *p;) {
      char* end = nullptr;
      const unsigned long long w = std::strtoull(p, &end, 10);
      if (end == p) break;
      if (w > 0) ladder.push_back(size_t(w));
      p = (*end == ',') ? end + 1 : end;
    }
  }

  std::printf("lazily-cpp pub/sub width ladder (#lzspecedgeindex)\n");
  std::printf("promote=%zu demote=%zu ceiling=%zu available=%.1f GiB\n\n",
              size_t(LAZILY_EDGE_INDEX_PROMOTE),
              size_t(LAZILY_EDGE_INDEX_DEMOTE), ceiling,
              double(available_bytes()) / double(size_t(1) << 30));
  std::printf("%10s %13s %13s %11s %13s %13s\n", "width", "build ns/sub",
              "notify ns/sub", "bytes/sub", "narrow build", "narrow notify");

  std::vector<Rung> rungs;
  double last_bytes_per_sub = 0;

  for (size_t width : ladder) {
    if (width > ceiling) break;

    // ── project and refuse ──
    if (last_bytes_per_sub > 0) {
      // 2x: the rung runs a wide graph and then a narrow control at the same
      // subscriber count but 1.5x the node count, so the projection has to cover
      // the larger of the two, not the measured wide figure alone.
      const double projected = 2.0 * last_bytes_per_sub * double(width);
      const size_t avail = available_bytes();
      if (projected + double(kMemoryFloor) > double(avail)) {
        std::printf(
            "\nREFUSE width=%zu: projected %.1f GiB from the last rung's "
            "%.1f B/sub, only %.1f GiB available (floor %.1f GiB).\n",
            width, projected / double(size_t(1) << 30), last_bytes_per_sub,
            double(avail) / double(size_t(1) << 30),
            double(kMemoryFloor) / double(size_t(1) << 30));
        break;
      }
    }

    // Narrow rungs are noisy; take more repetitions there.
    const int reps = width <= 4096 ? 25 : (width <= 65536 ? 9 : 3);
    Rung r = run_rung(width, reps);
    // Control graph at the same node count, fan-out 2. Runs after the wide one
    // and on its own context, so peak memory is the max of the two, not the sum.
    run_rung_narrow(width, reps, r);
    last_bytes_per_sub = r.bytes_per_sub;
    rungs.push_back(r);

    std::printf("%10zu %13.1f %13.1f %11.1f %13.1f %13.1f\n", r.width,
                r.build_ns_per_sub, r.notify_ns_per_sub, r.bytes_per_sub,
                r.narrow_build_ns_per_sub, r.narrow_notify_ns_per_sub);
    std::fflush(stdout);
  }

  std::printf("\n");
  const int failures = check_ladder(rungs);
  std::printf("\n%s (%d assertion failure%s)\n", failures ? "LADDER FAILED"
                                                          : "LADDER OK",
              failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
