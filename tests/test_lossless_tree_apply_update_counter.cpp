// `LosslessTreeCrdt::apply_update` advances the Lamport counter past every
// observed op. #lzcppapplyupdatelamport
//
// THE CONTRACT, and where it comes from. lazily-spec docs/lossless-tree-crdt.md
// ("Identity and clock"): "The counter advances past every observed op, so a
// causally-later write wins last-writer-wins and concurrent ops tiebreak
// deterministically by peer." lazily-rs, -go, -py, -zig and -js all open their
// apply-update loop with `counter = max(counter, op.id.counter)`. lazily-cpp did
// not, so a replica that received a higher-counter remote op and then edited
// locally minted an id BELOW ops it already held.
//
// WHY THIS IS A TEST AND NOT A COMMENT. The shared corpus cannot see it: no
// lossless-tree fixture makes a local edit AFTER receiving a higher-counter
// remote op. Every fixture forks, edits concurrently, and only then syncs, so
// the local ids are all minted before any remote counter is observed and a
// stalled counter is indistinguishable from an advancing one.
//
// WHY THIS TEST CANNOT PASS BY ACCIDENT. It does not assert the counter value.
// It drives the consequence: replica `a` mints a local `Reorder` after applying
// six remote `Reorder`s, and `b` decides whether to accept it by comparing op
// ids (`apply_op`: `op.id > sort_stamp`). With a stalled counter that local id
// sorts BELOW the remote stamp `b` already holds, `b` rejects it, and the two
// replicas render different documents FOREVER — a re-diff owes nothing, because
// both frontiers say the op is held. The pre-fix behaviour is the concrete id
// (4,1) losing to (9,2); the assertions below name both.
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

#include <iostream>
#include <string>

using namespace lazily;

static std::string show(const OpId& id) {
  return "(" + std::to_string(id.counter) + "," + std::to_string(id.peer) + ")";
}

static TreeNodeSeed leaf(const std::string& text) {
  return TreeNodeSeedLeaf{LeafKind::Trivia, text};
}

int main() {
  LosslessTreeCrdt a(1);
  auto para = a.create_node(kTreeRoot, nullptr, TreeNodeSeedElement{"para"});
  auto x = a.create_node(para, nullptr, leaf("x"));
  auto y = a.create_node(para, &x, leaf("y"));
  REQUIRE(a.render() == "xy", "seed should render xy, got " + a.render());

  // `b` forks at counter 3 and runs its clock far ahead by reordering `y`
  // repeatedly. The churn ends with `y` FIRST, so `b`'s document genuinely
  // differs from what `a` will ask for below — without that the final render
  // comparison would hold whether or not the reorder was accepted, and the test
  // would pin nothing.
  auto b = a.fork(2);
  for (int i = 0; i < 6; ++i) {
    if (i % 2 == 0) {
      b.reorder_child(y, &x);
    } else {
      b.reorder_child(y, nullptr);
    }
  }
  REQUIRE(b.render() == "yx", "b's churn must end with y first, got " + b.render());

  auto to_a = b.diff(a.frontier());
  REQUIRE(to_a.ops.size() == 6,
          "b should owe a its six reorders, got " + std::to_string(to_a.ops.size()));
  auto highest_remote = to_a.ops.back().id;
  REQUIRE(highest_remote.counter == 9 && highest_remote.peer == 2,
          "expected b's last reorder to be (9,2), got " + show(highest_remote));

  a.apply_update(to_a);
  REQUIRE(a.render() == "yx", "a should adopt b's order after sync, got " + a.render());

  // ── The local edit that follows a higher-counter remote op ─────────────────
  a.reorder_child(y, &x);
  auto to_b = a.diff(b.frontier());
  REQUIRE(to_b.ops.size() == 1,
          "a should owe b exactly its new reorder, got " + std::to_string(to_b.ops.size()));
  auto local_id = to_b.ops[0].id;

  // The contract, asserted directly: the newly minted id sorts ABOVE everything
  // this replica has observed. Without the advance it is (4,1) — a's counter was
  // still 3 — which sorts below the (9,2) a already holds.
  REQUIRE(local_id.compare(highest_remote) > 0,
          "a minted " + show(local_id) + " after observing " + show(highest_remote) +
              "; a local id must sort above every observed op, or every "
              "last-writer-wins comparison on the partner rejects it");

  // And the consequence, so the assertion above cannot be satisfied vacuously by
  // some future id scheme: the partner accepts the op and the replicas converge.
  REQUIRE(a.render() == "xy", "a's own reorder should render xy, got " + a.render());
  b.apply_update(to_b);
  REQUIRE(b.render() == a.render(),
          "replicas diverged after a's post-sync local edit: a=" + a.render() + " b=" + b.render() +
              " (a's op " + show(local_id) + " lost to b's " + show(highest_remote) + ")");

  // The divergence would be PERMANENT: both frontiers already hold every op, so
  // no amount of further anti-entropy re-requests anything.
  REQUIRE(a.diff(b.frontier()).ops.empty() && b.diff(a.frontier()).ops.empty(),
          "both replicas should owe each other nothing after the exchange");

  std::cout << "test_lossless_tree_apply_update_counter: ok " << show(local_id) << " > "
            << show(highest_remote) << "\n";
  return 0;
}
