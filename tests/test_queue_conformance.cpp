// Canonical queue conformance replay (#lzqfcppfx).
//
// lazily-cpp used to assert the queue contract in test_queue.cpp by hand: five
// blocks named after five canonical fixtures, each transcribing that fixture's
// steps into C++ literals. The observation harness there was sound — real
// derived readers, real validity checks — but the DATA was a copy, and a copy
// cannot drift-detect. If lazily-spec adds a step, tightens an `invalidates`
// entry, or changes an expected value, a hand transcription stays green while
// silently testing the old contract. That is why those five fixtures sat in
// scripts/check-conformance-coverage.sh's KNOWN_UNCOVERED list.
//
// This runner opens the canonical bytes and replays them. Everything it asserts
// comes from the library: `head`/`len`/`is_empty`/`is_full`/`closed` are read
// through derived `Computed` readers (so an edge is genuinely registered), the
// `invalidates` matrix is checked against whether those readers were actually
// invalidated, and `returns` is compared against what the queue really handed
// back. `elements` is checked structurally each step via head+len and then
// exactly at the end of every fixture by draining the queue and comparing the
// popped sequence — the queue exposes no enumeration, so a drain is the only
// way to assert the full contents without re-implementing the model.

#include <lazily/lazily.hpp>

#include <functional>
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
using lazily_test::parse_json;
using lazily_test::spec_fixture_text;

namespace {

// A reactive reader over one queue reader-kind. `is_valid()` reports whether
// the cached value survived the last op; the fixture's `invalidates` polarity
// is the negation of that.
template <typename T> struct Reader {
  Context& ctx;
  Computed<T> slot;
  T cached;
  Reader(Context& c, std::function<T(Compute&)> fn)
      : ctx(c), slot(c.template computed<T>([fn](Compute& cc) { return fn(cc); })) {
    cached = ctx.get(slot);
  }
  const T& value() { return cached; }
  bool is_valid() { return ctx.is_set(slot); }
  void refresh() { cached = ctx.get(slot); }
};

template <typename Queue> struct Readers {
  Reader<std::optional<std::string>> head;
  Reader<size_t> len;
  Reader<bool> empty;
  Reader<bool> full;
  Reader<bool> closed;
  Readers(Context& ctx, Queue& q)
      : head(ctx, [&q](Compute& c) { return q.head(c); }),
        len(ctx, [&q](Compute& c) { return q.len(c); }),
        empty(ctx, [&q](Compute& c) { return q.is_empty(c); }),
        full(ctx, [&q](Compute& c) { return q.is_full(c); }),
        closed(ctx, [&q](Compute& c) { return q.closed(c); }) {}
  void refresh() {
    head.refresh();
    len.refresh();
    empty.refresh();
    full.refresh();
    closed.refresh();
  }
};

int failures = 0;

void fail(const std::string& fixture, size_t step, const std::string& what) {
  std::cout << "FAIL [" << fixture << " step " << step << "]: " << what << std::endl;
  ++failures;
}

// Was this reader invalidated by the op just applied?
bool invalidated(bool still_valid) { return !still_valid; }

bool check_bool(const std::string& fixture, size_t step, const char* label, bool actual,
                bool expected) {
  if (actual == expected) return true;
  fail(fixture, step,
       std::string(label) + " = " + (actual ? "true" : "false") + ", fixture says " +
           (expected ? "true" : "false"));
  return false;
}

template <typename OwnerContext, typename Queue>
void replay(const std::string& name, const std::string& flavor) {
  const std::string label = flavor + " " + name;
  const std::string text = spec_fixture_text("collections", name);
  const auto doc = parse_json(text);
  REQUIRE(doc && doc->type == Json::Type::Object, "fixture did not parse as a JSON object");

  const Json* initial = doc->find("initial");
  REQUIRE(initial != nullptr, "fixture has no `initial`");
  const Json* cap = initial->find("capacity");

  OwnerContext ctx;
  // All five fixtures start from an empty, open queue; honour the fields anyway
  // so a future fixture with a seeded `initial` does not silently start blank.
  const Json* init_elems = initial->find("elements");
  REQUIRE(init_elems == nullptr || init_elems->array.empty(),
          "this runner does not seed a non-empty `initial.elements` yet — a "
          "fixture that needs one must extend the runner, not be skipped");

  Queue q = (cap != nullptr && cap->type == Json::Type::Number)
                ? Queue::bounded(ctx, static_cast<size_t>(cap->number))
                : Queue(ctx);

  Context& graph = queue_detail::graph(ctx);
  Readers<Queue> r(graph, q);

  const Json* steps = doc->find("steps");
  REQUIRE(steps != nullptr && !steps->array.empty(),
          "fixture has no steps — a replay of nothing is not a replay");

  std::vector<std::string> expected_final;

  for (size_t i = 0; i < steps->array.size(); ++i) {
    const Json& step = *steps->array[i];
    const Json* op = step.find("op");
    REQUIRE(op != nullptr, "step has no `op`");
    const std::string type = op->find("type")->str;

    r.refresh(); // every reader valid again before the op

    std::optional<std::string> returned;
    std::string status;

    if (type == "push") {
      q.push(ctx, op->find("value")->str);
    } else if (type == "try_push") {
      const PushResult res = q.try_push(ctx, op->find("value")->str);
      status = res == PushResult::Ok ? "Ok" : res == PushResult::Full ? "Full" : "Closed";
    } else if (type == "pop" || type == "try_pop") {
      // Both spellings go through `try_pop`. The spec's `pop` still reports
      // `Empty` vs `Closed` (see queuecell_spsc_push_pop step 4), and cpp's
      // `pop()` convenience wrapper collapses both to nullopt — so replaying
      // `pop` through that wrapper cannot express the contract the fixture
      // pins. `try_pop` is the same operation with the distinction intact.
      PopResult<std::string> res = q.try_pop(ctx);
      if (res.is_value()) {
        returned = res.value;
      } else {
        status = res.is_closed() ? "Closed" : "Empty";
      }
    } else if (type == "close") {
      q.close(ctx);
    } else if (type == "batch") {
      // MPSC: multiple producers inside one batch. The whole batch is one
      // invalidation frontier, which is the point of the fixture.
      const Json* inner = op->find("ops");
      REQUIRE(inner != nullptr, "batch op has no `ops`");
      queue_detail::batch(ctx, [&](Context&) {
        for (const auto& sub : inner->array) {
          const std::string sub_type = sub->find("type")->str;
          REQUIRE(sub_type == "push", "batch currently carries only pushes; a new inner op must "
                                      "extend this runner rather than be ignored");
          q.push(ctx, sub->find("value")->str);
        }
      });
    } else {
      fail(label, i,
           "unknown op type `" + type +
               "` — the corpus grew a case "
               "this runner does not implement");
      continue;
    }

    const Json* exp_block = step.find("expected");
    REQUIRE(exp_block != nullptr, "step has no `expected`");
    lazily_test::AssertionKeys exp(label + " step " + std::to_string(i) + " expected", *exp_block);

    // The invalidation matrix, captured BEFORE any refresh — reading a reader
    // revalidates it, so the order here is load-bearing.
    //
    // `invalidates` nests under `expected`. A runner that looked for it at step
    // level would find nothing and check nothing, which is exactly the bug that
    // silently disabled lazily-rs's map assertion.
    exp.assert_key_with_if_present("invalidates", [&](const Json& inv) {
      const struct {
        const char* key;
        bool still_valid;
      } probes[] = {
          {"head", r.head.is_valid()},      {"len", r.len.is_valid()},
          {"is_empty", r.empty.is_valid()}, {"is_full", r.full.is_valid()},
          {"closed", r.closed.is_valid()},
      };
      bool ok = true;
      for (const auto& probe : probes) {
        const Json* want = inv.find(probe.key);
        if (want == nullptr) continue;
        const bool got = invalidated(probe.still_valid);
        if (got != want->boolean) {
          fail(label, i,
               std::string("invalidates.") + probe.key + " = " + (got ? "true" : "false") +
                   ", fixture says " + (want->boolean ? "true" : "false"));
          ok = false;
        }
      }
      return ok;
    });

    // `returns`
    if (const Json* ret = step.find("returns")) {
      if (ret->type == Json::Type::String) {
        const std::string want = ret->str;
        const std::string got = returned ? *returned : status;
        if (got != want) {
          fail(label, i, "returns = `" + got + "`, fixture says `" + want + "`");
        }
      } else if (ret->type == Json::Type::Null) {
        if (returned) fail(label, i, "expected no return value, got one");
      }
    }

    // Reader values, checked only for the keys this step actually pins.
    r.refresh();
    exp.assert_key_with_if_present("head", [&](const Json& e) {
      const auto got = r.head.value();
      if (e.type == Json::Type::Null) {
        if (!got) return true;
        fail(label, i, "head has a value, fixture says null");
        return false;
      }
      if (got && *got == e.str) return true;
      fail(label, i, "head = " + (got ? *got : std::string("null")) + ", fixture says " + e.str);
      return false;
    });
    exp.assert_key_with_if_present("len", [&](const Json& e) {
      if (r.len.value() == static_cast<size_t>(e.number)) return true;
      fail(label, i,
           "len = " + std::to_string(r.len.value()) + ", fixture says " +
               std::to_string(static_cast<size_t>(e.number)));
      return false;
    });
    exp.assert_key_with_if_present("is_empty", [&](const Json& e) {
      return check_bool(name, i, "is_empty", r.empty.value(), e.boolean);
    });
    exp.assert_key_with_if_present("is_full", [&](const Json& e) {
      return check_bool(name, i, "is_full", r.full.value(), e.boolean);
    });
    exp.assert_key_with_if_present("closed", [&](const Json& e) {
      return check_bool(name, i, "closed", r.closed.value(), e.boolean);
    });

    if (const Json* e = exp.find("elements")) {
      // A queue exposes no enumeration, and popping to read it would mutate the
      // very state the next step asserts. The per-step body claim is therefore
      // carried to the end of the fixture and asserted by draining; head+len
      // above pin its boundary each step (#lzconsumednotasserted).
      exp.excuse_key("elements", "queue has no enumeration API and a read would consume; "
                                 "asserted at end of fixture by draining and comparing the "
                                 "popped sequence, with head+len pinned every step");
      expected_final.clear();
      for (const auto& el : e->array)
        expected_final.push_back(el->str);
    }
  }

  // `elements` exactly, from the library: drain and compare. Every step already
  // pinned head+len, but only a drain proves the whole body — and the queue has
  // no enumeration API, so reconstructing it any other way would mean asserting
  // against a model this runner maintains itself, which tests nothing.
  std::vector<std::string> drained;
  while (true) {
    auto v = q.pop(ctx);
    if (!v) break;
    drained.push_back(*v);
  }
  if (drained != expected_final) {
    std::string got;
    for (const auto& d : drained)
      got += d + " ";
    std::string want;
    for (const auto& w : expected_final)
      want += w + " ";
    fail(label, steps->array.size(),
         "final drain = [" + got + "], fixture's last `elements` = [" + want + "]");
  }
}

} // namespace

int main() {
  const char* fixtures[] = {
      "queuecell_spsc_push_pop.json",     "queuecell_popped_head_observation.json",
      "queuecell_mpsc_multi_writer.json", "queuecell_bounded_backpressure.json",
      "queuecell_closure_lifecycle.json",
  };

  for (const char* f : fixtures) {
    replay<Context, QueueCell<std::string>>(f, "sync");
    replay<ThreadSafeContext, ThreadSafeQueueCell<std::string>>(f, "thread-safe");
    replay<AsyncContext, AsyncQueueCell<std::string>>(f, "async");
  }

  if (failures != 0) {
    std::cout << "queue conformance: " << failures << " failure(s)" << std::endl;
    return 1;
  }

  // Positive proof this binary opened the corpus. Without it, a replay that
  // silently loaded nothing would print the same success line.
  REQUIRE_FIXTURES_LOADED(5);

  std::cout << "queue conformance: 5 canonical fixtures x 3 flavors replayed" << std::endl;
  return 0;
}
