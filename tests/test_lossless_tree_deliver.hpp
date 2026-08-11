// Interpretation of a lossless-tree corpus `deliver` step's selector.
// #lzspecoutoforderfixtures
//
// A `deliver` step withholds part of an anti-entropy batch. Both of its
// selectors index POSITIONALLY into the same list: the canonical diff, i.e.
// `from.diff(to.frontier())`, which every binding returns sorted by dotted
// `(counter, peer)` (this binding's sort landed in c4b26b3, pinned by
// tests/test_lossless_tree_diff_order.cpp).
//
//   * `only`  — deliver this SUBSET. Existing meaning, unchanged.
//   * `order` — deliver these entries IN THE LISTED SEQUENCE. Not necessarily a
//               permutation, and deliberately NOT re-sorted.
//
// Exactly one of the two is present; both or neither is a hard error rather
// than a silent preference, because a fixture that carries both means something
// the corpus has not defined.
//
// WHY `order` MUST NOT BE RE-SORTED. `out_of_order_delivery_buffers.json` exists
// to discriminate a dependency buffer: it delivers a three-op batch strictly
// reversed, so every op arrives before the op it depends on. A binding that
// sorts `order` back into canonical order was measured GREEN on that fixture
// against a library with NO buffer at all — the re-sort hands the receiver a
// batch it can apply top-to-bottom, and the fixture stops testing anything. The
// selection therefore preserves the listed sequence exactly, and hands it to
// `apply_update` as ONE call: splitting it into per-op calls would also destroy
// the fixture, since each single-op call would drain what it could and the
// undeliverable ops would arrive as separate batches.
//
// WHY THE INDEX IS NOT CLAMPED. An out-of-range index means the fixture and the
// diff disagree about what the batch contains — the exact drift `only`/`order`
// are positional in order to catch. Clamping (or skipping) it would deliver a
// DIFFERENT batch and still report the scenario's verdict, so it is an error.
//
// The runner cannot assert on any of this from a fixture: a fixture cannot
// observe how its own step was interpreted. So the selection is a separate,
// directly testable unit and `deliver_selection` is a template over the target —
// tests/test_lossless_tree_deliver_order.cpp passes a recording stub and pins
// the call count and the delivered sequence.
//
// NOT A CORPUS READER. This header parses a step object it is handed; it never
// opens a fixture and never includes tests/test_spec_fixture.hpp, so a target
// whose include closure reaches only this header stays outside the configure-time
// registration guard (tests/CMakeLists.txt, d052c4f). That guard greps raw
// source text INCLUDING COMMENTS for the seam function's name, so this file must
// not spell it — same hazard the diff-order test documents.

#ifndef LAZILY_TESTS_TEST_LOSSLESS_TREE_DELIVER_HPP
#define LAZILY_TESTS_TEST_LOSSLESS_TREE_DELIVER_HPP

#include <lazily/lossless_tree_crdt.hpp>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "test_json.hpp"
#include "test_require.hpp"

namespace lazily_test {

// The ops a `deliver` step selects, in the sequence it selects them, or the
// reason the step is invalid. Errors are RETURNED rather than asserted so the
// direct test can assert on them; the runner turns a non-empty `error` into a
// fixture failure.
struct DeliverySelection {
  std::vector<lazily::TreeOp> ops;
  std::string error;

  bool ok() const { return error.empty(); }
};

inline DeliverySelection delivery_selection_error(const std::string& reason) {
  DeliverySelection out;
  out.error = reason;
  return out;
}

// `canonical` is the canonical diff — `from.diff(to.frontier())`, already in
// dotted (counter, peer) order.
inline DeliverySelection select_delivery(const std::vector<lazily::TreeOp>& canonical,
                                         const Json& deliver) {
  const Json* only = deliver.find("only");
  const Json* order = deliver.find("order");
  if (only != nullptr && order != nullptr)
    return delivery_selection_error(
        "deliver step carries BOTH `only` and `order`; exactly one selector is allowed");
  const Json* selector = only != nullptr ? only : order;
  if (selector == nullptr)
    return delivery_selection_error(
        "deliver step carries NEITHER `only` nor `order`; exactly one selector is required");
  if (!selector->is_array())
    return delivery_selection_error(std::string("deliver `") +
                                    (only != nullptr ? "only" : "order") +
                                    "` is not an array of indexes");

  DeliverySelection out;
  for (const auto& index : selector->array) {
    const long long raw = index->as_int();
    if (raw < 0 || static_cast<std::size_t>(raw) >= canonical.size())
      return delivery_selection_error(
          "deliver index " + std::to_string(raw) + " is out of range for a canonical diff of " +
          std::to_string(canonical.size()) +
          " op(s) — the step selects an op the diff does not contain, and an out-of-range "
          "index is a mismatch to report, never one to clamp");
    out.ops.push_back(canonical[static_cast<std::size_t>(raw)]);
  }
  return out;
}

// Hand the selection to `target` as EXACTLY ONE `apply_update` call, in the
// listed sequence. `Target` is anything with `apply_update(const TreeUpdate&)`:
// a `LosslessTreeCrdt` in the runner, a recording stub in the direct test.
template <typename Target>
inline void deliver_selection(const std::vector<lazily::TreeOp>& canonical, const Json& deliver,
                              Target& target) {
  DeliverySelection selected = select_delivery(canonical, deliver);
  REQUIRE(selected.ok(), selected.error.c_str());
  lazily::TreeUpdate update;
  update.ops = std::move(selected.ops);
  target.apply_update(update);
}

} // namespace lazily_test

#endif // LAZILY_TESTS_TEST_LOSSLESS_TREE_DELIVER_HPP
