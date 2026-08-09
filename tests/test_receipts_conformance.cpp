// Causal-receipt conformance, replayed from the canonical lazily-spec bytes.
//
// `conformance/receipts/` holds one fixture and lazily-cpp had NO runner for it
// in any form — the area was not covered, hand-transcribed or otherwise. This
// runner folds the fixture's ACTUAL `wire.CausalReceipts.receipts` array through
// `ReceiptProjection` and asserts every entry in the fixture's `assertions`
// block: the receipt count, the converged generation, the causation id's
// terminal outcome, which receipt ids were classified stale, and that the
// outcomes the spec calls non-terminal really are non-terminal here.
//
// Idempotence is asserted too — re-observing the whole frame must record nothing
// new and must not move the generation — because a projection that double-counts
// is indistinguishable from a correct one on a single pass.
//
// ## No silent defaults
//
// `outcome_of` hard-fails on an unknown spelling rather than defaulting, and
// every key in the `assertions` block must be one this runner actually checks:
// a corpus that grows an assertion must not be silently half-verified.
//
// ## Positive assertion
//
// `REQUIRE_FIXTURES_LOADED(1)` proves the canonical file was opened, and
// `g_checks` proves the replay did real work.

#include <lazily/core.hpp>
#include <lazily/receipt.hpp>

#include <algorithm>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "test_assertion_keys.hpp"
#include "test_json.hpp"
#include "test_require.hpp"
#include "test_spec_fixture.hpp"

using namespace lazily;
using lazily_test::Json;
using lazily_test::JsonPtr;
using lazily_test::parse_json;

static const char* kArea = "receipts";
static const char* kFixture = "causal_receipts.json";

static size_t g_receipts_replayed = 0;
static size_t g_checks = 0;

// Outcome spellings are wire data shared by every binding. An unknown spelling
// aborts: silently resolving it to some default would fold the wrong receipt and
// still report green.
static ReceiptOutcome outcome_of(const std::string& spelling) {
  if (spelling == "observed") return ReceiptOutcome::Observed;
  if (spelling == "accepted") return ReceiptOutcome::Accepted;
  if (spelling == "applied") return ReceiptOutcome::Applied;
  if (spelling == "rejected") return ReceiptOutcome::Rejected;
  REQUIRE(false, "unknown receipt outcome spelling in fixture");
  return ReceiptOutcome::Observed; // unreachable
}

static std::vector<std::string> str_array(const Json* node) {
  REQUIRE(node != nullptr && node->is_array(), "expected a JSON array");
  std::vector<std::string> out;
  for (const auto& item : node->array) {
    REQUIRE(item->type == Json::Type::String, "expected an array of strings");
    out.push_back(item->str);
  }
  return out;
}

static std::string require_str(const Json* obj, const char* key) {
  const Json* node = obj->find(key);
  REQUIRE(node != nullptr && node->type == Json::Type::String,
          "fixture receipt is missing a required string field");
  (void)key;
  return node->str;
}

// `reason` / `payload_hash` are explicitly `null` in the corpus when unset. A
// MISSING key is a different thing and is rejected: absence would otherwise be
// indistinguishable from an explicit null, hiding a renamed field.
static std::optional<std::string> require_nullable(const Json* obj, const char* key) {
  const Json* node = obj->find(key);
  REQUIRE(node != nullptr, "fixture receipt is missing a nullable field — a renamed key must not "
                           "read as an absent value");
  if (node->is_null()) return std::nullopt;
  REQUIRE(node->type == Json::Type::String, "nullable field must be a string or null");
  (void)key;
  return node->str;
}

template <typename T> static void check_eq(const char* what, const T& actual, const T& expected) {
  ++g_checks;
  if (actual == expected) return;
  std::cout << "FAIL: " << what << ": expected " << expected << ", got " << actual << std::endl;
  std::abort();
}

int main() {
  // All four spellings resolve, so a corpus exercising only some of them still
  // pins the mapper. (The reject path aborts by design and is not asserted.)
  REQUIRE(outcome_of("observed") == ReceiptOutcome::Observed &&
              outcome_of("accepted") == ReceiptOutcome::Accepted &&
              outcome_of("applied") == ReceiptOutcome::Applied &&
              outcome_of("rejected") == ReceiptOutcome::Rejected,
          "receipt outcome spellings must map one-to-one");

  const std::string text = lazily_test::spec_fixture_text(kArea, kFixture);
  const JsonPtr fixture = parse_json(text);
  REQUIRE(fixture->is_object(), "fixture root is not an object");

  const Json* model = fixture->find("model");
  REQUIRE(model != nullptr && model->str == "CausalReceipt",
          "fixture is not a CausalReceipt corpus");

  const Json* assertions = fixture->find("assertions");
  REQUIRE(assertions != nullptr && assertions->is_object(), "fixture has no assertions block");

  const Json* wire = fixture->find("wire");
  REQUIRE(wire != nullptr, "fixture has no wire block");
  const Json* envelope = wire->find("CausalReceipts");
  REQUIRE(envelope != nullptr, "wire block is not a CausalReceipts envelope");
  const Json* receipts = envelope->find("receipts");
  REQUIRE(receipts != nullptr && receipts->is_array() && !receipts->array.empty(),
          "the CausalReceipts envelope carries no receipts");

  // The block is BOUND to the tracker here and asserted at the end of the fold.
  //
  // It was previously read through a hand-written `is_known_assertion` allowlist
  // — the exact shape `AssertionKeys` exists to replace. That allowlist covered
  // rung 2 (every key is READ) and nothing above it: a key read and never
  // compared, or compared against the fixture's own structure, passed. It is
  // also the shape that made this fixture invisible to every guard the family
  // has, since all of them are scoped to blocks a runner already bound
  // (`#lznullformblind`).
  lazily_test::AssertionKeys keys(std::string(kArea) + "/" + kFixture + " assertions", *assertions);

  // Two SELECTORS, consumed here and asserted below against the fold they drove.
  const int64_t current_generation = keys.required("current_generation").as_int();
  const Json& causation = keys.required("causation_id");
  REQUIRE(causation.type == Json::Type::String, "assertions.causation_id must be a string");
  const std::string want_causation_id = causation.str;

  // Fold the frame. `current_generation` is the projection's notion of "now": a
  // receipt stamped with any other generation is stale.
  ReceiptProjection projection;
  std::vector<std::string> classified_stale;
  std::set<std::string> observed_causation_ids; // read off the folded receipts
  size_t recorded = 0;
  for (const auto& entry : receipts->array) {
    REQUIRE(entry->is_object(), "a receipt is not an object");
    CausalReceipt receipt;
    receipt.receipt_id = require_str(entry.get(), "receipt_id");
    receipt.causation_id = require_str(entry.get(), "causation_id");
    observed_causation_ids.insert(receipt.causation_id);
    receipt.observer = require_str(entry.get(), "observer");
    const Json* gen = entry->find("generation");
    REQUIRE(gen != nullptr && gen->type == Json::Type::Number,
            "a receipt is missing its generation — the stale discriminator must "
            "never be defaulted");
    receipt.generation = gen->as_int();
    receipt.outcome = outcome_of(require_str(entry.get(), "outcome"));
    receipt.reason = require_nullable(entry.get(), "reason");
    receipt.payload_hash = require_nullable(entry.get(), "payload_hash");

    const ReceiptApplyStatus status = projection.observe(current_generation, receipt);
    ++g_receipts_replayed;
    if (std::holds_alternative<ReceiptStaleGeneration>(status)) {
      classified_stale.push_back(receipt.receipt_id);
      const auto& stale = std::get<ReceiptStaleGeneration>(status);
      check_eq("stale receipt expected generation", stale.expected, current_generation);
      check_eq("stale receipt actual generation", stale.actual, receipt.generation);
    } else {
      REQUIRE(std::holds_alternative<ReceiptRecorded>(status),
              "a non-stale receipt in the canonical frame was neither recorded "
              "nor classified stale");
      ++recorded;
      ++g_checks;
      REQUIRE(projection.contains_receipt(receipt.receipt_id),
              "a recorded receipt is not retrievable from the projection");
    }
  }

  // `#lznullformblind`. This compared `assertions.receipt_count` against
  // `wire.CausalReceipts.receipts.size()` — the fixture measured against ITSELF.
  // Delete the fold loop above entirely and it still passed, because neither
  // side of the comparison had anything to do with the run. It is now the
  // receipts the projection actually FOLDED.
  keys.assert_key("receipt_count", static_cast<int64_t>(g_receipts_replayed));
  ++g_checks;
  // The fixture's own two halves still have to agree with each other, which is a
  // separate fact from "the run folded that many" and is stated separately.
  check_eq("receipt_count (frame size)", static_cast<long long>(receipts->array.size()),
           static_cast<long long>(assertions->find("receipt_count")->as_int()));

  const std::vector<std::string> want_stale = str_array(assertions->find("stale_receipt_ids"));
  std::vector<std::string> got_stale = classified_stale;
  std::sort(got_stale.begin(), got_stale.end());
  std::vector<std::string> sorted_want = want_stale;
  std::sort(sorted_want.begin(), sorted_want.end());
  ++g_checks;
  // Against the ids the fold really CLASSIFIED stale, in both directions.
  keys.assert_key_with("stale_receipt_ids", [&](const Json& want) {
    std::vector<std::string> declared;
    for (const auto& element : lazily_test::json_array(want))
      declared.push_back(lazily_test::json_string(*element));
    std::sort(declared.begin(), declared.end());
    return declared == got_stale;
  });

  // The projection's own stale set must agree with the per-receipt statuses.
  std::vector<std::string> projection_stale = projection.stale_receipt_ids();
  std::sort(projection_stale.begin(), projection_stale.end());
  ++g_checks;
  REQUIRE(projection_stale == sorted_want,
          "the projection's stale set disagrees with its per-receipt statuses");

  // Non-stale receipts are the ones the projection retains.
  check_eq("recorded receipts", static_cast<long long>(projection.receipt_count()),
           static_cast<long long>(receipts->array.size() - want_stale.size()));
  check_eq("recorded receipts (loop count)", static_cast<long long>(recorded),
           static_cast<long long>(receipts->array.size() - want_stale.size()));

  // The generation the PROJECTION converged on, not the selector it was fed.
  keys.assert_key("current_generation", projection.current_generation());
  ++g_checks;

  const auto terminal = projection.terminal_for(want_causation_id);
  ++g_checks;
  // `causation_id` selects which chain to fold, so it is asserted against the
  // fold it selected: the projection really carries a terminal for that chain,
  // and the receipts really named it. A selector compared against nothing is the
  // label-as-assertion shape (`#lznullformblind`).
  keys.assert_key_with("causation_id", [&](const Json& want) {
    return terminal.has_value() && lazily_test::json_string(want) == want_causation_id &&
           observed_causation_ids.count(want_causation_id) != 0;
  });
  ++g_checks;
  keys.assert_key_with("terminal_outcome", [&](const Json& want) {
    const ReceiptOutcome declared = outcome_of(lazily_test::json_string(want));
    // Two facts: the FOLDED terminal is that outcome, and this binding really
    // classifies it terminal.
    return terminal.has_value() && terminal->outcome == declared && is_terminal(declared);
  });
  ++g_checks;
  keys.assert_key_with("nonterminal_outcomes", [&](const Json& want) {
    for (const auto& element : lazily_test::json_array(want)) {
      if (is_terminal(outcome_of(lazily_test::json_string(*element)))) return false;
    }
    return true;
  });
  ++g_checks;
  keys.finish();

  // State-based idempotence: re-observing the identical frame records nothing
  // new and does not move the generation.
  const int before_count = projection.receipt_count();
  const int64_t before_gen = projection.current_generation();
  for (const auto& entry : receipts->array) {
    CausalReceipt receipt;
    receipt.receipt_id = require_str(entry.get(), "receipt_id");
    receipt.causation_id = require_str(entry.get(), "causation_id");
    receipt.observer = require_str(entry.get(), "observer");
    receipt.generation = entry->find("generation")->as_int();
    receipt.outcome = outcome_of(require_str(entry.get(), "outcome"));
    const ReceiptApplyStatus status = projection.observe(current_generation, receipt);
    ++g_checks;
    REQUIRE(std::holds_alternative<ReceiptDuplicate>(status),
            "re-observing a receipt already in the projection must be a "
            "duplicate, not a new record");
  }
  check_eq("receipt count after re-delivery", projection.receipt_count(), before_count);
  check_eq("generation after re-delivery", projection.current_generation(), before_gen);

  REQUIRE(g_receipts_replayed == receipts->array.size(),
          "not every receipt in the canonical frame was folded");
  REQUIRE(g_receipts_replayed >= 4 && g_checks >= 20,
          "the receipts replay did too little work to be meaningful — the "
          "corpus is empty, truncated, or short-circuited");

  std::cout << "receipts conformance: " << g_receipts_replayed << " receipts folded, " << g_checks
            << " assertions" << std::endl;

  REQUIRE_FIXTURES_LOADED(1);
  return 0;
}
