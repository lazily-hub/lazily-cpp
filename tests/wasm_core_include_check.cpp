// The narrow-include contract, compiled and run (`#lzcppwasm` Phase 1).
//
// This translation unit includes `<lazily/core.hpp>` and NOTHING else from
// lazily. That is the entire point: if `core.hpp`'s transitive closure ever
// grows a POSIX or threading dependency, this TU stops building for wasm32 —
// the claim fails as a build error rather than as an unnoticed regression.
//
// A grep over `#include` lines cannot prove this. It reports what is written,
// not what the preprocessor actually pulls in, and it cannot see a dependency
// that arrives through a header three levels down. Compiling for the target is
// the only check that measures rather than describes.
//
// The body then exercises the kernel so the check is not merely a compile: a
// header that compiles but whose graph does not evaluate under wasm would pass
// a build-only assertion. Reads, writes, guarded recompute, collections,
// keyed order, and a statechart transition all run here.

#include <lazily/core.hpp>

#include <cstdio>
#include <string>
#include <unordered_map>

// `core.hpp` defines this. Its absence means this TU was compiled against some
// other header — the check would then be measuring the wrong thing.
#ifndef LAZILY_CORE
#error "wasm_core_include_check.cpp must be compiled against <lazily/core.hpp>"
#endif

// The umbrella and the native-only headers must NOT have been dragged in.
// Their include guards are the observable evidence, so this is a real check
// rather than a comment.
#ifdef LAZILY_TRANSPORT_HPP
#error "core.hpp pulled in transport.hpp — the freestanding closure has regressed"
#endif
#ifdef LAZILY_RELIABLE_SYNC_HPP
#error "core.hpp pulled in reliable_sync.hpp — the freestanding closure has regressed"
#endif
#ifdef LAZILY_WORK_QUEUE_HPP
#error "core.hpp pulled in work_queue.hpp — the freestanding closure has regressed"
#endif
#ifdef LAZILY_ASYNC_CONTEXT_HPP
#error "core.hpp pulled in async_context.hpp — the freestanding closure has regressed"
#endif
#ifdef LAZILY_THREAD_SAFE_HPP
#error "core.hpp pulled in thread_safe.hpp — the freestanding closure has regressed"
#endif

using namespace lazily;

static int failures = 0;

static void check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

int main() {
  // ── kernel: read, write, guarded recompute ────────────────────────────────
  {
    Context ctx;
    Source<long long> n = ctx.source<long long>(2);
    int computes = 0;
    Computed<long long> doubled = ctx.computed<long long>([n, &computes](Compute& c) {
      ++computes;
      return n.get(c) * 2;
    });
    check(doubled.get(ctx) == 4, "computed reads upstream");
    check(computes == 1, "one compute");
    doubled.get(ctx);
    check(computes == 1, "cached read does not recompute");
    n.set(ctx, 2);
    check(computes == 1, "guarded: an equal write does not recompute");
    n.set(ctx, 3);
    check(doubled.get(ctx) == 6, "recomputes when upstream changes");
    check(computes == 2, "exactly one recompute");
  }

  // ── depth: a write must reach past the first level ────────────────────────
  //
  // Read ONLY the deepest node after the write. Reading each level on the way
  // down can mask a one-level-mark defect by refreshing the chain as it goes.
  {
    Context ctx;
    Source<long long> root = ctx.source<long long>(1);
    Computed<long long> a = ctx.computed<long long>([root](Compute& c) { return root.get(c) + 1; });
    Computed<long long> b = ctx.computed<long long>([a](Compute& c) { return a.get(c) + 1; });
    Computed<long long> d = ctx.computed<long long>([b](Compute& c) { return b.get(c) + 1; });
    check(d.get(ctx) == 4, "chain settles");
    root.set(ctx, 10);
    check(d.get(ctx) == 13, "invalidation reaches depth 3");
  }

  // ── keyed order: present set plus authoritative order ─────────────────────
  {
    KeyedOrder<std::string, int> ko;
    ko.insert("a", 1);
    ko.insert("b", 2);
    ko.insert("c", 3);
    check(ko.len() == 3, "keyed order holds three keys");
    check(ko.keys().size() == 3, "order is in lockstep with entries");
    check(ko.keys()[0] == "a" && ko.keys()[2] == "c", "insertion order preserved");
    check(ko.position("b") == std::size_t{1}, "position reports the authoritative order");
  }

  // ── statechart: a real transition over the kernel ──────────────────────────
  {
    Context ctx;
    auto def = ChartBuilder()
                   .state(StateBuilder::compound("root", "a"))
                   .state(StateBuilder::atomic("a").parent("root").on("next", "b"))
                   .state(StateBuilder::atomic("b").parent("root"))
                   .build()
                   .value();
    StateChart chart(ctx, std::move(def));
    auto leaves = chart.active_leaves(ctx);
    check(leaves.size() == 1 && leaves[0] == "a", "statechart starts in its initial leaf");

    std::unordered_map<std::string, bool> guards;
    check(chart.send(ctx, "next", guards), "the transition is taken");
    leaves = chart.active_leaves(ctx);
    check(leaves.size() == 1 && leaves[0] == "b", "statechart moved to the target leaf");
  }

  if (failures != 0) {
    std::printf("wasm_core_include_check: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("wasm_core_include_check: ok (freestanding core.hpp builds and runs)\n");
  return 0;
}
