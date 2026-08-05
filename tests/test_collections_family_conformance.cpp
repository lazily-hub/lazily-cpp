// The keyed-collection ordering contract replayed against **all three**
// execution flavors.
//
// `tests/test_collections.cpp` already replays the ordering fixtures, but only
// against the single-threaded `SourceMap`. That is the blind spot this file
// closes: for years the coverage matrix read OK because *a* flavor passed,
// while `ThreadSafeReactiveMap` and `AsyncReactiveMap` shipped no ordering
// surface at all. A contract the spec calls Core must be gated on every flavor
// or the gate is decorative.
//
// Three anti-green-washing properties, each of which has failed somewhere in
// this family before:
//
//   1. The fixture must exist. `spec_fixture_text` exits 77 (CTest SKIP) when
//      the sibling spec checkout is absent, so a missing corpus can never read
//      as a pass.
//   2. The fixture must have been *read*. `REQUIRE_FIXTURES_LOADED(2)` asserts
//      the exact distinct-fixture count, so short-circuiting a read turns the
//      suite red rather than quietly shrinking coverage.
//   3. The fixture must have *steps*. A zero-step replay is vacuous — every
//      assertion inside the loop is skipped and the test still reports green.
//
// And the `invalidates` matrix is read from `steps[].expected.invalidates`,
// where the fixtures actually nest it. lazily-rs read it off the step instead,
// so it was always absent and the invalidation assertion never ran once.

#include <lazily/lazily.hpp>

#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "test_assertion_keys.hpp"
#include "test_json.hpp"
#include "test_require.hpp"
#include "test_spec_fixture.hpp"

using namespace lazily;
using lazily_test::Json;
using lazily_test::JsonParser;
using lazily_test::JsonPtr;

namespace {

constexpr const char* kArea = "collections";

// An order-sensitive digest, so an order reader's *value* changes on a reorder
// and not merely its cache state.
int order_digest(const std::vector<std::string>& keys) {
  int acc = 17;
  for (const auto& k : keys) {
    for (char c : k)
      acc = acc * 31 + static_cast<int>(c);
    acc = acc * 31 + 7;
  }
  return acc;
}

// ---------------------------------------------------------------------------
// Single-threaded
// ---------------------------------------------------------------------------

struct SyncModel {
  static constexpr const char* kFlavor = "sync";

  Context ctx;
  SourceMap<std::string, int> map{ctx};

  void set_value(const std::string& k, int v) { map.set(ctx, k, v); }
  void insert(const std::string& k, int v) { map.entry(ctx, k, v); }
  void remove(const std::string& k) { map.remove(ctx, k); }
  void move_to(const std::string& k, std::size_t i) { map.move_to(ctx, k, i); }
  void move_before(const std::string& k, const std::string& a) { map.move_before(ctx, k, a); }
  void move_after(const std::string& k, const std::string& a) { map.move_after(ctx, k, a); }

  std::vector<std::string> keys_untracked() { return map.present_keys(); }
  std::optional<int> value_untracked(const std::string& k) {
    auto h = map.handle(k);
    if (!h) return std::nullopt;
    return ctx.get(*h);
  }
  std::optional<std::uint64_t> handle_id(const std::string& k) {
    auto h = map.handle(k);
    if (!h) return std::nullopt;
    return h->id().value;
  }

  Computed<int> value_reader(const std::string& k) {
    return ctx.computed<int>([this, k](Compute& c) { return map.get(c, k).value_or(-1); });
  }
  Computed<int> membership_reader() {
    return ctx.computed<int>([this](Compute& c) { return static_cast<int>(map.len(c)); });
  }
  Computed<int> order_reader() {
    return ctx.computed<int>([this](Compute& c) { return order_digest(map.keys(c)); });
  }

  void prime(const Computed<int>& r) { (void)ctx.get(r); }
  bool cached(const Computed<int>& r) { return ctx.is_set(r); }
};

// ---------------------------------------------------------------------------
// Thread-safe
// ---------------------------------------------------------------------------

struct ThreadSafeModel {
  static constexpr const char* kFlavor = "thread-safe";

  ThreadSafeContext ts;
  ThreadSafeSourceMap<std::string, int> map{ts};

  void set_value(const std::string& k, int v) { map.set(ts, k, v); }
  void insert(const std::string& k, int v) {
    map.get_or_insert_handle(ts, k, [v](const std::string&) { return v; });
  }
  void remove(const std::string& k) { map.remove(ts, k); }
  void move_to(const std::string& k, std::size_t i) { map.move_to(ts, k, i); }
  void move_before(const std::string& k, const std::string& a) { map.move_before(ts, k, a); }
  void move_after(const std::string& k, const std::string& a) { map.move_after(ts, k, a); }

  std::vector<std::string> keys_untracked() { return map.present_keys(); }
  std::optional<int> value_untracked(const std::string& k) {
    auto h = map.handle(k);
    if (!h) return std::nullopt;
    return ts.get(*h);
  }
  std::optional<std::uint64_t> handle_id(const std::string& k) {
    auto h = map.handle(k);
    if (!h) return std::nullopt;
    return h->id().value;
  }

  // Readers live on the graph the thread-safe context projects onto — the same
  // graph the map's membership and order cells were minted on.
  Computed<int> value_reader(const std::string& k) {
    return ts.context().computed<int>([this, k](Compute& c) {
      auto h = map.handle(k);
      return h ? c.get(*h) : -1;
    });
  }
  Computed<int> membership_reader() {
    return ts.context().computed<int>([this](Compute& c) { return static_cast<int>(map.len(c)); });
  }
  Computed<int> order_reader() {
    return ts.context().computed<int>([this](Compute& c) { return order_digest(map.keys(c)); });
  }

  void prime(const Computed<int>& r) { (void)ts.context().get(r); }
  bool cached(const Computed<int>& r) { return ts.context().is_set(r); }
};

// ---------------------------------------------------------------------------
// Async
// ---------------------------------------------------------------------------

struct AsyncModel {
  static constexpr const char* kFlavor = "async";

  AsyncContext actx;
  AsyncSourceMap<std::string, int> map{actx};

  void set_value(const std::string& k, int v) { map.set(actx, k, v); }
  void insert(const std::string& k, int v) {
    map.get_or_insert_handle(actx, k, [v](const std::string&) { return v; });
  }
  void remove(const std::string& k) { map.remove(actx, k); }
  void move_to(const std::string& k, std::size_t i) { map.move_to(actx, k, i); }
  void move_before(const std::string& k, const std::string& a) { map.move_before(actx, k, a); }
  void move_after(const std::string& k, const std::string& a) { map.move_after(actx, k, a); }

  std::vector<std::string> keys_untracked() { return map.present_keys(); }
  std::optional<int> value_untracked(const std::string& k) {
    auto h = map.handle(k);
    if (!h) return std::nullopt;
    return h->get();
  }
  std::optional<std::uint64_t> handle_id(const std::string& k) {
    auto h = map.handle(k);
    if (!h) return std::nullopt;
    return h->cell.id().value;
  }

  Computed<int> value_reader(const std::string& k) {
    return actx.context().computed<int>([this, k](Compute& c) {
      auto h = map.handle(k);
      return h ? c.get(h->cell) : -1;
    });
  }
  Computed<int> membership_reader() {
    return actx.context().computed<int>(
        [this](Compute& c) { return static_cast<int>(map.len(c)); });
  }
  Computed<int> order_reader() {
    return actx.context().computed<int>([this](Compute& c) { return order_digest(map.keys(c)); });
  }

  void prime(const Computed<int>& r) { (void)actx.context().get(r); }
  bool cached(const Computed<int>& r) { return actx.context().is_set(r); }
};

// ---------------------------------------------------------------------------
// Replay
// ---------------------------------------------------------------------------

std::vector<std::string> string_array(const Json* node) {
  std::vector<std::string> out;
  if (node == nullptr || !node->is_array()) return out;
  for (const auto& item : node->array)
    out.push_back(item->as_str());
  return out;
}

std::string join(const std::vector<std::string>& items) {
  std::string out;
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i != 0) out += ",";
    out += items[i];
  }
  return out;
}

template <typename Model> void run_fixture(const std::string& fixture) {
  const std::string flavor = Model::kFlavor;
  const std::string text = lazily_test::spec_fixture_text(kArea, fixture);
  JsonParser parser(text);
  JsonPtr root = parser.parse();
  REQUIRE(root && root->is_object(), flavor + ": fixture " + fixture + " is not a JSON object");

  Model model;

  // -- seed ---------------------------------------------------------------
  const Json* initial = root->find("initial");
  REQUIRE(initial != nullptr, flavor + ": fixture " + fixture + " has no initial state");
  const std::vector<std::string> seed_order = string_array(initial->find("order"));
  const Json* seed_values = initial->find("values");
  REQUIRE(!seed_order.empty(), flavor + ": fixture " + fixture + " seeds no keys");
  for (const auto& key : seed_order) {
    const Json* v = seed_values ? seed_values->find(key) : nullptr;
    REQUIRE(v != nullptr, flavor + ": no initial value for key " + key);
    model.insert(key, static_cast<int>(v->as_int()));
  }

  const Json* steps = root->find("steps");
  REQUIRE(steps != nullptr && steps->is_array(),
          flavor + ": fixture " + fixture + " has no steps array");
  // A zero-step replay asserts nothing while still reporting green.
  REQUIRE(!steps->array.empty(),
          flavor + ": fixture " + fixture + " has no steps - a vacuous replay would report green");

  std::size_t asserted_invalidations = 0;

  for (std::size_t i = 0; i < steps->array.size(); ++i) {
    const Json& step = *steps->array[i];
    const std::string where = flavor + " " + fixture + " step " + std::to_string(i);
    const Json* op = step.find("op");
    REQUIRE(op != nullptr, where + ": missing op");
    const Json* expected_block = step.find("expected");
    REQUIRE(expected_block != nullptr, where + ": missing expected");
    lazily_test::AssertionKeys expected(where + " expected", *expected_block);

    // Rebuild + prime readers from the CURRENT key set, so each step's
    // invalidation is measured against a fully settled graph rather than
    // inheriting staleness from the previous step.
    const std::vector<std::string> before_keys = model.keys_untracked();
    std::map<std::string, Computed<int>> value_readers;
    for (const auto& key : before_keys)
      value_readers.emplace(key, model.value_reader(key));
    Computed<int> membership = model.membership_reader();
    Computed<int> order = model.order_reader();
    for (auto& kv : value_readers)
      model.prime(kv.second);
    model.prime(membership);
    model.prime(order);
    // A reader that failed to prime is trivially "invalidated" by the next op,
    // which would let a broken flavor pass the dirty-side assertions.
    for (auto& kv : value_readers)
      REQUIRE(model.cached(kv.second),
              where + ": value reader for " + kv.first + " failed to prime");
    REQUIRE(model.cached(membership), where + ": membership reader failed to prime");
    REQUIRE(model.cached(order), where + ": order reader failed to prime");

    // Handle identity before the op, to check `handle_stable`.
    std::map<std::string, std::optional<std::uint64_t>> handles_before;
    for (const auto& key : before_keys)
      handles_before.emplace(key, model.handle_id(key));

    // -- apply ------------------------------------------------------------
    const std::string type = op->find("type")->as_str();
    if (type == "set_value") {
      model.set_value(op->find("key")->as_str(), static_cast<int>(op->find("value")->as_int()));
    } else if (type == "insert") {
      const std::string key = op->find("key")->as_str();
      model.insert(key, static_cast<int>(op->find("value")->as_int()));
      // `at` says where the new key lands. Minting appends, so "end" is already
      // right; anything else is an explicit placement. An unrecognised form must
      // fail rather than silently append and then assert the wrong order.
      if (const Json* at = op->find("at")) {
        if (at->type == Json::Type::Number) {
          model.move_to(key, static_cast<std::size_t>(at->as_int()));
        } else {
          REQUIRE(at->as_str() == "end",
                  where + ": unsupported insert placement `" + at->as_str() + "`");
        }
      }
    } else if (type == "remove") {
      model.remove(op->find("key")->as_str());
    } else if (type == "move_to") {
      model.move_to(op->find("key")->as_str(),
                    static_cast<std::size_t>(op->find("index")->as_int()));
    } else if (type == "move_before") {
      model.move_before(op->find("key")->as_str(), op->find("before")->as_str());
    } else if (type == "move_after") {
      model.move_after(op->find("key")->as_str(), op->find("after")->as_str());
    } else {
      REQUIRE(false, where + ": unsupported op type " + type +
                         " - an unknown op must fail, never silently skip");
    }

    // -- order + membership -----------------------------------------------
    const std::vector<std::string> got_order = model.keys_untracked();
    expected.assert_key_with("order", [&](const Json& want) {
      const std::vector<std::string> want_order = string_array(&want);
      REQUIRE(want_order == got_order,
              where + ": order is [" + join(got_order) + "], expected [" + join(want_order) + "]");
      return true;
    });

    expected.assert_key_with_if_present("membership", [&](const Json& want_membership) {
      // Bind the vector: iterators taken from two separate temporaries
      // would belong to different containers.
      const std::vector<std::string> want_keys = string_array(&want_membership);
      const std::set<std::string> want(want_keys.begin(), want_keys.end());
      const std::set<std::string> got(got_order.begin(), got_order.end());
      REQUIRE(want == got, where + ": membership set diverged");
      return true;
    });

    // -- values -------------------------------------------------------------
    //
    // Descended into rather than walked by hand (`#lzsubblockkeyset`): the child
    // tracker owns each entry name, so a value the fixture grows is compared
    // rather than skipped.
    expected.with_sub_if_present("values", [&](lazily_test::AssertionKeys& values) {
      for (const auto& name : values.keys()) {
        values.assert_key_with(name, [&](const Json& want) {
          auto got = model.value_untracked(name);
          REQUIRE(got.has_value(), where + ": value for " + name + " is absent");
          REQUIRE(*got == static_cast<int>(want.as_int()),
                  where + ": value for " + name + " is " + std::to_string(*got) + ", expected " +
                      std::to_string(want.as_int()));
          return true;
        });
      }
    });

    // -- the invalidation matrix -------------------------------------------
    //
    // Nested under `expected`, which is where the fixtures actually put it. A
    // reader name the matrix grows fails the child tracker's own finish rather
    // than falling past the three arms this site knows (`#lzsubblockkeyset`).
    expected.with_sub("invalidates", [&](lazily_test::AssertionKeys& invalidates) {
      ++asserted_invalidations;
      // Only survivors are checked: a key this op removed has no entry left to
      // read, and a key it added had no reader to invalidate.
      const auto check_value_readers = [&](const std::set<std::string>& dirty) {
        const std::set<std::string> survivors(got_order.begin(), got_order.end());
        for (auto& kv : value_readers) {
          if (survivors.count(kv.first) == 0) continue;
          const bool want_dirty = dirty.count(kv.first) > 0;
          const bool still_cached = model.cached(kv.second);
          if (want_dirty) {
            REQUIRE(!still_cached,
                    where + ": value reader for " + kv.first + " should have been invalidated");
          } else {
            REQUIRE(still_cached, where + ": value reader for " + kv.first +
                                      " should have stayed cached - per-entry "
                                      "independence is the whole point");
          }
        }
      };
      const bool had_value = invalidates.assert_key_with_if_present("value", [&](const Json& want) {
        const std::vector<std::string> dirty_values = string_array(&want);
        check_value_readers(std::set<std::string>(dirty_values.begin(), dirty_values.end()));
        return true;
      });
      if (!had_value) check_value_readers({});

      invalidates.assert_key_with_if_present("membership", [&](const Json& want) {
        const bool want_dirty = want.as_bool();
        REQUIRE(model.cached(membership) != want_dirty,
                where + std::string(": membership reader should have ") +
                    (want_dirty ? "been invalidated" : "stayed cached") +
                    " - a pure reorder must NOT invalidate set-identity "
                    "readers");
        return true;
      });

      invalidates.assert_key_with_if_present("order", [&](const Json& want) {
        const bool want_dirty = want.as_bool();
        REQUIRE(model.cached(order) != want_dirty,
                where + std::string(": order reader should have ") +
                    (want_dirty ? "been invalidated" : "stayed cached"));
        return true;
      });
    });

    // -- handle stability ---------------------------------------------------
    //
    // The law that separates an atomic move from a remove + re-mint: a reorder
    // keeps the entry's node, so its dependents and CRDT lineage survive.
    expected.with_sub_if_present("handle_stable", [&](lazily_test::AssertionKeys& stable) {
      for (const auto& name : stable.keys()) {
        stable.assert_key_with(name, [&](const Json& want) {
          auto before = handles_before.count(name) ? handles_before[name] : std::nullopt;
          auto after = model.handle_id(name);
          if (want.as_bool()) {
            REQUIRE(before.has_value() && after.has_value() && *before == *after,
                    where + ": handle for " + name +
                        " must survive the move - a reorder that re-mints is a "
                        "remove + insert, not a move");
          } else {
            REQUIRE(!after.has_value() || !before.has_value() || *before != *after,
                    where + ": handle for " + name + " should have changed");
          }
          return true;
        });
      }
    });
  }

  // The matrix is the contract. A fixture whose `invalidates` block never
  // resolved means the assertion silently never ran - exactly the lazily-rs
  // defect this file exists to prevent recurring.
  REQUIRE(asserted_invalidations > 0,
          flavor + ": fixture " + fixture +
              " asserted no invalidation matrix - the reader-independence "
              "contract was never checked");

  std::cout << "ok  " << flavor << "  " << fixture << "  (" << steps->array.size() << " steps, "
            << asserted_invalidations << " invalidation matrices)" << std::endl;
}

void run_dependency_fixture() {
  const std::string fixture = "dependency_reactive_availability.json";
  const std::string text = lazily_test::spec_fixture_text(kArea, fixture);
  JsonParser parser(text);
  JsonPtr root = parser.parse();
  REQUIRE(root && root->is_object(), fixture + ": not a JSON object");
  const Json* key_json = root->find("key");
  const Json* steps = root->find("steps");
  REQUIRE(key_json != nullptr && steps != nullptr && steps->is_array(),
          fixture + ": missing key or steps");
  REQUIRE(!steps->array.empty(), fixture + ": a zero-step replay is vacuous");

  Context ctx;
  DependencyMap<std::string, int> map{ctx};
  const std::string key = key_json->as_str();
  std::size_t recomputes = 0;
  auto reader = ctx.computed<DependencyAvailability<int>>([&](Compute& compute) {
    ++recomputes;
    return map.observe_dependency(compute, key);
  });
  std::optional<std::uint64_t> identity;

  for (std::size_t i = 0; i < steps->array.size(); ++i) {
    const Json& step = *steps->array[i];
    const Json* op = step.find("op");
    const Json* expected_json = step.find("expected");
    REQUIRE(op != nullptr && expected_json != nullptr,
            fixture + " step " + std::to_string(i) + ": missing op/expected");
    const std::string type = op->find("type")->as_str();
    if (type == "observe_dependency") {
      (void)ctx.get(reader);
    } else if (type == "publish") {
      map.publish(ctx, op->find("key")->as_str(), static_cast<int>(op->find("value")->as_int()));
    } else if (type == "unpublish") {
      map.unpublish(ctx, op->find("key")->as_str());
    } else {
      REQUIRE(false, fixture + " step " + std::to_string(i) + ": unsupported op " + type);
    }

    const auto state = ctx.get(reader);
    lazily_test::AssertionKeys expected(fixture + " step " + std::to_string(i), *expected_json);
    if (state.value.has_value()) {
      expected.with_sub("state", [&](lazily_test::AssertionKeys& available) {
        available.assert_key("Available", *state.value);
      });
    } else {
      expected.assert_key("state", std::string("Unavailable"));
    }
    expected.assert_key("recomputes", static_cast<long long>(recomputes));
    expected.assert_key("present_count", static_cast<long long>(map.present_count()));
    const auto handle = map.handle(key);
    REQUIRE(handle.has_value(), fixture + ": exact-key source disappeared");
    identity = identity.value_or(handle->id().value);
    REQUIRE(*identity == handle->id().value, fixture + ": exact-key source identity changed");
    expected.assert_key("identity", std::string("wanted-1"));
    expected.finish();
  }

  std::cout << "ok  sync  " << fixture << "  (" << steps->array.size() << " steps)" << std::endl;
}

// ---------------------------------------------------------------------------
// Directional move coverage the canonical corpus does not provide
// ---------------------------------------------------------------------------
//
// `cellmap_atomic_move.json`'s only `move_before` step moves a key that sits
// AFTER its anchor (`from=2`, `anchor=0`), so it exercises only the branch where
// the insertion point is the anchor index itself. The other branch — a key that
// currently PRECEDES its anchor, where lifting it out shifts the anchor one slot
// left and the target must therefore be `anchor - 1` — is never replayed.
//
// That is not a hypothetical hole. It is precisely the direction in which
// lazily-zig's `moveBefore` was wrong: `moveBefore("a", "d")` on `[a,b,c,d]`
// produced `[b,c,d,a]` instead of `[b,c,a,d]`. The canonical corpus would have
// scored that binding green. Until the corpus covers both directions, each
// binding has to cover them itself.
template <typename Model> void check_directional_moves() {
  const std::string flavor = Model::kFlavor;
  const std::vector<std::string> seed = {"a", "b", "c", "d"};

  auto build = [&](Model& m) {
    int v = 1;
    for (const auto& k : seed)
      m.insert(k, v++);
  };

  { // key precedes anchor: the `anchor - 1` branch.
    Model m;
    build(m);
    const auto before_id = m.handle_id("a");
    m.move_before("a", "d");
    const std::vector<std::string> want = {"b", "c", "a", "d"};
    REQUIRE(m.keys_untracked() == want,
            flavor + ": move_before(a, d) on [a,b,c,d] gave [" + join(m.keys_untracked()) +
                "], expected [b,c,a,d] - the target must be computed on the "
                "pre-removal list");
    REQUIRE(before_id.has_value() && m.handle_id("a") == before_id,
            flavor + ": move_before must keep the entry's node");
  }
  { // key follows anchor: the `anchor` branch.
    Model m;
    build(m);
    m.move_before("d", "b");
    const std::vector<std::string> want = {"a", "d", "b", "c"};
    REQUIRE(m.keys_untracked() == want, flavor + ": move_before(d, b) gave [" +
                                            join(m.keys_untracked()) + "], expected [a,d,b,c]");
  }
  { // move_after, key precedes anchor.
    Model m;
    build(m);
    m.move_after("a", "c");
    const std::vector<std::string> want = {"b", "c", "a", "d"};
    REQUIRE(m.keys_untracked() == want, flavor + ": move_after(a, c) gave [" +
                                            join(m.keys_untracked()) + "], expected [b,c,a,d]");
  }
  { // move_after, key follows anchor.
    Model m;
    build(m);
    m.move_after("d", "a");
    const std::vector<std::string> want = {"a", "d", "b", "c"};
    REQUIRE(m.keys_untracked() == want, flavor + ": move_after(d, a) gave [" +
                                            join(m.keys_untracked()) + "], expected [a,d,b,c]");
  }
  { // out-of-range index clamps rather than desyncing entries from order.
    Model m;
    build(m);
    m.move_to("a", 99);
    const std::vector<std::string> want = {"b", "c", "d", "a"};
    REQUIRE(m.keys_untracked() == want,
            flavor + ": move_to past the end must clamp, gave [" + join(m.keys_untracked()) + "]");
  }
  { // a move naming an absent key must not mutate the order.
    Model m;
    build(m);
    m.move_before("zz", "a");
    m.move_to("zz", 0);
    REQUIRE(m.keys_untracked() == seed, flavor + ": a move on an absent key must be a no-op");
  }

  std::cout << "ok  " << flavor << "  directional move coverage" << std::endl;
}

} // namespace

int main() {
  for (const char* fixture : {"cellmap_atomic_move.json", "cellmap_independence.json"}) {
    run_fixture<SyncModel>(fixture);
    run_fixture<ThreadSafeModel>(fixture);
    run_fixture<AsyncModel>(fixture);
  }

  run_dependency_fixture();

  check_directional_moves<SyncModel>();
  check_directional_moves<ThreadSafeModel>();
  check_directional_moves<AsyncModel>();

  // All three canonical fixtures, read for real.
  REQUIRE_FIXTURES_LOADED(3);
  std::cout << "collections family conformance: 2 ordering fixtures x 3 flavors + dependency"
            << std::endl;
  return 0;
}
