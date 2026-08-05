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

#include "test_assertion_keys.hpp"
#include "test_json.hpp"
#include "test_spec_fixture.hpp"

using namespace lazily;
using lazily_test::Json;

static int test_count = 0;
static int test_passed = 0;

#define TEST(name)                                                                                 \
  static void name();                                                                              \
  struct name##_runner {                                                                           \
    name##_runner() {                                                                              \
      ++test_count;                                                                                \
      name();                                                                                      \
      ++test_passed;                                                                               \
    }                                                                                              \
  } name##_instance;                                                                               \
  static void name()

static TopicDurability durability_of(const std::string& s) {
  if (s == "ephemeral") return TopicDurability::Ephemeral;
  REQUIRE(s == "durable", "unknown topic durability");
  return TopicDurability::Durable;
}

static std::vector<std::string> str_array(const Json* node) {
  std::vector<std::string> out;
  if (node)
    for (const auto& e : node->array)
      out.push_back(e->str);
  return out;
}

template <typename OwnerContext, typename Topic> static void run_fixture(const std::string& name) {
  const std::string text = lazily_test::spec_fixture_text("collections", name);
  REQUIRE(text.find("\"TopicCell\"") != std::string::npos, "fixture is not a TopicCell corpus");
  auto doc = lazily_test::parse_json(text);

  OwnerContext ctx;
  Context& graph = queue_detail::graph(ctx);

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
  Topic topic(ctx, snap);

  // Per-subscriber invalidation probes: a `computed` over the reader cell.
  std::map<std::string, Computed<std::vector<std::string>>> probes;
  auto ensure_probe = [&](const std::string& id) {
    if (probes.count(id)) return;
    auto reader = topic.reader_handle(ctx, id);
    graph.get(reader); // prime -> is_set true
    probes.insert({id, reader});
  };
  for (const auto& id : all_ids)
    ensure_probe(id);

  const Json* steps = doc->find("steps");
  REQUIRE(steps != nullptr, "topic fixture missing steps");
  size_t step_idx = 0;
  for (const auto& step : steps->array) {
    const std::string tag = name + " step " + std::to_string(step_idx++);
    const Json* op = step->find("op");
    const std::string& type = op->find("type")->str;
    auto subscriber = [&]() { return op->find("subscriber")->str; };

    // A subscribe step asserts invalidation for the identity it creates. Mint
    // that reader before the subscription so the assertion measures a real
    // set -> unset transition instead of being skipped for lack of a probe.
    if (type == "subscribe") ensure_probe(subscriber());

    // Prime every probe so `is_set` starts true.
    std::set<std::string> existed_before;
    for (auto& kv : probes) {
      graph.get(kv.second);
      existed_before.insert(kv.first);
    }

    if (type == "publish") {
      topic.publish(ctx, op->find("value")->str);
    } else if (type == "advance") {
      topic.advance(ctx, subscriber());
    } else if (type == "subscribe") {
      // `durability` is the discriminator the ephemeral-lifecycle fixture
      // exists to pin. An ABSENT key used to default to Durable, so a renamed
      // key would have replayed the ephemeral start-at-tail/auto-removal
      // scenario as a durable-replay one and stayed green. `durability_of`
      // already hard-fails on a bad string; absence is now a hard failure too.
      const Json* dur = op->find("durability");
      REQUIRE(dur != nullptr && dur->type == Json::Type::String,
              ("subscribe op has no durability: " + tag).c_str());
      topic.subscribe(ctx, subscriber(), durability_of(dur->str));
      all_ids.insert(subscriber());
    } else if (type == "disconnect") {
      topic.disconnect(ctx, subscriber());
    } else if (type == "reconnect") {
      topic.reconnect(ctx, subscriber());
    } else if (type == "gc") {
      topic.gc();
    } else if (type == "restart") {
      topic.restart(); // no-op in cpp; snapshot round-trip is state-preserving
    } else {
      REQUIRE(false, ("unknown topic op: " + type).c_str());
    }

    const Json* expected = step->find("expected");
    REQUIRE(expected != nullptr, "topic step missing expected");
    lazily_test::AssertionKeys expect("collections/" + name + " " + tag + " expected", *expected);

    // Per-subscriber reader invalidation.
    expect.with_sub_if_present("invalidates", [&](lazily_test::AssertionKeys& invalidates) {
      for (const auto& id : invalidates.keys()) {
        REQUIRE(existed_before.count(id) != 0,
                ("invalidation assertion has no primed probe: " + tag + " sub=" + id).c_str());
        const bool actual = !graph.is_set(probes.at(id));
        invalidates.assert_key(id, actual);
      }
    });

    // read_stream per subscriber.
    expect.with_sub_if_present("reads", [&](lazily_test::AssertionKeys& reads) {
      for (const auto& id : reads.keys()) {
        const auto got = topic.read_stream(ctx, id);
        reads.assert_key(id, got, [](const Json& want) { return str_array(&want); });
      }
    });

    // Topic state: base_offset + elements.
    expect.assert_key("base_offset", topic.base_offset(),
                      [](const Json& want) { return static_cast<size_t>(want.as_int()); });
    expect.assert_key("elements", topic.elements(),
                      [](const Json& want) { return str_array(&want); });

    // Subscriptions: every expected entry matches; any known id absent from
    // `expected` must be gone from the topic (removed ephemeral).
    std::set<std::string> expected_ids;
    expect.with_sub("subscriptions", [&](lazily_test::AssertionKeys& subscriptions) {
      for (const auto& id : subscriptions.keys()) {
        expected_ids.insert(id);
        auto got = topic.subscription(id);
        REQUIRE(got.has_value(), ("subscription missing: " + tag + " sub=" + id).c_str());
        subscriptions.with_sub(id, [&](lazily_test::AssertionKeys& subscription) {
          subscription.assert_key("cursor", got->cursor, [](const Json& want) {
            return static_cast<size_t>(want.as_int());
          });
          subscription.assert_key("connected", got->connected);
          subscription.assert_key("durability", got->durability,
                                  [](const Json& want) { return durability_of(want.str); });
        });
      }
    });
    for (const auto& id : all_ids)
      if (!expected_ids.count(id))
        REQUIRE(!topic.subscription(id).has_value(),
                ("removed subscription still present: " + tag + " sub=" + id).c_str());

    // Drop probes for removed ephemeral identities before rebuilding. A later
    // subscription with the same id owns a newly-minted reader-kind node.
    for (auto it = probes.begin(); it != probes.end();) {
      if (!expected_ids.count(it->first))
        it = probes.erase(it);
      else
        ++it;
    }
    // Rebuild probes for every currently-present subscriber for the next step.
    for (const auto& id : expected_ids)
      ensure_probe(id);
  }
}

TEST(conformance_topiccell_broadcast_cursor_isolation) {
  run_fixture<Context, TopicCell<std::string>>("topiccell_broadcast_cursor_isolation.json");
  run_fixture<ThreadSafeContext, ThreadSafeTopicCell<std::string>>(
      "topiccell_broadcast_cursor_isolation.json");
  run_fixture<AsyncContext, AsyncTopicCell<std::string>>(
      "topiccell_broadcast_cursor_isolation.json");
}
TEST(conformance_topiccell_durable_replay_gc) {
  run_fixture<Context, TopicCell<std::string>>("topiccell_durable_replay_gc.json");
  run_fixture<ThreadSafeContext, ThreadSafeTopicCell<std::string>>(
      "topiccell_durable_replay_gc.json");
  run_fixture<AsyncContext, AsyncTopicCell<std::string>>("topiccell_durable_replay_gc.json");
}
TEST(conformance_topiccell_ephemeral_lifecycle) {
  run_fixture<Context, TopicCell<std::string>>("topiccell_ephemeral_lifecycle.json");
  run_fixture<ThreadSafeContext, ThreadSafeTopicCell<std::string>>(
      "topiccell_ephemeral_lifecycle.json");
  run_fixture<AsyncContext, AsyncTopicCell<std::string>>("topiccell_ephemeral_lifecycle.json");
}
TEST(conformance_topiccell_offline_tail_bounds) {
  run_fixture<Context, TopicCell<std::string>>("topiccell_offline_tail_bounds.json");
  run_fixture<ThreadSafeContext, ThreadSafeTopicCell<std::string>>(
      "topiccell_offline_tail_bounds.json");
  run_fixture<AsyncContext, AsyncTopicCell<std::string>>("topiccell_offline_tail_bounds.json");
}

int main() {
  REQUIRE_FIXTURES_LOADED(4);
  std::cout << "lazily-cpp topic conformance: 4 canonical fixtures x 3 "
               "flavors, "
            << test_passed << "/" << test_count << " groups passed" << std::endl;
  return test_passed == test_count ? 0 : 1;
}
