// `LosslessTreeCrdt::diff` returns ops in canonical (counter, peer) order.
// #lzdifforderallbindings
//
// WHY THIS IS A TEST AND NOT A COMMENT. The order is a cross-binding contract
// because the shared corpus addresses the returned ops POSITIONALLY:
// lazily-spec/conformance/lossless-tree/non_contiguous_anti_entropy.json says
// `deliver.only: [0, 2]`, and those indices index into whatever `diff` returns.
// The fixture therefore only means the same thing in every binding while every
// binding returns the same order.
//
// The corpus cannot police that. Measured in lazily-zig (commit e8a2a28,
// #lzzigdiffmutant): replacing the sort with a reverse, or deleting it outright,
// left the ENTIRE conformance suite green — including this very fixture, because
// the two indices select the same SET either way and applying an update is
// order-tolerant by design. Only a direct test can pin the order.
//
// NOT A CONFORMANCE SUITE. This file reads no corpus fixture, so it must not
// register through `lazily_add_spec_conformance_test()` (tests/CMakeLists.txt)
// and does not include tests/test_spec_fixture.hpp. It is registered with a bare
// `add_test()`; the configure-time guard added in d052c4f only demands
// registration for targets whose include closure reaches that header's
// skip-or-require seam, which this one does not.
//
// The guard greps the raw source text, comments included, so this file must not
// spell the seam function's name anywhere — naming it here would make an
// ordinary unit test look like a corpus reader and fail configure. Same class of
// hazard as 511d572, which stopped sources spelling the corpus root.

#include <lazily/lazily.hpp>

#include "test_require.hpp"

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

static TreeNodeSeed leaf(const std::string& text) {
  return TreeNodeSeedLeaf{LeafKind::Trivia, text};
}

int main() {
  LosslessTreeCrdt a(1);
  auto para = a.create_node(kTreeRoot, nullptr, TreeNodeSeedElement{"para"});
  auto base = a.create_node(para, nullptr, leaf("0"));

  auto b = a.fork(2);

  // `a` runs ahead to counter 4; `b`'s single op stays at counter 3, so it
  // arrives at `a` AFTER (4,1) while sorting BEFORE it. Without that inversion
  // the log already happens to be in canonical order and the ordering assertion
  // at the bottom would hold for an unsorted or reversed `diff` too — it would
  // pin nothing.
  auto one = a.create_node(para, &base, leaf("1"));
  auto two = a.create_node(para, &one, leaf("2"));
  auto remote = b.create_node(para, &base, leaf("9"));

  auto from_b = b.diff(a.frontier());
  REQUIRE(from_b.ops.size() == 1 && from_b.ops[0].id == remote,
          "b should owe a exactly its own op, got " + show(from_b.ops));

  // ── The difference is asserted EXPLICITLY, before the order is checked ──────
  //
  // A refactor that makes arrival order and canonical order coincide must fail
  // HERE, loudly, rather than silently hollowing out the check below.
  //
  // Arrival order is observable through the frontier: `record` observes an op
  // into the frontier at the same moment it appends it to the log, so an id the
  // frontier lacks cannot already be in the log. At this point `a` holds (4,1)
  // and lacks (3,2) — so (3,2) enters a's log strictly AFTER (4,1) does.
  const auto& before = a.frontier();
  REQUIRE(before.contains(two), "a should already hold its own " + show(two));
  REQUIRE(!before.contains(remote),
          "a should not yet hold " + show(remote) + " — it has not been applied");
  // ... yet canonically (3,2) sorts BEFORE (4,1). Arrival and canonical differ.
  REQUIRE(remote.compare(two) < 0, "the later-arriving op " + show(remote) +
                                       " must sort EARLIER than " + show(two) +
                                       ", or this test pins nothing");

  a.apply_update(from_b);

  TreeVersionFrontier empty;
  auto all = a.diff(empty);
  REQUIRE(all.ops.size() == 5,
          "diff against an empty frontier returns the whole log, got " + show(all.ops));

  // ── The contract: strictly increasing by (counter, peer) ───────────────────
  for (size_t i = 1; i < all.ops.size(); ++i) {
    REQUIRE(all.ops[i - 1].id.compare(all.ops[i].id) < 0,
            "diff ops are not in canonical (counter, peer) order: " + show(all.ops));
  }

  // And the inversion actually shows up in the output: the op that arrived LAST
  // is emitted before the local op that arrived before it.
  size_t remote_at = all.ops.size(), two_at = all.ops.size();
  for (size_t i = 0; i < all.ops.size(); ++i) {
    if (all.ops[i].id == remote) remote_at = i;
    if (all.ops[i].id == two) two_at = i;
  }
  REQUIRE(remote_at < two_at, "sorted diff must place " + show(remote) + " before " + show(two) +
                                  ", got " + show(all.ops));

  std::cout << "test_lossless_tree_diff_order: ok " << show(all.ops) << "\n";
  return 0;
}
