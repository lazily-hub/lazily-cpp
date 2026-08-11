// `LosslessTreeCrdt::apply_update` buffers an op whose dependency has not
// arrived and retries it. #lzcppapplyupdatelamport
//
// THE CONTRACT, and where it comes from. lazily-spec docs/lossless-tree-crdt.md:
// "`apply_update` is idempotent (already-held ops are skipped) and buffers ops
// whose parent/target or `prev` has not arrived yet", and, on the per-leaf text
// chain, "each `LeafEdit`/`SplitLeaf`/`MergeLeaves` carries the prior text-op id
// (`prev`) and is buffered until it arrives, keeping out-of-order delivery
// convergent." lazily-rs, -go, -py, -zig and -js all buffer and re-drain.
// lazily-cpp DECLARED a `buffered_` member and never wrote to it: every op was
// applied on arrival, and `apply_op`'s existence guards silently dropped the ones
// whose target was missing.
//
// WHY THIS IS A TEST AND NOT A COMMENT. The corpus cannot see it.
// `non_contiguous_anti_entropy.json` is the only fixture that delivers a gap, and
// the ops it withholds are siblings — every DELIVERED op's parent is already
// present on the receiver, so applying them in arrival order needs no buffer.
//
// WHY THIS TEST CANNOT PASS BY ACCIDENT. The batch is delivered in strictly
// reversed dotted order, so a child arrives before its parent and a leaf edit
// arrives before the op that creates the leaf it edits. The dropped op is not
// merely delayed: `record` observes it into the frontier at the same moment, so
// the receiver believes it holds an op it threw away and NO later diff re-requests
// it. The test asserts that too — a re-diff owing zero ops is what makes the loss
// permanent rather than eventually-repaired.
//
// A note on why creates alone would not discriminate: this binding's `apply_op`
// pushes a new node into `children_[parent]` whether or not the parent exists
// yet, so a create delivered ahead of its parent is accidentally recovered when
// the parent lands. The leaf edit is the op that is genuinely destroyed, which is
// why the scenario carries all three.
//
// NOT A CONFORMANCE SUITE. This file reads no corpus fixture, so it must not
// register through `lazily_add_spec_conformance_test()` (tests/CMakeLists.txt)
// and does not include tests/test_spec_fixture.hpp. It is registered with a bare
// `add_test()`; the configure-time guard added in d052c4f only demands
// registration for targets whose include closure reaches that header's
// skip-or-require seam, which this one does not. The guard greps raw source text
// INCLUDING COMMENTS, so this file must not spell that seam function's name
// anywhere — naming it here would make an ordinary unit test look like a corpus
// reader and fail configure. Same class of hazard as 511d572.

#include <lazily/lazily.hpp>

#include "test_require.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

using namespace lazily;

static std::string show(const OpId& id) {
  return "(" + std::to_string(id.counter) + "," + std::to_string(id.peer) + ")";
}

static std::string show(const std::vector<TreeOp>& ops) {
  std::string out = "[";
  for (size_t i = 0; i < ops.size(); ++i) {
    if (i) out += " ";
    out += show(ops[i].id);
  }
  return out + "]";
}

int main() {
  LosslessTreeCrdt a(1);
  auto para = a.create_node(kTreeRoot, nullptr, TreeNodeSeedElement{"para"});

  // `b` forks holding only the paragraph shell. Everything below it is delivered
  // in one reversed batch.
  auto b = a.fork(2);

  auto outer = a.create_node(para, nullptr, TreeNodeSeedElement{"wrap"});
  auto inner = a.create_node(outer, nullptr, TreeNodeSeedLeaf{LeafKind::Raw, "deep"});
  a.edit_leaf(inner, 4, 0, "X");
  REQUIRE(a.render() == "deepX", "a should render deepX, got " + a.render());

  auto update = a.diff(b.frontier());
  REQUIRE(update.ops.size() == 3, "a should owe b three ops, got " + show(update.ops));
  auto edit_id = update.ops.back().id;

  // ── The delivery order is asserted EXPLICITLY, before the outcome is checked ─
  //
  // Reversing a canonically ordered diff is what puts the child strictly before
  // its parent. If a future change made `diff` emit something already reversed
  // this must fail HERE, loudly, rather than quietly delivering in dependency
  // order and testing nothing.
  std::reverse(update.ops.begin(), update.ops.end());
  size_t outer_at = update.ops.size(), inner_at = update.ops.size(), edit_at = update.ops.size();
  for (size_t i = 0; i < update.ops.size(); ++i) {
    if (update.ops[i].id == outer) outer_at = i;
    if (update.ops[i].id == inner) inner_at = i;
    if (update.ops[i].id == edit_id) edit_at = i;
  }
  REQUIRE(inner_at < outer_at, "the child " + show(inner) +
                                   " must be delivered strictly BEFORE "
                                   "its parent " +
                                   show(outer) + ", got " + show(update.ops));
  REQUIRE(edit_at < inner_at, "the leaf edit " + show(edit_id) +
                                  " must be delivered strictly BEFORE the op that creates the "
                                  "leaf it edits " +
                                  show(inner) + ", got " + show(update.ops));
  REQUIRE(!b.frontier().contains(outer) && !b.frontier().contains(inner) &&
              !b.frontier().contains(edit_id),
          "b must hold none of the batch before delivery, or the reordering is moot");

  b.apply_update(update);

  // ── The contract: order-tolerant delivery still converges ──────────────────
  REQUIRE(b.render() == "deepX", "b lost an op delivered ahead of its dependency: b rendered " +
                                     b.render() + ", expected deepX (the leaf edit " +
                                     show(edit_id) + " arrived before " + show(inner) + ")");
  REQUIRE(b.live_node_count() == a.live_node_count(),
          "b should hold the same live nodes as a, got " + std::to_string(b.live_node_count()) +
              " vs " + std::to_string(a.live_node_count()));

  // ── …and the loss it prevents would have been PERMANENT ────────────────────
  //
  // An op applied against a missing target is dropped by `apply_op`, but it is
  // observed into the frontier all the same, so anti-entropy never offers it
  // again. This assertion states that fact positively: after delivery b owes and
  // is owed nothing, so "eventually it syncs again" is not a repair path.
  REQUIRE(a.diff(b.frontier()).ops.empty(),
          "a should owe b nothing after the batch, got " + show(a.diff(b.frontier()).ops));
  REQUIRE(b.frontier().contains(edit_id),
          "b's frontier should record the leaf edit " + show(edit_id) + " as held");

  std::cout << "test_lossless_tree_apply_update_buffering: ok " << show(update.ops) << " -> ["
            << b.render() << "]\n";
  return 0;
}
