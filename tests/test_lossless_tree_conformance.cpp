// Lossless full-document tree CRDT conformance (`lazily-spec/lossless-tree-crdt.md`).
// A C++17 port of `lazily-rs/tests/lossless_tree_conformance.rs`, generically
// replaying every `lazily-spec/conformance/lossless-tree/*.json` corpus through
// `lazily::LosslessTreeCrdt`: build the seed tree on replica `a`, run a schedule
// of ops / forks / anti-entropy syncs (full + non-contiguous `deliver`) across
// named replicas, and assert exact rendered text, live-node counts, and
// convergence — the lossless invariant `render(tree) == source_text`.
//
// Fixture bytes are read from the sibling lazily-spec checkout;
// REQUIRE_FIXTURES_LOADED asserts the exact distinct-fixture count so a green
// run proves the whole corpus actually ran.

#include <lazily/lossless_tree_crdt.hpp>

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

static LeafKind leaf_kind(const std::string& s) {
  if (s == "token") return LeafKind::Token;
  if (s == "trivia") return LeafKind::Trivia;
  if (s == "raw") return LeafKind::Raw;
  if (s == "error") return LeafKind::Error;
  REQUIRE(false, "unknown leaf kind");
  return LeafKind::Raw;
}

static TreeNodeSeed node_seed(const Json* spec) {
  if (const Json* el = spec->find("element"))
    return TreeNodeSeedElement{el->str};
  const Json* leaf = spec->find("leaf");
  REQUIRE(leaf != nullptr, "node spec has neither element nor leaf");
  return TreeNodeSeedLeaf{leaf_kind(leaf->find("kind")->str), leaf->find("text")->str};
}

struct World {
  std::map<std::string, LosslessTreeCrdt> replicas;
  std::map<std::string, OpId> ids;

  const OpId& id(const std::string& label) {
    auto it = ids.find(label);
    REQUIRE(it != ids.end(), ("unknown node label: " + label).c_str());
    return it->second;
  }
  // `after` = null/absent -> nullptr; else the referenced node id.
  const OpId* after_of(const Json* op) {
    const Json* a = op->find("after");
    if (!a || a->is_null()) return nullptr;
    return &id(a->str);
  }

  void build_children(const Json* spec, const OpId& parent) {
    const Json* children = spec->find("children");
    if (!children) return;
    const OpId* prev = nullptr;
    OpId prev_store;
    for (const auto& child : children->array) {
      const std::string label = child->find("label")->str;
      OpId created =
          replicas.at("a").create_node(parent, prev, node_seed(child.get()));
      ids[label] = created;
      build_children(child.get(), ids.at(label));
      prev_store = created;
      prev = &prev_store;
    }
  }
};

static void apply_op(World& w, const std::string& on, const Json* op) {
  const std::string& kind = op->find("op")->str;
  LosslessTreeCrdt& r = w.replicas.at(on);
  if (kind == "create") {
    const OpId& parent = w.id(op->find("parent")->str);
    const OpId* after = w.after_of(op);
    OpId created = r.create_node(parent, after, node_seed(op));
    w.ids[op->find("label")->str] = created;
  } else if (kind == "edit_leaf") {
    const OpId& node = w.id(op->find("node")->str);
    const size_t at = static_cast<size_t>(op->find("at_byte")->as_int());
    const Json* del = op->find("delete_bytes");
    const Json* ins = op->find("insert");
    r.edit_leaf(node, at, del ? static_cast<size_t>(del->as_int()) : 0,
                ins ? ins->str : std::string());
  } else if (kind == "split") {
    const OpId& node = w.id(op->find("node")->str);
    OpId created = r.split_leaf(node, static_cast<size_t>(op->find("at_byte")->as_int()));
    w.ids[op->find("new_label")->str] = created;
  } else if (kind == "merge_leaves") {
    r.merge_adjacent_leaves(w.id(op->find("left")->str), w.id(op->find("right")->str));
  } else if (kind == "reorder") {
    r.reorder_child(w.id(op->find("node")->str), w.after_of(op));
  } else if (kind == "tombstone") {
    r.tombstone_node(w.id(op->find("node")->str));
  } else {
    REQUIRE(false, ("unknown lossless op: " + kind).c_str());
  }
}

static void apply_step(World& w, const Json* step) {
  if (const Json* fork = step->find("fork")) {
    const auto peer = static_cast<PeerId>(step->find("peer")->as_int());
    w.replicas.insert_or_assign(fork->str, w.replicas.at("a").fork(peer));
  } else if (const Json* clone = step->find("clone")) {
    w.replicas.insert_or_assign(clone->str, w.replicas.at(step->find("from")->str));
  } else if (const Json* sync = step->find("sync")) {
    const std::string& from = sync->find("from")->str;
    const std::string& to = sync->find("to")->str;
    TreeUpdate update = w.replicas.at(from).diff(w.replicas.at(to).frontier());
    w.replicas.at(to).apply_update(update);
  } else if (const Json* deliver = step->find("deliver")) {
    const std::string& from = deliver->find("from")->str;
    const std::string& to = deliver->find("to")->str;
    TreeUpdate full = w.replicas.at(from).diff(w.replicas.at(to).frontier());
    TreeUpdate selected;
    for (const auto& idx : deliver->find("only")->array)
      selected.ops.push_back(full.ops.at(static_cast<size_t>(idx->as_int())));
    w.replicas.at(to).apply_update(selected);
  } else if (const Json* on = step->find("on")) {
    apply_op(w, on->str, step);
  } else {
    REQUIRE(false, "unrecognized lossless step");
  }
}

static void assert_expect(World& w, const Json* expect, const std::string& scen) {
  if (const Json* text = expect->find("render"))
    REQUIRE(w.replicas.at("a").render() == text->str, ("render on a: " + scen).c_str());
  if (const Json* per = expect->find("render_on"))
    for (const auto& kv : per->object)
      REQUIRE(w.replicas.at(kv.first).render() == kv.second->str,
              ("render_on " + kv.first + ": " + scen).c_str());
  if (const Json* n = expect->find("live_nodes"))
    REQUIRE(w.replicas.at("a").live_node_count() == static_cast<size_t>(n->as_int()),
            ("live_nodes on a: " + scen).c_str());
  if (const Json* names = expect->find("converged")) {
    REQUIRE(!names->array.empty(), "converged list empty");
    const std::string first = w.replicas.at(names->array[0]->str).render();
    for (const auto& name : names->array)
      REQUIRE(w.replicas.at(name->str).render() == first,
              ("convergence: " + scen + " on " + name->str).c_str());
  }
}

static void run_fixture(const std::string& name, int& fixture_scenarios) {
  const std::string text = lazily_test::spec_fixture_text("lossless-tree", name);
  REQUIRE(text.find("\"LosslessTreeCrdt\"") != std::string::npos,
          "fixture is not a LosslessTreeCrdt corpus");
  auto doc = lazily_test::parse_json(text);
  const Json* scenarios = doc->find("scenarios");
  REQUIRE(scenarios != nullptr && !scenarios->array.empty(), "no scenarios");
  for (size_t i = 0; i < scenarios->array.size(); ++i) {
    const Json* scenario = scenarios->array[i].get();
    const Json* seed = scenario->find("seed");
    REQUIRE(seed != nullptr, "scenario missing seed");
    const std::string label =
        name + "[" + (scenario->find("name") ? scenario->find("name")->str
                                              : std::to_string(i)) + "]";
    World w;
    w.replicas.insert_or_assign(
        "a", LosslessTreeCrdt(static_cast<PeerId>(seed->find("peer")->as_int())));
    w.build_children(seed->find("tree"), kTreeRoot);
    if (const Json* steps = scenario->find("steps"))
      for (const auto& step : steps->array) apply_step(w, step.get());
    assert_expect(w, scenario->find("expect"), label);
    ++fixture_scenarios;
  }
}

static int g_scenarios = 0;

TEST(conformance_lossless_tree_all) {
  const char* fixtures[] = {
      "exact_roundtrip.json",
      "one_leaf_edit_delta.json",
      "split_merge.json",
      "concurrent_insert_same_parent.json",
      "concurrent_reorder_and_leaf_edit.json",
      "non_contiguous_anti_entropy.json",
      "token_trivia_preservation.json",
      "invalid_source_roundtrip.json",
      "concurrent_conflict_preserves_text.json",
  };
  for (const char* f : fixtures) run_fixture(f, g_scenarios);
  REQUIRE(g_scenarios >= 9, "expected at least one scenario per fixture");
}

int main() {
  REQUIRE_FIXTURES_LOADED(9);
  std::cout << "lazily-cpp lossless-tree conformance: " << test_passed << "/"
            << test_count << " passed (" << g_scenarios << " scenarios)" << std::endl;
  return test_passed == test_count ? 0 : 1;
}
