// Canonical WorkQueueCell replay across sync, thread-safe, and async flavors.
// Every flavor opens the same two lazily-spec fixtures and asserts both the
// state machine and the reader-kind invalidation matrix nested under
// steps[].expected.invalidates.

#include <lazily/lazily.hpp>

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "test_assertion_keys.hpp"
#include "test_json.hpp"
#include "test_require.hpp"
#include "test_spec_fixture.hpp"

using namespace lazily;
using lazily_test::Json;

namespace {

template <typename Queue> struct Readers {
  Context& graph;
  WorkQueueReaderHandles handles;

  Readers(Context& context, Queue& queue) : graph(context), handles(queue.reader_handles()) {
    refresh();
  }

  void refresh() {
    (void)graph.get(handles.pending_len);
    (void)graph.get(handles.is_empty);
    (void)graph.get(handles.in_flight_len);
    (void)graph.get(handles.dead_letter_len);
  }

  void assert_invalidates(lazily_test::AssertionKeys& expected, const std::string& where) {
    expected.assert_key_with("invalidates", [&](const Json& inv) {
      const struct {
        const char* name;
        bool cached;
      } actual[] = {
          {"pending_len", graph.is_set(handles.pending_len)},
          {"is_empty", graph.is_set(handles.is_empty)},
          {"in_flight_len", graph.is_set(handles.in_flight_len)},
          {"dead_letter_len", graph.is_set(handles.dead_letter_len)},
      };
      for (const auto& reader : actual) {
        const Json* want = inv.find(reader.name);
        REQUIRE(want != nullptr, where + ": invalidates." + reader.name + " is absent");
        REQUIRE((!reader.cached) == want->as_bool(),
                where + ": invalidates." + reader.name + " mismatch");
      }
      return true;
    });
  }
};

void assert_delivery(const WorkQueueDelivery<std::string>& got, const Json& want,
                     const std::string& where) {
  REQUIRE(got.delivery_id == static_cast<std::uint64_t>(want.find("delivery_id")->as_int()),
          where + ": delivery_id mismatch");
  REQUIRE(got.item_id == static_cast<std::uint64_t>(want.find("item_id")->as_int()),
          where + ": item_id mismatch");
  REQUIRE(got.value == want.find("value")->as_str(), where + ": delivery value mismatch");
  REQUIRE(got.worker == want.find("worker")->as_str(), where + ": delivery worker mismatch");
  REQUIRE(got.attempt == static_cast<std::uint64_t>(want.find("attempt")->as_int()),
          where + ": delivery attempt mismatch");
  REQUIRE(got.deadline == static_cast<std::uint64_t>(want.find("deadline")->as_int()),
          where + ": delivery deadline mismatch");
}

template <typename Queue>
void assert_state(Queue& queue, lazily_test::AssertionKeys& expected, const std::string& where) {
  expected.assert_key_with("pending", [&](const Json& pending) {
    const auto got_pending = queue.pending_items();
    REQUIRE(got_pending.size() == pending.array.size(), where + ": pending length mismatch");
    for (std::size_t i = 0; i < got_pending.size(); ++i) {
      const Json& want = *pending.array[i];
      REQUIRE(got_pending[i].item_id == static_cast<std::uint64_t>(want.find("item_id")->as_int()),
              where + ": pending item_id mismatch");
      REQUIRE(got_pending[i].value == want.find("value")->as_str(),
              where + ": pending value mismatch");
      REQUIRE(got_pending[i].attempts ==
                  static_cast<std::uint64_t>(want.find("attempts")->as_int()),
              where + ": pending attempts mismatch");
    }
    return true;
  });

  expected.assert_key_with("in_flight", [&](const Json& in_flight) {
    const auto got_in_flight = queue.in_flight_deliveries();
    REQUIRE(got_in_flight.size() == in_flight.array.size(), where + ": in_flight length mismatch");
    for (std::size_t i = 0; i < got_in_flight.size(); ++i)
      assert_delivery(got_in_flight[i], *in_flight.array[i],
                      where + " in_flight[" + std::to_string(i) + "]");
    return true;
  });

  expected.assert_key_with("dead_letters", [&](const Json& dead) {
    const auto got_dead = queue.dead_letter_items();
    REQUIRE(got_dead.size() == dead.array.size(), where + ": dead-letter length mismatch");
    for (std::size_t i = 0; i < got_dead.size(); ++i) {
      const Json& want = *dead.array[i];
      REQUIRE(got_dead[i].item_id == static_cast<std::uint64_t>(want.find("item_id")->as_int()),
              where + ": dead-letter item_id mismatch");
      REQUIRE(got_dead[i].value == want.find("value")->as_str(),
              where + ": dead-letter value mismatch");
      REQUIRE(got_dead[i].attempts == static_cast<std::uint64_t>(want.find("attempts")->as_int()),
              where + ": dead-letter attempts mismatch");
      const std::string reason =
          got_dead[i].reason == WorkQueueDeadLetterReason::Expired ? "expired" : "nack";
      REQUIRE(reason == want.find("reason")->as_str(), where + ": dead-letter reason mismatch");
    }
    return true;
  });
}

template <typename OwnerContext, typename Queue>
std::size_t replay(const std::string& fixture, const std::string& flavor) {
  const auto root = lazily_test::parse_json(lazily_test::spec_fixture_text("collections", fixture));
  REQUIRE(root && root->is_object(), fixture + ": invalid JSON object");
  const Json* config = root->find("config");
  const Json* steps = root->find("steps");
  REQUIRE(config && steps && !steps->array.empty(), fixture + ": config or non-empty steps absent");

  OwnerContext ctx;
  Context& graph = queue_detail::graph(ctx);
  Queue queue(ctx, static_cast<std::uint64_t>(config->find("visibility_timeout")->as_int()),
              static_cast<std::uint64_t>(config->find("max_deliveries")->as_int()));
  Readers<Queue> readers(graph, queue);

  for (std::size_t i = 0; i < steps->array.size(); ++i) {
    const Json& step = *steps->array[i];
    const std::string where = flavor + " " + fixture + " step " + std::to_string(i);
    const Json* op = step.find("op");
    const Json* expected_block = step.find("expected");
    const Json* returns = step.find("returns");
    REQUIRE(op && expected_block && returns, where + ": op/expected/returns absent");
    readers.refresh();

    const std::string type = op->find("type")->as_str();
    if (type == "push") {
      const auto got = queue.push(ctx, op->find("value")->as_str());
      REQUIRE(got == static_cast<std::uint64_t>(returns->as_int()),
              where + ": push return mismatch");
    } else if (type == "claim") {
      const auto got = queue.claim(ctx, op->find("worker")->as_str(),
                                   static_cast<std::uint64_t>(op->find("now")->as_int()));
      if (returns->type == Json::Type::Null) {
        REQUIRE(!got, where + ": claim returned a delivery");
      } else {
        REQUIRE(got.has_value(), where + ": claim returned no delivery");
        assert_delivery(*got, *returns, where);
      }
    } else if (type == "ack" || type == "nack") {
      const std::string worker = op->find("worker")->as_str();
      const auto delivery_id = static_cast<std::uint64_t>(op->find("delivery_id")->as_int());
      const bool got = type == "ack" ? queue.ack(ctx, worker, delivery_id)
                                     : queue.nack(ctx, worker, delivery_id);
      REQUIRE(got == returns->as_bool(), where + ": settlement return mismatch");
    } else if (type == "reap_expired") {
      const auto got =
          queue.reap_expired(ctx, static_cast<std::uint64_t>(op->find("now")->as_int()));
      REQUIRE(got == static_cast<std::size_t>(returns->as_int()),
              where + ": reap_expired return mismatch");
    } else {
      REQUIRE(false, where + ": unsupported operation " + type);
    }

    lazily_test::AssertionKeys expected(where + " expected", *expected_block);
    readers.assert_invalidates(expected, where);
    assert_state(queue, expected, where);
    readers.refresh();
    expected.assert_key_with("reads", [&](const Json& reads) {
      REQUIRE(queue.pending_len(ctx) ==
                  static_cast<std::size_t>(reads.find("pending_len")->as_int()),
              where + ": pending_len read mismatch");
      REQUIRE(queue.is_empty(ctx) == reads.find("is_empty")->as_bool(),
              where + ": is_empty read mismatch");
      REQUIRE(queue.in_flight_len(ctx) ==
                  static_cast<std::size_t>(reads.find("in_flight_len")->as_int()),
              where + ": in_flight_len read mismatch");
      REQUIRE(queue.dead_letter_len(ctx) ==
                  static_cast<std::size_t>(reads.find("dead_letter_len")->as_int()),
              where + ": dead_letter_len read mismatch");
      return true;
    });
  }
  return steps->array.size();
}

} // namespace

int main() {
  const std::vector<std::string> fixtures = {
      "workqueue_competing_delivery.json",
      "workqueue_lease_deadletter.json",
  };
  std::size_t replayed = 0;
  for (const auto& fixture : fixtures) {
    replayed += replay<Context, WorkQueueCell<std::string>>(fixture, "sync");
    replayed +=
        replay<ThreadSafeContext, ThreadSafeWorkQueueCell<std::string>>(fixture, "thread-safe");
    replayed += replay<AsyncContext, AsyncWorkQueueCell<std::string>>(fixture, "async");
  }
  REQUIRE(replayed > 0, "work-queue replay executed zero steps");
  REQUIRE_FIXTURES_LOADED(2);
  std::cout << "work-queue conformance: 2 canonical fixtures x 3 flavors, " << replayed
            << " steps replayed" << std::endl;
  return 0;
}
