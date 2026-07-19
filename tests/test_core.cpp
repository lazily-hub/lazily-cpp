#include <lazily/lazily.hpp>

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>
#include <set>
#include <vector>

using namespace lazily;

static int test_count = 0;
static int test_passed = 0;

// `make check` builds Release (-DNDEBUG), which compiles assert() out — an
// assert-only test passes vacuously there. REQUIRE survives NDEBUG.
#define REQUIRE(cond, msg)                                              \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::cout << "FAIL: " << (msg) << " @" << __FILE__ << ":"         \
                << __LINE__ << std::endl;                               \
      std::abort();                                                     \
    }                                                                   \
  } while (0)

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

// ── EdgeSet: the hash-indexed dependency-edge set (#lzspecedgeindex) ──
//
// These exercise the promoted (hash-indexed) regime directly. The reactive
// tests above only ever build low-degree graphs, which is precisely why the
// O(n^2) dedup went unnoticed — every one of them stays under the threshold.

TEST(test_edge_set_dedup_and_order) {
  EdgeSet s;
  assert(s.insert(SlotId(7)));
  assert(!s.insert(SlotId(7)));  // idempotent
  assert(s.size() == 1);
  assert(s.insert(SlotId(9)));
  assert(s.size() == 2);
  assert(!s.remove(SlotId(11)));  // absent
  assert(s.remove(SlotId(7)));
  assert(s.size() == 1);
  assert(s[0] == SlotId(9));
}

TEST(test_edge_set_promotes_and_stays_correct) {
  EdgeSet s;
  const size_t n = EdgeSet::kPromote * 4;
  for (size_t i = 0; i < n; ++i) assert(s.insert(SlotId(i * 3 + 1)));
  assert(s.size() == n);
  // Every member is still found (dedup) once indexed.
  for (size_t i = 0; i < n; ++i) assert(!s.insert(SlotId(i * 3 + 1)));
  // Non-members are still absent.
  for (size_t i = 0; i < n; ++i) assert(!s.remove(SlotId(i * 3 + 2)));
  assert(s.size() == n);
}

TEST(test_edge_set_matches_reference_under_churn) {
  // Differential test against std::set. Removal swaps the last element into the
  // hole and has to repoint the index at it; a tombstone written in the wrong
  // order silently loses an edge, which shows up here and nowhere else.
  EdgeSet s;
  std::set<uint64_t> reference;
  uint64_t rng = 0x243f6a8885a308d3ULL;
  auto next = [&rng]() {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
  };
  for (int step = 0; step < 200000; ++step) {
    const uint64_t id = next() % 400;  // small space => many collisions
    if (next() & 1) {
      assert(s.insert(SlotId(id)) == reference.insert(id).second);
    } else {
      assert(s.remove(SlotId(id)) == (reference.erase(id) == 1));
    }
    assert(s.size() == reference.size());
  }
  std::set<uint64_t> got;
  for (SlotId id : s) got.insert(id.value);
  assert(got == reference);
}

TEST(test_edge_set_crosses_thresholds_repeatedly) {
  // Drive the list across the promote and demote points many times. With a
  // single shared boundary this is the thrash case; either way it must stay
  // correct, which is what this asserts (the cost is measured in
  // benches/pubsub_load.cpp).
  EdgeSet s;
  for (int cycle = 0; cycle < 20; ++cycle) {
    for (size_t i = 0; i < EdgeSet::kPromote + 8; ++i) s.insert(SlotId(i));
    assert(s.size() == EdgeSet::kPromote + 8);
    for (size_t i = 0; i < EdgeSet::kPromote + 8; ++i) assert(s.remove(SlotId(i)));
    assert(s.empty());
  }
}

TEST(test_edge_set_clear_drops_the_index) {
  // A recycled id must not inherit an index. Here the index is owned by the set
  // rather than held in a side table keyed by owner, so clearing the set frees
  // it and no stale index can be aliased onto a later, unrelated node.
  EdgeSet s;
  for (size_t i = 0; i < EdgeSet::kPromote * 2; ++i) s.insert(SlotId(i));
  s.clear();
  assert(s.empty());
  assert(s.insert(SlotId(0)));  // would report "already present" against a stale index
  assert(s.size() == 1);
}

TEST(test_edge_set_copy_is_independent) {
  EdgeSet a;
  for (size_t i = 0; i < EdgeSet::kPromote * 2; ++i) a.insert(SlotId(i));
  EdgeSet b = a;  // copies past the promote point, so b rebuilds its own index
  assert(b.size() == a.size());
  assert(b.remove(SlotId(0)));
  assert(b.size() == a.size() - 1);
  assert(!a.insert(SlotId(0)));  // a is untouched
}

TEST(test_wide_fanout_dispose_and_recycle) {
  // End to end, above the promote threshold, with id recycling in the middle:
  // dispose half the subscribers (their ids go to the free list and are handed
  // to the replacements), then publish and check every survivor observed it.
  Context ctx;
  const size_t width = EdgeSet::kPromote * 3;
  auto topic = ctx.cell(0);
  std::vector<int> seen(width * 2, -1);
  std::vector<EffectHandle> handles;
  for (size_t i = 0; i < width; ++i)
    handles.push_back(ctx.effect_void(
        [&, i](Context& c) { seen[i] = c.get_cell(topic); }));

  for (size_t i = 0; i < width; i += 2) ctx.dispose_effect(handles[i]);
  for (size_t i = width; i < width + width / 2; ++i)
    handles.push_back(ctx.effect_void(
        [&, i](Context& c) { seen[i] = c.get_cell(topic); }));

  ctx.set_cell(topic, 42);
  for (size_t i = 1; i < width; i += 2) assert(seen[i] == 42);
  for (size_t i = width; i < width + width / 2; ++i) assert(seen[i] == 42);
  for (size_t i = 0; i < width; i += 2) assert(seen[i] == 0);  // disposed
}

TEST(test_wide_fanout_repeated_publish) {
  // Every publish tears down and re-registers every edge. Above the threshold
  // that is the indexed remove/insert pair; a lost or duplicated edge shows up
  // as a subscriber missing a later publish.
  Context ctx;
  const size_t width = EdgeSet::kPromote * 2 + 1;
  auto topic = ctx.cell(0);
  std::vector<int> seen(width, -1);
  std::vector<int> runs(width, 0);
  for (size_t i = 0; i < width; ++i)
    ctx.effect_void([&, i](Context& c) {
      seen[i] = c.get_cell(topic);
      runs[i]++;
    });
  for (int v = 1; v <= 5; ++v) ctx.set_cell(topic, v);
  for (size_t i = 0; i < width; ++i) {
    assert(seen[i] == 5);
    assert(runs[i] == 6);  // creation + 5 publishes, no duplicate edges
  }
}

TEST(test_wide_fanin_tracks_dynamic_dependencies) {
  // The mirror of wide fan-out: one slot reading many cells, so the indexed list
  // is the reader's own `dependencies`. Dependencies are re-registered on every
  // recompute, and a dynamic read set must still shrink correctly when indexed.
  Context ctx;
  const size_t n = EdgeSet::kPromote * 4;
  std::vector<CellHandle<int>> cells;
  for (size_t i = 0; i < n; ++i) cells.push_back(ctx.cell(1));
  auto use_all = ctx.cell(true);

  int computes = 0;
  auto sum = ctx.computed<long>([&](Context& c) {
    computes++;
    if (!c.get_cell(use_all)) return long(-1);
    long total = 0;
    for (auto& h : cells) total += c.get_cell(h);
    return total;
  });

  assert(ctx.get(sum) == long(n));
  ctx.set_cell(cells[n / 2], 3);
  assert(ctx.get(sum) == long(n) + 2);

  // Collapse the read set to a single cell; the other n edges must detach.
  ctx.set_cell(use_all, false);
  assert(ctx.get(sum) == -1);
  const int before = computes;
  // A cell that is no longer read must not invalidate the slot any more.
  ctx.set_cell(cells[0], 99);
  (void)ctx.get(sum);
  assert(computes == before);
}

TEST(test_pending_queue_matches_reference_model) {
  // Randomised differential against a straight vector implementing the same
  // contract: FIFO pop at the head, swap-with-back erase in the middle. Drives
  // the queue well past the promote threshold and back down so the scanned,
  // indexed and demoted regimes all run, and so the moved-id position fixup on
  // the indexed path is exercised thousands of times.
  PendingQueue q;
  std::vector<SlotId> model;
  uint64_t rng = 0x9E3779B97F4A7C15ULL;
  auto next = [&rng]() {
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng;
  };
  for (int step = 0; step < 200000; ++step) {
    const uint64_t r = next() % 100;
    if (r < 55) {  // push a fresh id (the bitset upstream keeps ids unique)
      SlotId id(next() % 4096);
      bool dup = false;
      for (auto& m : model) if (m == id) { dup = true; break; }
      if (dup) continue;
      q.push_back(id);
      model.push_back(id);
    } else if (r < 80) {  // pop head
      if (model.empty()) continue;
      SlotId got = q.pop_front();
      REQUIRE(got == model.front(), "pop_front returned the wrong id");
      model.erase(model.begin());
    } else {  // erase an arbitrary live id
      if (model.empty()) continue;
      const size_t i = size_t(next() % model.size());
      const SlotId id = model[i];
      q.erase(id);
      model[i] = model.back();
      model.pop_back();
    }
    REQUIRE(q.size() == model.size(), "size diverged from model");
  }
  // Drain and compare element-for-element.
  while (!model.empty()) {
    REQUIRE(q.pop_front() == model.front(), "drain order diverged from model");
    model.erase(model.begin());
  }
  REQUIRE(q.empty(), "queue must be empty when the model is");
}

// ── PendingQueue: the scheduler queue backing pending_effects_
// (#lzspecedgeindex) ──
//
// The shape that matters is disposing effects from INSIDE a flush, while the
// rest of a wide cascade is still queued. That is the only way pending depth
// reaches the fan-out width, and it is what made the old linear erase
// quadratic. Nothing else in this file builds a queue deeper than a couple of
// entries, which is why it hid.

TEST(test_dispose_from_inside_flush_runs_survivors_only) {
  Context ctx;
  auto topic = ctx.cell(0);
  const size_t n = 200;  // > PendingQueue promote, so the indexed path runs
  std::vector<int> ran(n, 0);
  std::vector<EffectHandle> effects;
  for (size_t i = 0; i < n; ++i)
    effects.push_back(
        ctx.effect_void([topic, i, &ran](Context& c) { c.get_cell(topic); ++ran[i]; }));

  // Registered last => scheduled first (the invalidation walk uses a stack), so
  // this runs with all n still queued. Disposes every even-indexed sibling.
  int runs = 0;
  ctx.effect_void([topic, &effects, &runs, n](Context& c) {
    c.get_cell(topic);
    if (++runs != 2) return;  // run 1 is the registration flush
    for (size_t i = 0; i < n; i += 2) c.dispose_effect(effects[i]);
  });

  for (auto& r : ran) r = 0;
  ctx.set_cell(topic, 1);

  for (size_t i = 0; i < n; ++i) {
    if (i % 2 == 0)
      REQUIRE(ran[i] == 0, "disposed effect must not run");
    else
      REQUIRE(ran[i] == 1, "survivor must run exactly once");
  }
}

TEST(test_dispose_from_inside_flush_survives_id_recycling) {
  // Disposing frees the id for immediate reuse, so a queue entry left behind by
  // a sloppy erase would fire the *new* effect. Create replacements during the
  // same flush and assert each runs exactly once.
  Context ctx;
  auto topic = ctx.cell(0);
  const size_t n = 128;
  std::vector<EffectHandle> effects;
  std::vector<int> ran(n, 0);
  for (size_t i = 0; i < n; ++i)
    effects.push_back(
        ctx.effect_void([topic, i, &ran](Context& c) { c.get_cell(topic); ++ran[i]; }));

  int fresh_runs = 0;
  int runs = 0;
  ctx.effect_void([topic, &effects, &runs, &fresh_runs, n](Context& c) {
    c.get_cell(topic);
    if (++runs != 2) return;
    for (size_t i = 0; i < n; ++i) c.dispose_effect(effects[i]);
    for (size_t i = 0; i < n; ++i)
      c.effect_void([topic, &fresh_runs](Context& cc) {
        cc.get_cell(topic);
        ++fresh_runs;
      });
  });

  for (auto& r : ran) r = 0;
  ctx.set_cell(topic, 1);

  for (size_t i = 0; i < n; ++i) REQUIRE(ran[i] == 0, "disposed must not run");
  // Each replacement runs once on creation; none may be run a second time by a
  // stale queue entry inherited from the id it recycled.
  REQUIRE(fresh_runs == int(n), "replacement effects must run exactly once");
}

int main() {
  std::cout << "lazily-cpp core tests: " << test_passed << "/" << test_count
            << " passed" << std::endl;
  return test_passed == test_count ? 0 : 1;
}
