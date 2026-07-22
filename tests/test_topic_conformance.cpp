// TopicCell broadcast/cursor/retention conformance (`#lztopiccell`). A C++17
// port of `lazily-rs/tests/topic_conformance.rs`, generically replaying the
// canonical `lazily-spec/conformance/collections/topiccell_*.json` corpora
// through `lazily::TopicCell`: fan-out delivery, per-subscriber cursor
// isolation, durable offline replay + slowest-cursor GC, ephemeral
// start-at-tail lifecycle, and per-subscriber reader-invalidation independence.
//
// Each fixture is `{initial, steps:[{op, expected}]}`. For every step we apply
// the op and assert the full topic state (base_offset / elements /
// subscriptions), each subscriber's read_stream, and per-subscriber reader
// invalidation (observed by wrapping each subscriber's reader cell in a
// `computed`, priming it before the op, and checking `is_set` after — exactly
// the temporal-conformance invalidation-observation idiom). Fixture bytes are
// read from the sibling lazily-spec checkout; REQUIRE_FIXTURES_LOADED(4) proves
// the corpus actually ran.

#include <lazily/queue.hpp>

#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "test_json.hpp"
#include "test_spec_fixture.hpp"

using namespace lazily;
using lazily_test::Json;

static int test_count = 0;
static int test_passed = 0;

#define TEST(name)                 \
  static void name();              \
  struct name##_runner {           \
    name##_runner() {              \
      ++test_count;                \
      name();                      \
      ++test_passed;               \
    }                              \
  } name##_instance;               \
  static void name()

static TopicDurability durability_of(const std::string& s) {
  if (s == "ephemeral") return TopicDurability::Ephemeral;
  REQUIRE(s == "durable", "unknown topic durability");
  return TopicDurability::Durable;
}

static std::vector<std::string> str_array(const Json* node) {
  std::vector<std::string> out;
  if (node)
    for (const auto& e : node->array) out.push_back(e->str);
  return out;
}

static void run_fixture(const std::string& name) {
  const std::string text = lazily_test::spec_fixture_text("collections", name);
  REQUIRE(text.find("\"TopicCell\"") != std::string::npos,
          "fixture is not a TopicCell corpus");
  auto doc = lazily_test::parse_json(text);

  Context ctx;

  // Build the initial snapshot.
  const Json* init = doc->find("initial");
  REQUIRE(init != nullptr, "topic fixture missing initial");
  TopicSnapshot<std::string> snap;
  snap.base_offset = static_cast<size_t>(init->find("base_offset")->as_int());
  snap.elements = str_array(init->find("elements"));
  std::set<std::string> all_ids;
  for (const auto& kv : init->find("subscriptions")->object) {
    const Json* s = kv.second.get();
    snap.subscriptions.push_back(TopicSubscriptionSnapshot{
        kv.first, static_cast<size_t>(s->find("cursor")->as_int()),
        durability_of(s->find("durability")->str), s->find("connected")->as_bool()});
    all_ids.insert(kv.first);
  }
  TopicCell<std::string> topic(ctx, snap);

  // Per-subscriber invalidation probes: a `computed` over the reader cell.
  std::map<std::string, Computed<std::uint64_t>> probes;
  auto ensure_probe = [&](const std::string& id) {
    if (probes.count(id)) return;
    auto reader = topic.reader_handle(ctx, id);
    auto probe = ctx.computed<std::uint64_t>(
        [reader](Context& c) { return c.get(reader); });
    ctx.get(probe);  // prime -> is_set true
    probes.insert({id, probe});
  };
  for (const auto& id : all_ids) ensure_probe(id);

  const Json* steps = doc->find("steps");
  REQUIRE(steps != nullptr, "topic fixture missing steps");
  size_t step_idx = 0;
  for (const auto& step : steps->array) {
    const std::string tag = name + " step " + std::to_string(step_idx++);

    // Prime every existing probe so `is_set` starts true; record which existed
    // before this op (a subscriber created by THIS op has no before-state).
    std::set<std::string> existed_before;
    for (auto& kv : probes) {
      ctx.get(kv.second);
      existed_before.insert(kv.first);
    }

    const Json* op = step->find("op");
    const std::string& type = op->find("type")->str;
    auto subscriber = [&]() { return op->find("subscriber")->str; };
    if (type == "publish") {
      topic.publish(ctx, op->find("value")->str);
    } else if (type == "advance") {
      topic.advance(ctx, subscriber());
    } else if (type == "subscribe") {
      const Json* dur = op->find("durability");
      topic.subscribe(ctx, subscriber(),
                      dur ? durability_of(dur->str) : TopicDurability::Durable);
      all_ids.insert(subscriber());
    } else if (type == "disconnect") {
      topic.disconnect(ctx, subscriber());
    } else if (type == "reconnect") {
      topic.reconnect(ctx, subscriber());
    } else if (type == "gc") {
      topic.gc();
    } else if (type == "restart") {
      topic.restart();  // no-op in cpp; snapshot round-trip is state-preserving
    } else {
      REQUIRE(false, ("unknown topic op: " + type).c_str());
    }

    const Json* expected = step->find("expected");
    REQUIRE(expected != nullptr, "topic step missing expected");

    // Per-subscriber reader invalidation.
    if (const Json* inval = expected->find("invalidates")) {
      for (const auto& kv : inval->object) {
        const std::string& id = kv.first;
        const bool want = kv.second->as_bool();
        if (existed_before.count(id)) {
          const bool still_set = ctx.is_set(probes.at(id));
          REQUIRE(still_set == !want,
                  ("invalidation mismatch: " + tag + " sub=" + id).c_str());
        }
        // A subscriber created by this op has no before-state to transition
        // from; its probe is (re)built below for subsequent steps.
      }
    }

    // read_stream per subscriber.
    if (const Json* reads = expected->find("reads")) {
      for (const auto& kv : reads->object) {
        auto got = topic.read_stream(ctx, kv.first);
        REQUIRE(got == str_array(kv.second.get()),
                ("read_stream mismatch: " + tag + " sub=" + kv.first).c_str());
      }
    }

    // Topic state: base_offset + elements.
    REQUIRE(topic.base_offset() == static_cast<size_t>(expected->find("base_offset")->as_int()),
            ("base_offset mismatch: " + tag).c_str());
    REQUIRE(topic.elements() == str_array(expected->find("elements")),
            ("elements mismatch: " + tag).c_str());

    // Subscriptions: every expected entry matches; any known id absent from
    // `expected` must be gone from the topic (removed ephemeral).
    const Json* subs = expected->find("subscriptions");
    std::set<std::string> expected_ids;
    for (const auto& kv : subs->object) {
      expected_ids.insert(kv.first);
      auto got = topic.subscription(kv.first);
      REQUIRE(got.has_value(), ("subscription missing: " + tag + " sub=" + kv.first).c_str());
      const Json* e = kv.second.get();
      REQUIRE(got->cursor == static_cast<size_t>(e->find("cursor")->as_int()),
              ("cursor mismatch: " + tag + " sub=" + kv.first).c_str());
      REQUIRE(got->connected == e->find("connected")->as_bool(),
              ("connected mismatch: " + tag + " sub=" + kv.first).c_str());
      REQUIRE(got->durability == durability_of(e->find("durability")->str),
              ("durability mismatch: " + tag + " sub=" + kv.first).c_str());
    }
    for (const auto& id : all_ids)
      if (!expected_ids.count(id))
        REQUIRE(!topic.subscription(id).has_value(),
                ("removed subscription still present: " + tag + " sub=" + id).c_str());

    // Rebuild probes for every currently-present subscriber for the next step.
    for (const auto& id : expected_ids) ensure_probe(id);
  }
}

TEST(conformance_topiccell_broadcast_cursor_isolation) {
  run_fixture("topiccell_broadcast_cursor_isolation.json");
}
TEST(conformance_topiccell_durable_replay_gc) {
  run_fixture("topiccell_durable_replay_gc.json");
}
TEST(conformance_topiccell_ephemeral_lifecycle) {
  run_fixture("topiccell_ephemeral_lifecycle.json");
}
TEST(conformance_topiccell_offline_tail_bounds) {
  run_fixture("topiccell_offline_tail_bounds.json");
}

int main() {
  REQUIRE_FIXTURES_LOADED(4);
  std::cout << "lazily-cpp topic conformance: " << test_passed << "/" << test_count
            << " passed" << std::endl;
  return test_passed == test_count ? 0 : 1;
}
