// The lossless-tree corpus `deliver` selector contract.
// #lzspecoutoforderfixtures
//
// WHY THIS IS A TEST AND NOT A COMMENT. lazily-spec 39df4b3 added
// out_of_order_delivery_buffers.json, which delivers a three-op batch in
// strictly REVERSED order (`deliver.order: [2, 1, 0]`) so that every op arrives
// before the op it depends on, and only a dependency buffer can drain it. That
// fixture is destroyed by a runner that re-sorts the listed sequence: with the
// batch handed over in canonical order it applies top-to-bottom, and the same
// fixture was measured GREEN against a library with no buffer at all. It is
// equally destroyed by a runner that splits the sequence into one
// `apply_update` per op, or that clamps an index it cannot address.
//
// None of that is observable from inside a fixture — a fixture cannot assert on
// how its own step was interpreted, and the two selectors it can carry produce
// identical VERDICTS under a wrong interpretation. So the interpretation lives
// in tests/test_lossless_tree_deliver.hpp as a unit, and this file pins it
// against a recording target: the delivered sequence, the call count, and the
// two rejection arms.
//
// NOT A CONFORMANCE SUITE. This file reads no corpus fixture — every `deliver`
// step below is written inline — so it must not register through
// `lazily_add_spec_conformance_test()` (tests/CMakeLists.txt) and does not
// include tests/test_spec_fixture.hpp. It is registered with a bare
// `add_test()`; the configure-time guard added in d052c4f only demands
// registration for targets whose include closure reaches that header's
// skip-or-require seam, which this one does not. The guard greps the raw source
// text, comments included, so this file must not spell that seam function's name
// anywhere — same hazard tests/test_lossless_tree_diff_order.cpp documents.

#include <lazily/lossless_tree_crdt.hpp>

#include "test_json.hpp"
#include "test_lossless_tree_deliver.hpp"
#include "test_require.hpp"

#include <iostream>
#include <string>
#include <vector>

using namespace lazily;
using lazily_test::select_delivery;

static std::string show(const OpId& id) {
  return "(" + std::to_string(id.counter) + "," + std::to_string(id.peer) + ")";
}

static std::string show(const std::vector<OpId>& ids) {
  std::string out = "[";
  for (size_t i = 0; i < ids.size(); ++i) {
    if (i) out += " ";
    out += show(ids[i]);
  }
  return out + "]";
}

static std::vector<OpId> ids_of(const std::vector<TreeOp>& ops) {
  std::vector<OpId> ids;
  for (const auto& op : ops)
    ids.push_back(op.id);
  return ids;
}

// Anything with `apply_update` is a delivery target. This one applies nothing;
// it records WHAT arrived and HOW MANY TIMES, which is exactly what a real
// replica cannot tell us.
struct RecordingTarget {
  std::vector<std::vector<OpId>> calls;

  void apply_update(const TreeUpdate& update) { calls.push_back(ids_of(update.ops)); }
};

static const lazily_test::Json& step(const std::string& json,
                                     std::vector<lazily_test::JsonPtr>& keep) {
  keep.push_back(lazily_test::parse_json(json));
  return *keep.back();
}

static TreeNodeSeed leaf(const std::string& text) {
  return TreeNodeSeedLeaf{LeafKind::Trivia, text};
}

int main() {
  std::vector<lazily_test::JsonPtr> keep;

  // `b` forks before `a` emits the three ops it will owe, so the canonical diff
  // `a.diff(b.frontier())` is exactly those three, in dotted (counter, peer)
  // order (tests/test_lossless_tree_diff_order.cpp pins that ordering).
  LosslessTreeCrdt a(1);
  const OpId para = a.create_node(kTreeRoot, nullptr, TreeNodeSeedElement{"para"});
  LosslessTreeCrdt b = a.fork(2);
  const OpId one = a.create_node(para, nullptr, leaf("1"));
  const OpId two = a.create_node(para, &one, leaf("2"));
  const OpId three = a.create_node(para, &two, leaf("3"));

  const std::vector<TreeOp> canonical = a.diff(b.frontier()).ops;
  const std::vector<OpId> in_order = ids_of(canonical);
  REQUIRE(in_order.size() == 3, "expected a three-op canonical diff, got " + show(in_order));
  REQUIRE(in_order[0] == one && in_order[1] == two && in_order[2] == three,
          "canonical diff is not in (counter, peer) order: " + show(in_order));

  // ── `order` reaches apply_update UNSORTED, in ONE call ─────────────────────
  {
    RecordingTarget target;
    lazily_test::deliver_selection(canonical, step(R"({"order": [2, 1, 0]})", keep), target);
    REQUIRE(target.calls.size() == 1,
            "a `deliver` step is ONE apply_update call; a runner that splits it hands the "
            "receiver several batches and the buffering fixture stops discriminating. Got " +
                std::to_string(target.calls.size()) + " call(s)");
    const std::vector<OpId>& got = target.calls[0];
    REQUIRE(got.size() == 3, "expected three ops delivered, got " + show(got));
    REQUIRE(got[0] == three && got[1] == two && got[2] == one,
            "`order` must reach apply_update in the LISTED sequence, unsorted. Expected " +
                show({three, two, one}) + ", got " + show(got));
    // Stated separately and positively: what arrived is NOT the canonical order.
    // A refactor that re-sorts would satisfy a weaker "same set" check.
    REQUIRE(got != in_order, "`order` was re-sorted back into canonical order " + show(in_order) +
                                 " — that leaves out_of_order_delivery_buffers.json green "
                                 "against a library with no dependency buffer");
  }

  // ── `only` keeps its existing meaning: a subset, one call ──────────────────
  {
    RecordingTarget target;
    lazily_test::deliver_selection(canonical, step(R"({"only": [0, 2]})", keep), target);
    REQUIRE(target.calls.size() == 1, "`only` is also exactly one apply_update call");
    REQUIRE(target.calls[0] == std::vector<OpId>({one, three}), "`only` [0, 2] should deliver " +
                                                                    show({one, three}) + ", got " +
                                                                    show(target.calls[0]));
  }

  // ── `order` need not be a permutation ──────────────────────────────────────
  {
    RecordingTarget target;
    lazily_test::deliver_selection(canonical, step(R"({"order": [1, 1, 0]})", keep), target);
    REQUIRE(target.calls.size() == 1 && target.calls[0] == std::vector<OpId>({two, two, one}),
            "`order` is a sequence of indexes, not a permutation — repeats and omissions are "
            "delivered as listed");
  }

  // ── An out-of-range index FAILS; it is never clamped or skipped ────────────
  {
    const auto past_end = select_delivery(canonical, step(R"({"order": [0, 3]})", keep));
    REQUIRE(!past_end.ok(), "index 3 into a three-op diff must be rejected, not clamped to 2");
    REQUIRE(past_end.error.find("out of range") != std::string::npos,
            "the rejection should say what went wrong, got: " + past_end.error);
    REQUIRE(past_end.ops.empty(), "a rejected `deliver` step delivers nothing");

    const auto negative = select_delivery(canonical, step(R"({"order": [-1]})", keep));
    REQUIRE(!negative.ok(), "a negative index must be rejected, not wrapped");

    const auto only_past_end = select_delivery(canonical, step(R"({"only": [0, 9]})", keep));
    REQUIRE(!only_past_end.ok(), "`only` bounds are checked on the same terms as `order`");
  }

  // ── Exactly one selector: both or neither is a hard error ──────────────────
  {
    const auto both = select_delivery(canonical, step(R"({"only": [0], "order": [0]})", keep));
    REQUIRE(!both.ok(), "a step carrying BOTH `only` and `order` must be rejected, not resolved "
                        "by silently preferring one");
    REQUIRE(both.error.find("BOTH") != std::string::npos,
            "the rejection should name the conflict, got: " + both.error);

    const auto neither = select_delivery(canonical, step(R"({"from": "a", "to": "b"})", keep));
    REQUIRE(!neither.ok(), "a `deliver` step with no selector must be rejected, not treated as "
                           "a full sync");
    REQUIRE(neither.error.find("NEITHER") != std::string::npos,
            "the rejection should say a selector is required, got: " + neither.error);

    const auto not_a_list = select_delivery(canonical, step(R"({"order": 2})", keep));
    REQUIRE(!not_a_list.ok(), "a non-array selector must be rejected");
  }

  std::cout << "test_lossless_tree_deliver_order: ok (canonical " << show(in_order)
            << ", order [2,1,0] delivered unsorted in one call)\n";
  return 0;
}
