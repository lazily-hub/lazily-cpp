// Canonical collections conformance replays for the fixtures that do not fit
// the keyed-map/queue-family runners.

#include <lazily/lazily.hpp>
#include <lazily/merge.hpp>
#include <lazily/sem_tree.hpp>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "test_json.hpp"
#include "test_spec_fixture.hpp"

using namespace lazily;

namespace {

using lazily_test::Json;
using lazily_test::JsonPtr;

JsonPtr fixture(const std::string& name) {
  return lazily_test::parse_json(
      lazily_test::spec_fixture_text("collections", name));
}

std::vector<std::string> string_array(const Json& value) {
  std::vector<std::string> result;
  for (const auto& item : lazily_test::json_array(value)) {
    result.push_back(lazily_test::json_string(*item));
  }
  return result;
}

std::vector<std::pair<std::string, int>> keyed_values(const Json& value) {
  const auto order =
      string_array(lazily_test::json_member(value, "order"));
  const auto& values = lazily_test::json_member(value, "values");
  std::vector<std::pair<std::string, int>> result;
  for (const auto& key : order) {
    result.emplace_back(
        key, static_cast<int>(lazily_test::json_u64(
                 lazily_test::json_member(values, key))));
  }
  return result;
}

template <typename Policy>
void replay_merge_scenario(const Json& scenario) {
  const auto& flags = lazily_test::json_member(scenario, "flags");
  assert(Policy::commutative ==
         lazily_test::json_bool(
             lazily_test::json_member(flags, "commutative")));
  assert(Policy::idempotent ==
         lazily_test::json_bool(
             lazily_test::json_member(flags, "idempotent")));

  Context ctx;
  MergeCell<long, Policy> cell(
      ctx, static_cast<long>(
               lazily_test::json_member(scenario, "initial").as_int()));
  auto observed =
      ctx.computed<long>([&](Compute& compute) { return cell.get(compute); });
  (void)ctx.get(observed);

  for (const auto& step_ptr : lazily_test::json_array(
           lazily_test::json_member(scenario, "steps"))) {
    const auto& step = *step_ptr;
    const auto& expected = lazily_test::json_member(step, "expected");
    cell.merge(static_cast<long>(
        lazily_test::json_member(step, "merge").as_int()));
    assert(cell.get() ==
           static_cast<long>(
               lazily_test::json_member(expected, "value").as_int()));
    const bool was_cached = ctx.is_set(observed);
    (void)ctx.get(observed);
    assert((!was_cached) ==
           lazily_test::json_bool(
               lazily_test::json_member(expected, "invalidates")));
  }
}

void test_mergecell_algebra() {
  const auto root = fixture("mergecell_algebra.json");
  for (const auto& scenario_ptr : lazily_test::json_array(
           lazily_test::json_member(*root, "scenarios"))) {
    const auto& scenario = *scenario_ptr;
    const auto policy =
        lazily_test::json_string(
            lazily_test::json_member(scenario, "policy"));
    if (policy == "KeepLatest") {
      replay_merge_scenario<KeepLatest>(scenario);
    } else if (policy == "Sum") {
      replay_merge_scenario<Sum>(scenario);
    } else {
      assert(policy == "Max");
      replay_merge_scenario<Max>(scenario);
    }
  }
}

void test_keyed_reconciliation() {
  const auto root = fixture("keyed_reconciliation_lis.json");
  const auto& reconcile_json = lazily_test::json_member(*root, "reconcile");
  const auto prior =
      keyed_values(lazily_test::json_member(reconcile_json, "prior"));
  const auto target =
      keyed_values(lazily_test::json_member(reconcile_json, "target"));
  const auto ops = reconcile(prior, target);
  const auto& expected = lazily_test::json_member(*root, "expected");
  const auto& expected_ops =
      lazily_test::json_array(lazily_test::json_member(expected, "ops"));
  assert(ops.size() == expected_ops.size());

  const auto result_order =
      string_array(lazily_test::json_member(expected, "result_order"));
  for (std::size_t i = 0; i < ops.size(); ++i) {
    const auto& want = *expected_ops[i];
    const auto type =
        lazily_test::json_string(lazily_test::json_member(want, "type"));
    const auto key =
        lazily_test::json_string(lazily_test::json_member(want, "key"));
    assert(ops[i].key == key);
    if (type == "remove") {
      assert((ops[i].kind == DiffOp<std::string, int>::Kind::Remove));
    } else {
      assert(type == "move");
      assert((ops[i].kind == DiffOp<std::string, int>::Kind::Move));
      const auto at =
          std::find(result_order.begin(), result_order.end(), key);
      assert(at != result_order.end());
      assert(ops[i].index ==
             static_cast<std::size_t>(at - result_order.begin()));
      const auto& after =
          lazily_test::json_string(
              lazily_test::json_member(want, "after"));
      assert(at != result_order.begin() && *(at - 1) == after);
    }
  }

  Context ctx;
  SourceMap<std::string, int> map(ctx);
  for (const auto& entry : prior) map.entry(ctx, entry.first, entry.second);
  std::map<std::string, Computed<int>> observers;
  for (const auto& key : string_array(lazily_test::json_member(
           expected, "stable_keys_not_invalidated"))) {
    observers.emplace(
        key, ctx.computed<int>(
                 [&map, key](Compute& compute) {
                   return map.get(compute, key).value_or(0);
                 }));
    (void)ctx.get(observers.at(key));
  }
  map.reconcile(ctx, target);
  assert(map.keys(ctx) == result_order);
  for (const auto& observer : observers) {
    assert(ctx.is_set(observer.second));
  }
}

void add_sem_children(SemTree<int, int>& tree, const Json& node) {
  const Json* children = node.find("children");
  if (children == nullptr) return;
  const auto parent =
      lazily_test::json_string(lazily_test::json_member(node, "id"));
  const auto order =
      string_array(lazily_test::json_member(*children, "order"));
  const auto& values = lazily_test::json_member(*children, "values");
  for (const auto& child_id : order) {
    const auto& child = lazily_test::json_member(values, child_id);
    tree.add_child(
        parent, child_id,
        static_cast<int>(
            lazily_test::json_member(child, "value").as_int()));
    add_sem_children(tree, child);
  }
}

void assert_sem_values(SemTree<int, int>& tree, const Json& expected) {
  assert(expected.is_object());
  for (const auto& entry : expected.object) {
    if (entry.first == "sibling_a_cached" ||
        entry.first == "downstream_consumer_reran") {
      continue;
    }
    const auto got = tree.node_value(entry.first);
    assert(got.has_value());
    assert(*got == entry.second->as_int());
  }
}

void test_semtree_incremental() {
  const auto root = fixture("semtree_incremental.json");
  for (const auto& scenario_ptr : lazily_test::json_array(
           lazily_test::json_member(*root, "scenarios"))) {
    const auto& scenario = *scenario_ptr;
    const auto& tree_json = lazily_test::json_member(scenario, "tree");
    const auto fold_name =
        lazily_test::json_string(
            lazily_test::json_member(scenario, "fold"));
    SemTree<int, int>::FoldFn fold;
    if (fold_name == "sum") {
      fold = [](const int& value, const std::vector<int>& children) {
        int result = value;
        for (int child : children) result += child;
        return result;
      };
    } else {
      assert(fold_name == "count_positive");
      fold = [](const int& value, const std::vector<int>& children) {
        int result = value > 0 ? 1 : 0;
        for (int child : children) result += child;
        return result;
      };
    }

    Context ctx;
    SemTree<int, int> tree(
        ctx,
        lazily_test::json_string(
            lazily_test::json_member(tree_json, "id")),
        static_cast<int>(
            lazily_test::json_member(tree_json, "value").as_int()),
        fold);
    add_sem_children(tree, tree_json);
    assert_sem_values(
        tree, lazily_test::json_member(scenario, "expect_initial"));

    int downstream_runs = 0;
    std::optional<Computed<int>> downstream;
    const auto& expect_after =
        lazily_test::json_member(scenario, "expect_after");
    if (expect_after.has("downstream_consumer_reran")) {
      const auto root_handle = tree.root_handle();
      downstream.emplace(ctx.computed<int>(
          [&, root_handle](Compute& compute) {
            ++downstream_runs;
            return compute.get(root_handle);
          }));
      (void)ctx.get(*downstream);
    }

    const Json* edit = scenario.find("edit");
    if (edit != nullptr) {
      tree.set_value(
          lazily_test::json_string(
              lazily_test::json_member(*edit, "id")),
          static_cast<int>(
              lazily_test::json_member(*edit, "value").as_int()));
    } else {
      const auto& remove =
          lazily_test::json_member(scenario, "remove_child");
      tree.remove_child(
          lazily_test::json_string(
              lazily_test::json_member(remove, "parent")),
          lazily_test::json_string(
              lazily_test::json_member(remove, "child")));
    }

    const Json* sibling_cached =
        expect_after.find("sibling_a_cached");
    if (sibling_cached != nullptr) {
      assert(tree.is_cached("a") ==
             lazily_test::json_bool(*sibling_cached));
    }

    const Json* consumer_reran =
        expect_after.find("downstream_consumer_reran");
    if (consumer_reran != nullptr) {
      const int before = downstream_runs;
      (void)tree.value();
      (void)ctx.get(*downstream);
      assert((downstream_runs > before) ==
             lazily_test::json_bool(*consumer_reran));
    }
    assert_sem_values(tree, expect_after);
  }
}

Block json_block(const Json& value) {
  const auto text =
      lazily_test::json_string(lazily_test::json_member(value, "text"));
  const Json* anchor = value.find("anchor");
  return anchor == nullptr
             ? new_block(text)
             : new_anchored_block(lazily_test::json_string(*anchor), text);
}

std::vector<Block> json_blocks(const Json& value) {
  std::vector<Block> result;
  for (const auto& item : lazily_test::json_array(value)) {
    result.push_back(json_block(*item));
  }
  return result;
}

std::string match_string(const Match& match) {
  if (match.kind == Match::Kind::Inserted) return "Inserted";
  return std::string(match.kind == Match::Kind::Same ? "Same:" : "Edited:") +
         std::to_string(match.old_index);
}

void test_stableid_alignment() {
  const auto root = fixture("stableid_alignment.json");
  for (const auto& scenario_ptr : lazily_test::json_array(
           lazily_test::json_member(*root, "scenarios"))) {
    const auto& scenario = *scenario_ptr;
    const auto& expect = lazily_test::json_member(scenario, "expect");
    if (scenario.has("blocks")) {
      const auto blocks =
          json_blocks(lazily_test::json_member(scenario, "blocks"));
      const Json* equal = expect.find("key_equal");
      if (equal != nullptr) {
        for (const auto& pair : lazily_test::json_array(*equal)) {
          const auto& indices = lazily_test::json_array(*pair);
          assert(block_key_of(blocks[lazily_test::json_u64(*indices[0])])
                     .equals(block_key_of(
                         blocks[lazily_test::json_u64(*indices[1])])));
        }
      }
      const Json* not_equal = expect.find("key_not_equal");
      if (not_equal != nullptr) {
        for (const auto& pair : lazily_test::json_array(*not_equal)) {
          const auto& indices = lazily_test::json_array(*pair);
          assert(!block_key_of(blocks[lazily_test::json_u64(*indices[0])])
                      .equals(block_key_of(
                          blocks[lazily_test::json_u64(*indices[1])])));
        }
      }
      continue;
    }

    const auto old_blocks =
        json_blocks(lazily_test::json_member(scenario, "old"));
    const auto new_blocks =
        json_blocks(lazily_test::json_member(scenario, "new"));
    if (expect.has("new_key_equals_old_key")) {
      const auto old_keys = assign_stable_keys(old_blocks, old_blocks);
      const auto new_keys = assign_stable_keys(old_blocks, new_blocks);
      for (const auto& pair : lazily_test::json_array(
               lazily_test::json_member(expect, "new_key_equals_old_key"))) {
        const auto& indices = lazily_test::json_array(*pair);
        assert(new_keys[lazily_test::json_u64(*indices[0])] ==
               old_keys[lazily_test::json_u64(*indices[1])]);
      }
      continue;
    }

    const auto alignment = align(old_blocks, new_blocks);
    const auto matches =
        string_array(lazily_test::json_member(expect, "matches"));
    assert(matches.size() == alignment.new_matches.size());
    for (std::size_t i = 0; i < matches.size(); ++i) {
      assert(match_string(alignment.new_matches[i]) == matches[i]);
    }
    std::vector<int> removed;
    for (const auto& item : lazily_test::json_array(
             lazily_test::json_member(expect, "removed"))) {
      removed.push_back(static_cast<int>(lazily_test::json_u64(*item)));
    }
    assert(alignment.removed == removed);
    const Json* similarity_min = expect.find("similarity_min");
    if (similarity_min != nullptr) {
      for (const auto& match : alignment.new_matches) {
        if (match.kind == Match::Kind::Edited) {
          assert(match.sim >= lazily_test::json_number(*similarity_min));
        }
      }
    }
  }
}

using ReplicaMap = std::map<std::string, TextCrdt>;

ReplicaMap text_seed(const Json& scenario) {
  const auto& seed = lazily_test::json_member(scenario, "seed");
  const Json* replica = scenario.find("replica");
  if (replica != nullptr) {
    ReplicaMap result;
    result.emplace(
        "a", TextCrdt::from_str(
                 lazily_test::json_u64(
                     lazily_test::json_member(*replica, "peer")),
                 lazily_test::json_string(seed)));
    return result;
  }
  ReplicaMap result;
  result.emplace(
      "a", TextCrdt::from_str(
               lazily_test::json_u64(
                   lazily_test::json_member(seed, "peer")),
               lazily_test::json_string(
                   lazily_test::json_member(seed, "text"))));
  return result;
}

void exchange(ReplicaMap& replicas, const std::string& left_name,
              const std::string& right_name) {
  auto& left = replicas.at(left_name);
  auto& right = replicas.at(right_name);
  const auto to_left = right.delta_since(left.version_vector());
  const auto to_right = left.delta_since(right.version_vector());
  left.apply_delta(to_left);
  right.apply_delta(to_right);
}

void apply_text_steps(ReplicaMap& replicas, const Json& steps) {
  for (const auto& step_ptr : lazily_test::json_array(steps)) {
    const auto& step = *step_ptr;
    if (step.has("fork")) {
      replicas.emplace(
          lazily_test::json_string(lazily_test::json_member(step, "fork")),
          replicas.at("a").fork(lazily_test::json_u64(
              lazily_test::json_member(step, "peer"))));
    } else if (step.has("clone")) {
      replicas.emplace(
          lazily_test::json_string(lazily_test::json_member(step, "clone")),
          replicas
              .at(lazily_test::json_string(
                  lazily_test::json_member(step, "from")))
              .clone());
    } else if (step.has("merge")) {
      const auto& merge = lazily_test::json_member(step, "merge");
      replicas
          .at(lazily_test::json_string(
              lazily_test::json_member(merge, "into")))
          .merge(replicas.at(lazily_test::json_string(
              lazily_test::json_member(merge, "from"))));
    } else if (step.has("exchange")) {
      const auto names =
          string_array(lazily_test::json_member(step, "exchange"));
      exchange(replicas, names[0], names[1]);
    } else if (step.has("snapshot")) {
      const auto& snapshot = lazily_test::json_member(step, "snapshot");
      const auto from =
          lazily_test::json_string(
              lazily_test::json_member(snapshot, "from"));
      const auto into =
          lazily_test::json_string(
              lazily_test::json_member(snapshot, "into"));
      TextCrdt target(lazily_test::json_u64(
          lazily_test::json_member(snapshot, "peer")));
      const bool changed =
          target.apply_delta(replicas.at(from).delta_since({}));
      assert(changed ==
             lazily_test::json_bool(
                 lazily_test::json_member(step, "expect_changed")));
      replicas.emplace(into, std::move(target));
    } else if (step.has("new")) {
      replicas.emplace(
          lazily_test::json_string(lazily_test::json_member(step, "new")),
          TextCrdt(lazily_test::json_u64(
              lazily_test::json_member(step, "peer"))));
    } else if (step.has("delta")) {
      const auto& delta = lazily_test::json_member(step, "delta");
      auto& into = replicas.at(lazily_test::json_string(
          lazily_test::json_member(delta, "into")));
      auto& from = replicas.at(lazily_test::json_string(
          lazily_test::json_member(delta, "from")));
      const bool changed =
          into.apply_delta(from.delta_since(into.version_vector()));
      assert(changed ==
             lazily_test::json_bool(
                 lazily_test::json_member(step, "expect_changed")));
    } else {
      const auto name =
          step.has("on")
              ? lazily_test::json_string(
                    lazily_test::json_member(step, "on"))
              : "a";
      auto& replica = replicas.at(name);
      const auto op =
          lazily_test::json_string(
              lazily_test::json_member(step, "op"));
      if (op == "insert") {
        replica.insert(
            lazily_test::json_u64(
                lazily_test::json_member(step, "index")),
            lazily_test::json_string(
                lazily_test::json_member(step, "ch")));
      } else if (op == "insert_str") {
        replica.insert_str(
            lazily_test::json_u64(
                lazily_test::json_member(step, "index")),
            lazily_test::json_string(
                lazily_test::json_member(step, "str")));
      } else if (op == "delete") {
        replica.del(lazily_test::json_u64(
            lazily_test::json_member(step, "index")));
      } else {
        assert(op == "gc");
        const bool stable = lazily_test::json_bool(
            lazily_test::json_member(step, "stable"));
        const auto collected =
            replica.gc_with([stable](OpId) { return stable; });
        assert(collected ==
               lazily_test::json_u64(
                   lazily_test::json_member(step, "expect_collected")));
      }
    }
  }
}

void assert_version_vector(const TextCrdt& replica, const Json& expected) {
  const auto actual = replica.version_vector();
  assert(actual.size() == expected.object.size());
  for (const auto& entry : expected.object) {
    assert(actual.at(std::stoull(entry.first)) == entry.second->as_int());
  }
}

void assert_text_expectations(const ReplicaMap& replicas, const Json& expect) {
  const Json* equal = expect.find("texts_equal");
  if (equal != nullptr) {
    for (const auto& pair : lazily_test::json_array(*equal)) {
      const auto names = string_array(*pair);
      assert(replicas.at(names[0]).text() == replicas.at(names[1]).text());
    }
  }
  const Json* text = expect.find("text");
  if (text != nullptr) {
    assert(replicas.at("a").text() == lazily_test::json_string(*text));
  }
  const Json* len = expect.find("len");
  if (len != nullptr) {
    assert(replicas.at("a").visible_len() == lazily_test::json_u64(*len));
  }
  const Json* starts = expect.find("a_starts_with");
  if (starts != nullptr) {
    assert(replicas.at("a").text().rfind(
               lazily_test::json_string(*starts), 0) == 0);
  }
  const Json* ends = expect.find("a_ends_with");
  if (ends != nullptr) {
    const auto& actual = replicas.at("a").text();
    const auto& suffix = lazily_test::json_string(*ends);
    assert(actual.size() >= suffix.size() &&
           actual.compare(actual.size() - suffix.size(), suffix.size(),
                          suffix) == 0);
  }
  const Json* tombstones = expect.find("tombstone_count");
  if (tombstones != nullptr) {
    assert(replicas.at("a").tombstone_count() ==
           lazily_test::json_u64(*tombstones));
  }
  const Json* text_on = expect.find("text_on");
  if (text_on != nullptr) {
    for (const auto& entry : text_on->object) {
      assert(replicas.at(entry.first).text() ==
             lazily_test::json_string(*entry.second));
    }
  }
  const Json* vv_on = expect.find("version_vector_on");
  if (vv_on != nullptr) {
    for (const auto& entry : vv_on->object) {
      assert_version_vector(replicas.at(entry.first), *entry.second);
    }
  }
}

void replay_text_fixture(const std::string& name) {
  const auto root = fixture(name);
  for (const auto& scenario_ptr : lazily_test::json_array(
           lazily_test::json_member(*root, "scenarios"))) {
    const auto& scenario = *scenario_ptr;
    auto replicas = text_seed(scenario);
    apply_text_steps(
        replicas, lazily_test::json_member(scenario, "steps"));
    assert_text_expectations(
        replicas, lazily_test::json_member(scenario, "expect"));
  }
}

}  // namespace

int main() {
  test_mergecell_algebra();
  test_keyed_reconciliation();
  test_semtree_incremental();
  test_stableid_alignment();
  replay_text_fixture("textcrdt_convergence.json");
  replay_text_fixture("textcrdt_delta_sync.json");
  REQUIRE_FIXTURES_LOADED(6);
  return 0;
}
