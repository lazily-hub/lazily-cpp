#include <lazily/lazily.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>
#include "test_spec_fixture.hpp"

using namespace lazily;

static std::string fixture_text() {
  return lazily_test::spec_fixture_text("crdt-tree", "algebra.json");
}

int main() {
  static_assert(is_crdt_tree_v<TextCrdt>, "TextCrdt is the canonical CrdtTree");
  const auto fixture = fixture_text();
  assert(fixture.find("\"kind\": \"CrdtTree\"") != std::string::npos);
  assert(fixture.find("merge algebra is order and duplication independent") !=
         std::string::npos);
  assert(fixture.find("empty frontier snapshot preserves lineage") !=
         std::string::npos);
  assert(fixture.find("own frontier emits an empty delta") !=
         std::string::npos);

  const auto base = TextCrdt::from_str(1, "root\n");
  auto a = base.fork(2);
  auto b = base.fork(3);
  auto c = base.fork(4);
  a.insert_str(a.visible_len(), "a");
  b.insert_str(b.visible_len(), "b");
  c.insert_str(c.visible_len(), "c");

  std::vector<TextCrdt> folds;
  auto first = base.fork(100);
  first.merge_from(a);
  first.merge_from(b);
  first.merge_from(c);
  folds.push_back(std::move(first));
  auto second = base.fork(101);
  second.merge_from(c);
  second.merge_from(a);
  second.merge_from(b);
  folds.push_back(std::move(second));
  auto third = base.fork(102);
  third.merge_from(b);
  assert(!third.merge_from(b));
  third.merge_from(c);
  third.merge_from(a);
  folds.push_back(std::move(third));
  for (const auto &fold : folds) {
    assert(fold.value() == folds.front().value());
    assert(fold.version_vector() == folds.front().version_vector());
  }

  auto source = TextCrdt::from_str(7, "snapshot\n");
  const auto snapshot = source.delta_since({});
  TextCrdt restored(8);
  assert(restored.apply_delta(snapshot));
  assert(restored.value() == source.value());
  std::set<OpId> source_ids;
  std::set<OpId> restored_ids;
  for (const auto &op : snapshot)
    source_ids.insert(op.id);
  for (const auto &op : restored.delta_since({}))
    restored_ids.insert(op.id);
  assert(source_ids == restored_ids);

  source.insert_str(source.visible_len(), "A");
  restored.insert_str(restored.visible_len(), "B");
  source.merge_from(restored);
  restored.merge_from(source);
  assert(source.value() == restored.value());
  const auto converged_ops = source.delta_since({});
  std::set<OpId> converged_ids;
  for (const auto &op : converged_ops)
    converged_ids.insert(op.id);
  assert(converged_ids.size() == converged_ops.size());

  auto steady = TextCrdt::from_str(9, "steady\n");
  const auto empty = steady.delta_since(steady.version_vector());
  assert(empty.empty());
  assert(!steady.apply_delta(empty));
  REQUIRE_FIXTURES_LOADED(1);
  return 0;
}
