#include <lazily/lazily.hpp>

#include <cassert>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "test_assertion_keys.hpp"
#include "test_json.hpp"
#include "test_spec_fixture.hpp"

using namespace lazily;

static lazily_test::JsonPtr fixture() {
  return lazily_test::parse_json(
      lazily_test::spec_fixture_text("crdt-tree", "algebra.json"));
}

int main() {
  static_assert(is_crdt_tree_v<TextCrdt>, "TextCrdt is the canonical CrdtTree");
  const auto fx = fixture();
  for (const auto& scenario_ptr :
       lazily_test::json_array(lazily_test::json_member(*fx, "scenarios"))) {
    const auto& scenario = *scenario_ptr;
    const auto& seed = lazily_test::json_member(scenario, "seed");
    const auto peer =
        lazily_test::json_u64(lazily_test::json_member(seed, "peer"));
    const auto text =
        lazily_test::json_string(lazily_test::json_member(seed, "text"));

    if (scenario.has("replicas")) {
      const auto base = TextCrdt::from_str(peer, text);
      std::map<std::string, TextCrdt> replicas;
      for (const auto& replica_ptr : lazily_test::json_array(
               lazily_test::json_member(scenario, "replicas"))) {
        const auto& replica = *replica_ptr;
        auto branch = base.fork(lazily_test::json_u64(
            lazily_test::json_member(replica, "peer")));
        const auto insert =
            lazily_test::json_string(lazily_test::json_member(replica, "insert"));
        branch.insert_str(branch.visible_len(), insert);
        replicas.emplace(
            lazily_test::json_string(lazily_test::json_member(replica, "name")),
            std::move(branch));
      }

      std::vector<TextCrdt> folds;
      uint64_t fold_peer = 100;
      for (const auto& order_ptr : lazily_test::json_array(
               lazily_test::json_member(scenario, "merge_orders"))) {
        auto fold = base.fork(fold_peer++);
        for (const auto& name_ptr : lazily_test::json_array(*order_ptr)) {
          fold.merge_from(replicas.at(lazily_test::json_string(*name_ptr)));
        }
        folds.push_back(std::move(fold));
      }
      lazily_test::AssertionKeys expect(
          std::string("crdt-tree/algebra.json expect"),
          lazily_test::json_member(scenario, "expect"));
      for (const auto& fold : folds) {
        if (lazily_test::json_bool(
                lazily_test::json_member(expect, "texts_equal"))) {
          assert(fold.value() == folds.front().value());
        }
        if (lazily_test::json_bool(
                lazily_test::json_member(expect, "version_vectors_equal"))) {
          assert(fold.version_vector() == folds.front().version_vector());
        }
      }
    } else if (scenario.has("snapshot")) {
      assert(lazily_test::json_string(
                 lazily_test::json_member(scenario, "snapshot")) ==
             "delta_since({})");
      auto source = TextCrdt::from_str(peer, text);
      const auto snapshot = source.delta_since({});
      TextCrdt restored(lazily_test::json_u64(
          lazily_test::json_member(scenario, "restore_peer")));
      assert(restored.apply_delta(snapshot));
      lazily_test::AssertionKeys expect(
          std::string("crdt-tree/algebra.json expect"),
          lazily_test::json_member(scenario, "expect"));
      if (lazily_test::json_bool(
              lazily_test::json_member(expect, "restored_text_equal"))) {
        assert(restored.value() == source.value());
      }
      std::set<OpId> source_ids;
      std::set<OpId> restored_ids;
      for (const auto& op : snapshot) source_ids.insert(op.id);
      for (const auto& op : restored.delta_since({})) restored_ids.insert(op.id);
      if (lazily_test::json_bool(
              lazily_test::json_member(expect, "op_ids_equal"))) {
        assert(source_ids == restored_ids);
      }

      if (lazily_test::json_bool(
              lazily_test::json_member(scenario, "then_concurrent_edit"))) {
        source.insert_str(source.visible_len(), "A");
        restored.insert_str(restored.visible_len(), "B");
        source.merge_from(restored);
        restored.merge_from(source);
        assert(source.value() == restored.value());
        const auto converged_ops = source.delta_since({});
        std::set<OpId> converged_ids;
        for (const auto& op : converged_ops) converged_ids.insert(op.id);
        const auto duplicates = converged_ops.size() - converged_ids.size();
        assert(duplicates == lazily_test::json_u64(
                                 lazily_test::json_member(
                                     expect, "later_merge_duplicates")));
      }
    } else {
      assert(lazily_test::json_string(
                 lazily_test::json_member(scenario, "frontier")) ==
             "version_vector()");
      auto steady = TextCrdt::from_str(peer, text);
      const auto empty = steady.delta_since(steady.version_vector());
      lazily_test::AssertionKeys expect(
          std::string("crdt-tree/algebra.json expect"),
          lazily_test::json_member(scenario, "expect"));
      assert(empty.size() ==
             lazily_test::json_array(
                 lazily_test::json_member(expect, "delta")).size());
      assert(steady.apply_delta(empty) ==
             lazily_test::json_bool(
                 lazily_test::json_member(expect, "apply_changed")));
    }
  }
  REQUIRE_FIXTURES_LOADED(1);
  return 0;
}
