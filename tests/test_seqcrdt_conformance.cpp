// Move-aware sequence CRDT conformance (`lazily-spec/cell-model.md` § Move-aware
// sequence order). A C++17 port of `lazily-rs/tests/seqcrdt_conformance.rs`:
// generically replay the canonical compute fixture
// `lazily-spec/conformance/collections/seqcrdt_convergence.json` through
// `lazily::SeqCrdt` — concurrent insert/move/value-edit convergence, tombstone
// commutativity, and the lexicographic `(frac, peer)` total order.
//
// Each element is three independent LWW registers (value, position, deleted); a
// move is a SINGLE LWW reassignment of position (not delete+reinsert). The
// fixture bytes are read from the sibling lazily-spec checkout and the exact
// distinct-fixture count is asserted (REQUIRE_FIXTURES_LOADED) so a green run
// proves the corpus actually ran.

#include <lazily/crdt.hpp>

#include <iostream>
#include <map>
#include <optional>
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

// A fixture value is either an integer or a string (the seqcrdt corpus uses
// both). Held as a tagged value so `get` comparisons stay exact per JSON type.
struct SeqValue {
  bool is_str = false;
  long long i = 0;
  std::string s;
  bool operator==(const SeqValue& o) const {
    return is_str == o.is_str && (is_str ? s == o.s : i == o.i);
  }
};

static SeqValue value_of(const Json* v) {
  REQUIRE(v != nullptr, "seqcrdt op missing value");
  SeqValue out;
  if (v->type == Json::Type::String) {
    out.is_str = true;
    out.s = v->str;
  } else {
    out.is_str = false;
    out.i = v->as_int();
  }
  return out;
}

static bool value_matches(const SeqValue& got, const Json* expected) {
  return got == value_of(expected);
}

using Replica = SeqCrdt<std::string, SeqValue>;
using Replicas = std::map<std::string, Replica>;

static const std::string& field_str(const Json* op, const char* key) {
  const Json* f = op->find(key);
  REQUIRE(f != nullptr && f->type == Json::Type::String, "seqcrdt op field missing/str");
  return f->str;
}

// Apply an inline op node (insert_*/set_value/move_after/remove) to `replica`.
static void apply_op(Replica& replica, const Json* op, long long now) {
  const std::string& kind = field_str(op, "op");
  const std::string& id = field_str(op, "id");
  if (kind == "insert_back") {
    replica.insert_back(id, value_of(op->find("value")), now);
  } else if (kind == "insert_front") {
    replica.insert_front(id, value_of(op->find("value")), now);
  } else if (kind == "set_value") {
    replica.set_value(id, value_of(op->find("value")), now);
  } else if (kind == "move_after") {
    // move_after(id, anchor): right = element after anchor in the live order
    // (no id-skipping — matches lazily-rs SeqCrdt::move_after).
    const std::string& anchor = field_str(op, "anchor");
    auto ord = replica.order();
    const std::string* right = nullptr;
    for (size_t i = 0; i < ord.size(); ++i) {
      if (ord[i] == anchor) {
        if (i + 1 < ord.size()) right = &ord[i + 1];
        break;
      }
    }
    replica.move_between(id, &anchor, right, now);
  } else if (kind == "remove") {
    replica.remove(id, now);
  } else {
    REQUIRE(false, "unknown seqcrdt op");
  }
}

static void assert_order(const Replica& r, const Json* expected, const std::string& msg) {
  std::vector<std::string> want;
  for (const auto& e : expected->array)
    want.push_back(e->type == Json::Type::String ? e->str
                                                  : std::to_string(e->as_int()));
  const auto got = r.order();
  REQUIRE(got == want, ("order mismatch: " + msg).c_str());
}

static void run_scenario(const Json* scenario, size_t idx) {
  Replicas replicas;

  if (const Json* seed = scenario->find("seed")) {
    const long long peer = seed->find("peer")->as_int();
    Replica a(static_cast<PeerId>(peer));
    if (const Json* inserts = seed->find("inserts")) {
      for (const auto& ins : inserts->array)
        a.insert_back(field_str(ins.get(), "id"), value_of(ins->find("value")),
                      ins->find("now")->as_int());
    }
    replicas.insert_or_assign("a", std::move(a));
  } else if (const Json* rep = scenario->find("replica")) {
    replicas.insert_or_assign("a", Replica(static_cast<PeerId>(rep->find("peer")->as_int())));
  } else {
    REQUIRE(false, "scenario missing seed or replica");
  }

  const Json* steps = scenario->find("steps");
  REQUIRE(steps != nullptr, "scenario missing steps");
  for (const auto& step : steps->array) {
    if (const Json* fork = step->find("fork")) {
      const long long peer = step->find("peer")->as_int();
      replicas.insert_or_assign(fork->str,
                                replicas.at("a").fork(static_cast<PeerId>(peer)));
    } else if (const Json* clone = step->find("clone")) {
      const std::string& from = field_str(step.get(), "from");
      replicas.insert_or_assign(clone->str, replicas.at(from));  // deep copy
    } else if (const Json* merge = step->find("merge")) {
      const std::string& into = field_str(merge, "into");
      const std::string& from = field_str(merge, "from");
      const long long now = step->find("now")->as_int();
      replicas.at(into).merge(replicas.at(from), now);
    } else if (const Json* on = step->find("on")) {
      apply_op(replicas.at(on->str), step.get(), step->find("now")->as_int());
    } else if (step->find("op")) {
      apply_op(replicas.at("a"), step.get(), step->find("now")->as_int());
    } else {
      REQUIRE(false, "unrecognized seqcrdt step");
    }
  }

  const Json* expect = scenario->find("expect");
  REQUIRE(expect != nullptr, "scenario missing expect");
  const std::string tag = "scenario " + std::to_string(idx);

  // `len`/`contains_all` target: first element of first `orders_equal` pair, else "a".
  auto converged_target = [&]() -> std::string {
    if (const Json* oe = expect->find("orders_equal"))
      if (!oe->array.empty() && !oe->array[0]->array.empty())
        return oe->array[0]->array[0]->str;
    return "a";
  };

  if (const Json* order = expect->find("order"))
    assert_order(replicas.at("a"), order, tag);

  if (const Json* gets = expect->find("get")) {
    for (const auto& kv : gets->object) {
      auto got = replicas.at("a").get(kv.first);
      REQUIRE(got.has_value() && value_matches(*got, kv.second.get()),
              ("get mismatch: " + tag + " id=" + kv.first).c_str());
    }
  }

  if (const Json* len = expect->find("len"))
    REQUIRE(replicas.at(converged_target()).len() == static_cast<size_t>(len->as_int()),
            ("len mismatch: " + tag).c_str());

  if (const Json* pairs = expect->find("orders_equal")) {
    for (const auto& pair : pairs->array) {
      const std::string& a = pair->array[0]->str;
      const std::string& b = pair->array[1]->str;
      REQUIRE(replicas.at(a).order() == replicas.at(b).order(),
              ("orders should converge: " + tag + " " + a + "/" + b).c_str());
    }
  }

  if (const Json* on = expect->find("order_on"))
    for (const auto& kv : on->object)
      assert_order(replicas.at(kv.first), kv.second.get(), tag + " on " + kv.first);

  if (const Json* on = expect->find("get_on"))
    for (const auto& kv : on->object)
      for (const auto& g : kv.second->object) {
        auto got = replicas.at(kv.first).get(g.first);
        REQUIRE(got.has_value() && value_matches(*got, g.second.get()),
                ("get_on mismatch: " + tag + " " + kv.first + "/" + g.first).c_str());
      }

  if (const Json* ca = expect->find("contains_all")) {
    const std::string target = converged_target();
    for (const auto& id : ca->array)
      REQUIRE(replicas.at(target).contains(id->str),
              ("contains_all: " + tag + " id=" + id->str).c_str());
  }

  if (const Json* nc = expect->find("not_contains_on"))
    for (const auto& kv : nc->object)
      for (const auto& id : kv.second->array)
        REQUIRE(!replicas.at(kv.first).contains(id->str),
                ("not_contains_on: " + tag + " " + kv.first + "/" + id->str).c_str());
}

TEST(conformance_seqcrdt_convergence) {
  const std::string text =
      lazily_test::spec_fixture_text("collections", "seqcrdt_convergence.json");
  REQUIRE(text.find("\"model\": \"SeqCrdt\"") != std::string::npos ||
              text.find("\"SeqCrdt\"") != std::string::npos,
          "fixture is not the SeqCrdt convergence corpus");
  auto doc = lazily_test::parse_json(text);
  const Json* scenarios = doc->find("scenarios");
  REQUIRE(scenarios != nullptr && !scenarios->array.empty(), "no scenarios in fixture");
  for (size_t i = 0; i < scenarios->array.size(); ++i)
    run_scenario(scenarios->array[i].get(), i);
}

int main() {
  REQUIRE_FIXTURES_LOADED(1);
  std::cout << "lazily-cpp seqcrdt conformance: " << test_passed << "/" << test_count
            << " passed" << std::endl;
  return test_passed == test_count ? 0 : 1;
}
