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

#include <lazily/lazily.hpp>

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

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
  return ReceiptOutcome::Observed;  // unreachable
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
static std::optional<std::string> require_nullable(const Json* obj,
                                                   const char* key) {
  const Json* node = obj->find(key);
  REQUIRE(node != nullptr,
          "fixture receipt is missing a nullable field — a renamed key must not "
          "read as an absent value");
  if (node->is_null()) return std::nullopt;
  REQUIRE(node->type == Json::Type::String, "nullable field must be a string or null");
  (void)key;
  return node->str;
}

// Assertion keys this runner verifies. Anything else means the corpus grew a
// property nobody checks.
static bool is_known_assertion(const std::string& key) {
  return key == "receipt_count" || key == "current_generation" ||
         key == "causation_id" || key == "terminal_outcome" ||
         key == "stale_receipt_ids" || key == "nonterminal_outcomes";
}

template <typename T>
static void check_eq(const char* what, const T& actual, const T& expected) {
  ++g_checks;
  if (actual == expected) return;
  std::cout << "FAIL: " << what << ": expected " << expected << ", got "
            << actual << std::endl;
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
  REQUIRE(assertions != nullptr && assertions->is_object(),
          "fixture has no assertions block");
  for (const auto& kv : assertions->object)
    REQUIRE(is_known_assertion(kv.first),
            "unrecognised receipts assertion key in fixture — it would be "
            "silently ignored");

  const Json* wire = fixture->find("wire");
  REQUIRE(wire != nullptr, "fixture has no wire block");
  const Json* envelope = wire->find("CausalReceipts");
  REQUIRE(envelope != nullptr, "wire block is not a CausalReceipts envelope");
  const Json* receipts = envelope->find("receipts");
  REQUIRE(receipts != nullptr && receipts->is_array() && !receipts->array.empty(),
          "the CausalReceipts envelope carries no receipts");

  const Json* want_gen = assertions->find("current_generation");
  REQUIRE(want_gen != nullptr, "assertions have no current_generation");
  const int64_t current_generation = want_gen->as_int();

  const Json* want_causation = assertions->find("causation_id");
  REQUIRE(want_causation != nullptr && want_causation->type == Json::Type::String,
          "assertions have no causation_id");

  // Fold the frame. `current_generation` is the projection's notion of "now": a
  // receipt stamped with any other generation is stale.
  ReceiptProjection projection;
  std::vector<std::string> classified_stale;
  size_t recorded = 0;
  for (const auto& entry : receipts->array) {
    REQUIRE(entry->is_object(), "a receipt is not an object");
    CausalReceipt receipt;
    receipt.receipt_id = require_str(entry.get(), "receipt_id");
    receipt.causation_id = require_str(entry.get(), "causation_id");
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

  const Json* want_count = assertions->find("receipt_count");
  REQUIRE(want_count != nullptr, "assertions have no receipt_count");
  check_eq("receipt_count (frame size)",
           static_cast<long long>(receipts->array.size()),
           static_cast<long long>(want_count->as_int()));

  const std::vector<std::string> want_stale =
      str_array(assertions->find("stale_receipt_ids"));
  std::vector<std::string> got_stale = classified_stale;
  std::sort(got_stale.begin(), got_stale.end());
  std::vector<std::string> sorted_want = want_stale;
  std::sort(sorted_want.begin(), sorted_want.end());
  ++g_checks;
  if (got_stale != sorted_want) {
    std::cout << "FAIL: stale_receipt_ids mismatch\n  expected:";
    for (const auto& s : sorted_want) std::cout << " " << s;
    std::cout << "\n  actual:  ";
    for (const auto& s : got_stale) std::cout << " " << s;
    std::cout << std::endl;
    std::abort();
  }

  // The projection's own stale set must agree with the per-receipt statuses.
  std::vector<std::string> projection_stale = projection.stale_receipt_ids();
  std::sort(projection_stale.begin(), projection_stale.end());
  ++g_checks;
  REQUIRE(projection_stale == sorted_want,
          "the projection's stale set disagrees with its per-receipt statuses");

  // Non-stale receipts are the ones the projection retains.
  check_eq("recorded receipts",
           static_cast<long long>(projection.receipt_count()),
           static_cast<long long>(receipts->array.size() - want_stale.size()));
  check_eq("recorded receipts (loop count)", static_cast<long long>(recorded),
           static_cast<long long>(receipts->array.size() - want_stale.size()));

  check_eq("current_generation", projection.current_generation(), current_generation);

  const auto terminal = projection.terminal_for(want_causation->str);
  ++g_checks;
  REQUIRE(terminal.has_value(),
          "the causation id named by the fixture has no terminal receipt");
  const Json* want_terminal = assertions->find("terminal_outcome");
  REQUIRE(want_terminal != nullptr && want_terminal->type == Json::Type::String,
          "assertions have no terminal_outcome");
  ++g_checks;
  REQUIRE(terminal->outcome == outcome_of(want_terminal->str),
          "the folded terminal outcome does not match the fixture");
  ++g_checks;
  REQUIRE(is_terminal(outcome_of(want_terminal->str)),
          "the fixture's terminal_outcome is not classified terminal");

  for (const auto& spelling : str_array(assertions->find("nonterminal_outcomes"))) {
    ++g_checks;
    REQUIRE(!is_terminal(outcome_of(spelling)),
            "an outcome the fixture calls non-terminal is classified terminal");
  }

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

  std::cout << "receipts conformance: " << g_receipts_replayed
            << " receipts folded, " << g_checks << " assertions" << std::endl;

  REQUIRE_FIXTURES_LOADED(1);
  return 0;
}
