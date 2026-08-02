#include <lazily/lazily.hpp>

#include "test_assertion_keys.hpp"
#include "test_json.hpp"
#include "test_spec_fixture.hpp"
#include <cassert>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace lazily;

static lazily_test::JsonPtr fixture() {
  return lazily_test::parse_json(lazily_test::spec_fixture_text("crdt-tree", "algebra.json"));
}

int main() {
  static_assert(is_crdt_tree_v<TextCrdt>, "TextCrdt is the canonical CrdtTree");
  const auto fx = fixture();
  const auto& scenarios = lazily_test::json_array(lazily_test::json_member(*fx, "scenarios"));
  for (const auto& sv : lazily_test::scenario_views("crdt-tree/algebra.json", scenarios)) {
    // Rung 4 books on the PAYLOAD handoff (#lzscenariobodyskip), so a body
    // that stops short of replaying stops being booked.
    const auto& scenario = sv.replay();
    const auto& seed = lazily_test::json_member(scenario, "seed");
    const auto peer = lazily_test::json_u64(lazily_test::json_member(seed, "peer"));
    const auto text = lazily_test::json_string(lazily_test::json_member(seed, "text"));

    if (scenario.has("replicas")) {
      const auto base = TextCrdt::from_str(peer, text);
      std::map<std::string, TextCrdt> replicas;
      for (const auto& replica_ptr :
           lazily_test::json_array(lazily_test::json_member(scenario, "replicas"))) {
        const auto& replica = *replica_ptr;
        auto branch = base.fork(lazily_test::json_u64(lazily_test::json_member(replica, "peer")));
        const auto insert = lazily_test::json_string(lazily_test::json_member(replica, "insert"));
        branch.insert_str(branch.visible_len(), insert);
        replicas.emplace(lazily_test::json_string(lazily_test::json_member(replica, "name")),
                         std::move(branch));
      }

      std::vector<TextCrdt> folds;
      uint64_t fold_peer = 100;
      for (const auto& order_ptr :
           lazily_test::json_array(lazily_test::json_member(scenario, "merge_orders"))) {
        auto fold = base.fork(fold_peer++);
        for (const auto& name_ptr : lazily_test::json_array(*order_ptr)) {
          fold.merge_from(replicas.at(lazily_test::json_string(*name_ptr)));
        }
        folds.push_back(std::move(fold));
      }
      lazily_test::AssertionKeys expect(std::string("crdt-tree/algebra.json expect"),
                                        lazily_test::json_member(scenario, "expect"));
      // The fixture's claim is the ASSERTION, not a gate on one: reading
      // `texts_equal` and only checking convergence when it is true means a
      // fixture that flips it to false changes nothing (#lzconsumednotasserted).
      for (const auto& fold : folds) {
        expect.assert_key("texts_equal", fold.value() == folds.front().value());
        expect.assert_key("version_vectors_equal",
                          fold.version_vector() == folds.front().version_vector());
      }
    } else if (scenario.has("snapshot")) {
      assert(lazily_test::json_string(lazily_test::json_member(scenario, "snapshot")) ==
             "delta_since({})");
      auto source = TextCrdt::from_str(peer, text);
      const auto snapshot = source.delta_since({});
      TextCrdt restored(lazily_test::json_u64(lazily_test::json_member(scenario, "restore_peer")));
      assert(restored.apply_delta(snapshot));
      lazily_test::AssertionKeys expect(std::string("crdt-tree/algebra.json expect"),
                                        lazily_test::json_member(scenario, "expect"));
      expect.assert_key("restored_text_equal", restored.value() == source.value());
      std::set<OpId> source_ids;
      std::set<OpId> restored_ids;
      for (const auto& op : snapshot)
        source_ids.insert(op.id);
      for (const auto& op : restored.delta_since({}))
        restored_ids.insert(op.id);
      expect.assert_key("op_ids_equal", source_ids == restored_ids);

      if (lazily_test::json_bool(lazily_test::json_member(scenario, "then_concurrent_edit"))) {
        source.insert_str(source.visible_len(), "A");
        restored.insert_str(restored.visible_len(), "B");
        source.merge_from(restored);
        restored.merge_from(source);
        assert(source.value() == restored.value());
        const auto converged_ops = source.delta_since({});
        std::set<OpId> converged_ids;
        for (const auto& op : converged_ops)
          converged_ids.insert(op.id);
        const auto duplicates = converged_ops.size() - converged_ids.size();
        expect.assert_key("later_merge_duplicates", duplicates);
      }
    } else {
      assert(lazily_test::json_string(lazily_test::json_member(scenario, "frontier")) ==
             "version_vector()");
      auto steady = TextCrdt::from_str(peer, text);
      const auto empty = steady.delta_since(steady.version_vector());
      lazily_test::AssertionKeys expect(std::string("crdt-tree/algebra.json expect"),
                                        lazily_test::json_member(scenario, "expect"));
      // The fixture spells the steady-state delta as a literal array, so its
      // length is the claim: a frontier delta carries no ops.
      expect.assert_key_with("delta", [&](const lazily_test::Json& want) {
        return empty.size() == lazily_test::json_array(want).size();
      });
      expect.assert_key("apply_changed", steady.apply_delta(empty));
    }
  }
  REQUIRE_FIXTURES_LOADED(1);
  return 0;
}
