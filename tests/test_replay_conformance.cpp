// Replay the canonical replay-equivalence corpus against `lazily/replay.hpp`
// (`#lzreplaycpp`).
//
// Three fixtures, one obligation each
// (`lazily-spec/docs/replay-equivalence.md`): the fingerprint is bound to its
// log and that binding is revalidated before any value compare; a divergence is
// reported at the first checkpoint where the values parted; the observation
// encoding agrees with the family on which differences are differences.
//
// The corpus declares its subjects in PROSE, because a JSON fixture cannot
// carry a reactive graph. `Accumulator` below is this binding's copy of that
// declaration, kept to the letter — including that `observe` exposes `sum` and
// `names` under exactly those labels.
//
// Routing is on the EXCEPTION TYPE, never on a message string: `verify` is
// wrapped in three separate `catch` clauses and each maps to the outcome token
// the fixture names. A driver that matched on text would keep passing after a
// message was reworded and would conflate a stride mismatch with a log
// mismatch, which are the two faults obligation 1 and obligation 2 insist are
// distinct.

#include <lazily/replay.hpp>

#include "test_assertion_keys.hpp"
#include "test_json.hpp"
#include "test_require.hpp"
#include "test_spec_fixture.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace lazily;

static const char* const kFixtureArea = "replay";

// -- the corpus's canonical subjects -----------------------------------------

// `accumulator`, and `drifting_accumulator` when a drift is configured.
//
// `drifting_accumulator(drift_at, drift)` stands in for the one thing a replay
// proof is looking for — a graph that takes a value from OUTSIDE its log —
// with `drift = 0` as the honest run and a non-zero `drift` as the defect.
class Accumulator : public ReplayGraph {
public:
  Accumulator() = default;
  Accumulator(std::int64_t drift_at, std::int64_t drift)
      : drifting_(true), drift_at_(drift_at), drift_(drift) {}

  void apply(const ReplayEvent& event) override {
    REQUIRE(event.payload.kind() == ReplayValue::Kind::Int,
            "the canonical accumulator subject folds an integer payload");
    sum_ += event.payload.int_value();
    names_.push_back(event.name);
    if (drifting_ && event.seq == drift_at_) sum_ += drift_;
  }

  ReplayObservation observe() const override {
    std::vector<ReplayValue> names;
    names.reserve(names_.size());
    for (const auto& name : names_)
      names.push_back(ReplayValue::text(name));
    ReplayObservation observed;
    observed.emplace("sum", ReplayValue::integer(sum_));
    observed.emplace("names", ReplayValue::seq(std::move(names)));
    return observed;
  }

  std::int64_t sum() const { return sum_; }

private:
  std::int64_t sum_ = 0;
  std::vector<std::string> names_;
  bool drifting_ = false;
  std::int64_t drift_at_ = 0;
  std::int64_t drift_ = 0;
};

using Builder = ReplayHarness::Builder;

static Builder subject_builder(const lazily_test::Json& config, bool drifting,
                               std::int64_t drift_at, std::int64_t drift) {
  const std::string subject = lazily_test::json_string(lazily_test::json_member(config, "subject"));
  if (subject == "accumulator")
    return []() -> std::unique_ptr<ReplayGraph> { return std::make_unique<Accumulator>(); };
  REQUIRE(subject == "drifting_accumulator", "unknown canonical replay subject: " + subject);
  REQUIRE(drifting, "the drifting_accumulator subject needs a drift_at in the fixture config");
  return [drift_at, drift]() -> std::unique_ptr<ReplayGraph> {
    return std::make_unique<Accumulator>(drift_at, drift);
  };
}

static ReplayLog log_from_fixture(const lazily_test::Json& entries) {
  std::vector<ReplayEvent> events;
  for (const auto& entry : lazily_test::json_array(entries)) {
    events.emplace_back(
        static_cast<std::int64_t>(lazily_test::json_u64(lazily_test::json_member(*entry, "seq"))),
        lazily_test::json_string(lazily_test::json_member(*entry, "name")),
        ReplayValue::integer(
            static_cast<std::int64_t>(lazily_test::json_member(*entry, "payload").as_int())));
  }
  return ReplayLog(std::move(events));
}

// Replay the log once against a fresh subject and read its `sum` off `observe`.
// This is the DECLARED final state, derived independently of the fingerprint so
// the cross-check below compares two different paths to it.
static std::int64_t declared_final_sum(const Builder& build, const ReplayLog& log) {
  const std::unique_ptr<ReplayGraph> subject = build();
  for (const auto& event : log.events())
    subject->apply(event);
  const ReplayObservation observed = subject->observe();
  const auto sum = observed.find("sum");
  REQUIRE(sum != observed.end(), "the canonical subject must observe a `sum` cell");
  REQUIRE(sum->second.kind() == ReplayValue::Kind::Int, "the observed `sum` must be an integer");
  return sum->second.int_value();
}

static std::vector<long long> checkpoint_seqs(const ReplayFingerprint& fingerprint) {
  std::vector<long long> seqs;
  seqs.reserve(fingerprint.checkpoints().size());
  for (const auto& checkpoint : fingerprint.checkpoints())
    seqs.push_back(static_cast<long long>(checkpoint.seq));
  return seqs;
}

static std::vector<long long> json_int_array(const lazily_test::Json& value) {
  std::vector<long long> out;
  for (const auto& item : lazily_test::json_array(value))
    out.push_back(static_cast<long long>(item->as_int()));
  return out;
}

// -- obligations 1 and 2 ------------------------------------------------------

static void drive_harness_fixture(const std::string& name, std::size_t minimum_steps) {
  const std::string fixture_id = std::string(kFixtureArea) + "/" + name;
  const std::string text = lazily_test::spec_fixture_text(kFixtureArea, name);
  const lazily_test::JsonPtr fixture = lazily_test::parse_json(text);
  REQUIRE(lazily_test::json_string(lazily_test::json_member(*fixture, "kind")) == "Replay",
          fixture_id + ": kind must be Replay");
  REQUIRE(lazily_test::json_string(lazily_test::json_member(*fixture, "model")) == "ReplayHarness",
          fixture_id + ": model must be ReplayHarness");

  const lazily_test::Json& config = lazily_test::json_member(*fixture, "config");
  const bool drifting = config.has("drift_at");
  const std::int64_t drift_at =
      drifting ? static_cast<std::int64_t>(lazily_test::json_member(config, "drift_at").as_int())
               : 0;
  const int config_stride =
      config.has("stride") ? static_cast<int>(lazily_test::json_member(config, "stride").as_int())
                           : 1;

  std::map<std::string, ReplayLog> logs;
  const lazily_test::Json& log_block = lazily_test::json_member(config, "logs");
  REQUIRE(log_block.is_object(), fixture_id + ": config.logs must be an object");
  for (const auto& entry : log_block.object)
    logs.emplace(entry.first, log_from_fixture(*entry.second));

  std::map<std::string, ReplayFingerprint> fingerprints;

  const auto& steps = lazily_test::json_array(lazily_test::json_member(*fixture, "steps"));
  REQUIRE(steps.size() >= minimum_steps,
          fixture_id + ": fixture carries fewer steps than this runner replays");

  for (std::size_t index = 0; index < steps.size(); ++index) {
    const lazily_test::Json& step = *steps[index];
    const lazily_test::Json& op = lazily_test::json_member(step, "op");
    const std::string type = lazily_test::json_string(lazily_test::json_member(op, "type"));
    const std::string where = fixture_id + " step " + std::to_string(index) + " (" + type + ")";

    if (type == "log_digest_equal") {
      const auto left = logs.find(lazily_test::json_string(lazily_test::json_member(op, "left")));
      const auto right = logs.find(lazily_test::json_string(lazily_test::json_member(op, "right")));
      REQUIRE(left != logs.end() && right != logs.end(), where + ": names an undeclared log");
      const bool equal = left->second.digest() == right->second.digest();
      REQUIRE(lazily_test::json_bool(lazily_test::json_member(step, "returns")) == equal,
              where + ": returns");
      // `expected` carries only narration here; binding it keeps the block
      // accounted for rather than silently unread.
      lazily_test::AssertionKeys expected(where + ".expected",
                                          lazily_test::json_member(step, "expected"));
      continue;
    }

    const std::int64_t drift =
        op.has("drift") ? static_cast<std::int64_t>(lazily_test::json_member(op, "drift").as_int())
                        : 0;
    const Builder build = subject_builder(config, drifting, drift_at, drift);
    const int stride = op.has("stride")
                           ? static_cast<int>(lazily_test::json_member(op, "stride").as_int())
                           : config_stride;
    const ReplayHarness harness(build, stride);
    const auto log_it = logs.find(lazily_test::json_string(lazily_test::json_member(op, "log")));
    REQUIRE(log_it != logs.end(), where + ": names an undeclared log");
    const ReplayLog& log = log_it->second;

    lazily_test::AssertionKeys expected(where + ".expected",
                                        lazily_test::json_member(step, "expected"));

    if (type == "record") {
      const ReplayFingerprint fingerprint = harness.record(log);
      fingerprints.insert_or_assign(lazily_test::json_string(lazily_test::json_member(op, "into")),
                                    fingerprint);
      expected.assert_key("outcome", std::string("recorded"));
      expected.assert_key("checkpoint_seqs", checkpoint_seqs(fingerprint), json_int_array);
      expected.assert_key("stride", fingerprint.stride());
      const std::int64_t final_sum = declared_final_sum(build, log);
      expected.assert_key("final_sum", static_cast<long long>(final_sum));
      // The fingerprint must have observed the value the subject ENDS on, not
      // merely some value. Without this the fixture would accept a harness that
      // observed something else entirely and still reported `recorded`: every
      // other key here is about the shape of the checkpoint sequence, and this
      // is the one place the recorded digest and the declared state meet.
      const std::map<std::string, std::string> final_cells =
          fingerprint.final_checkpoint().as_map();
      const auto recorded_sum = final_cells.find("sum");
      REQUIRE(recorded_sum != final_cells.end(),
              where + ": the fingerprint's final checkpoint carries no `sum` cell");
      REQUIRE(recorded_sum->second == canonical_digest(ReplayValue::integer(final_sum)),
              where + ": the recorded `sum` digest is not the subject's final sum");
      continue;
    }

    if (type == "prove") {
      harness.prove(log, static_cast<int>(lazily_test::json_member(op, "replays").as_int()));
      expected.assert_key("outcome", std::string("ok"));
      expected.assert_key("divergences", 0);
      continue;
    }

    const auto pinned =
        fingerprints.find(lazily_test::json_string(lazily_test::json_member(op, "fingerprint")));
    REQUIRE(pinned != fingerprints.end(), where + ": names an unrecorded fingerprint");
    const ReplayFingerprint& fingerprint = pinned->second;

    if (type == "verify") {
      std::string outcome = "ok";
      bool diverged = false;
      ReplayDivergence first;
      try {
        harness.verify(log, fingerprint);
      } catch (const ReplayLogMismatchError&) {
        outcome = "log_mismatch";
      } catch (const ReplayStrideMismatchError&) {
        outcome = "stride_mismatch";
      } catch (const ReplayDivergenceError& error) {
        outcome = "divergent";
        diverged = true;
        first = error.first();
      }
      expected.assert_key("outcome", outcome);
      if (!diverged) {
        expected.assert_key("divergences", 0);
      } else {
        expected.assert_key("first_divergent_seq", static_cast<long long>(first.seq));
        expected.assert_key("first_divergent_label", first.label);
        expected.assert_key("first_divergent_kind", std::string(first.kind_name()));
      }
      continue;
    }

    if (type == "check") {
      // The non-raising reporting form still REFUSES a stale fingerprint: a
      // log mismatch is an unanswerable question, not a report.
      try {
        const std::vector<ReplayDivergence> divergences = harness.check(log, fingerprint);
        expected.assert_key("outcome", std::string("ok"));
        expected.assert_key("divergences", static_cast<long long>(divergences.size()));
      } catch (const ReplayLogMismatchError&) {
        expected.assert_key("outcome", std::string("log_mismatch"));
        expected.assert_key("divergences", 0);
      }
      continue;
    }

    REQUIRE(false, where + ": unknown canonical replay operation");
  }
}

// -- obligation 3 -------------------------------------------------------------

static ReplayValue value_from_fixture(const lazily_test::Json& tagged) {
  const std::string tag = lazily_test::json_string(lazily_test::json_member(tagged, "t"));
  if (tag == "opaque") return ReplayValue::opaque("OpaqueProbe");
  const lazily_test::Json& raw = lazily_test::json_member(tagged, "v");
  if (tag == "int") {
    // Integers cross the fixture as decimal STRINGS so a value beyond 2^53
    // stays exact; parsing the JSON number would round it.
    return ReplayValue::integer(std::stoll(lazily_test::json_string(raw)));
  }
  if (tag == "str") return ReplayValue::text(lazily_test::json_string(raw));
  if (tag == "float") return ReplayValue::floating(std::stod(lazily_test::json_string(raw)));
  if (tag == "bool") return ReplayValue::boolean(lazily_test::json_bool(raw));
  if (tag == "bytes") {
    const std::string hex = lazily_test::json_string(raw);
    REQUIRE(hex.size() % 2 == 0, "a byte string is spelled as hex pairs");
    std::string bytes;
    bytes.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2)
      bytes += static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16));
    return ReplayValue::bytes(std::move(bytes));
  }
  if (tag == "seq" || tag == "set") {
    std::vector<ReplayValue> items;
    for (const auto& item : lazily_test::json_array(raw))
      items.push_back(value_from_fixture(*item));
    return tag == "seq" ? ReplayValue::seq(std::move(items)) : ReplayValue::set(std::move(items));
  }
  if (tag == "map") {
    std::vector<std::pair<ReplayValue, ReplayValue>> entries;
    for (const auto& entry : lazily_test::json_array(raw)) {
      const auto& pair = lazily_test::json_array(*entry);
      REQUIRE(pair.size() == 2, "a map entry is a [key, value] pair");
      entries.emplace_back(ReplayValue::text(lazily_test::json_string(*pair[0])),
                           value_from_fixture(*pair[1]));
    }
    return ReplayValue::map(std::move(entries));
  }
  REQUIRE(false, "unknown canonical value tag: " + tag);
  return ReplayValue::null();
}

static void drive_encoding_fixture(const std::string& name, std::size_t minimum_steps) {
  const std::string fixture_id = std::string(kFixtureArea) + "/" + name;
  const std::string text = lazily_test::spec_fixture_text(kFixtureArea, name);
  const lazily_test::JsonPtr fixture = lazily_test::parse_json(text);
  REQUIRE(lazily_test::json_string(lazily_test::json_member(*fixture, "kind")) == "Replay",
          fixture_id + ": kind must be Replay");
  REQUIRE(lazily_test::json_string(lazily_test::json_member(*fixture, "model")) ==
              "CanonicalEncoding",
          fixture_id + ": model must be CanonicalEncoding");

  const lazily_test::Json& values =
      lazily_test::json_member(lazily_test::json_member(*fixture, "config"), "values");
  REQUIRE(values.is_object(), fixture_id + ": config.values must be an object");

  const auto& steps = lazily_test::json_array(lazily_test::json_member(*fixture, "steps"));
  REQUIRE(steps.size() >= minimum_steps,
          fixture_id + ": fixture carries fewer steps than this runner replays");

  std::set<bool> outcomes;
  for (std::size_t index = 0; index < steps.size(); ++index) {
    const lazily_test::Json& step = *steps[index];
    const lazily_test::Json& op = lazily_test::json_member(step, "op");
    const std::string type = lazily_test::json_string(lazily_test::json_member(op, "type"));
    const std::string where = fixture_id + " step " + std::to_string(index) + " (" + type + ")";
    lazily_test::AssertionKeys expected(where + ".expected",
                                        lazily_test::json_member(step, "expected"));

    if (type == "digest_equal") {
      const ReplayValue left = value_from_fixture(lazily_test::json_member(
          values, lazily_test::json_string(lazily_test::json_member(op, "left"))));
      const ReplayValue right = value_from_fixture(lazily_test::json_member(
          values, lazily_test::json_string(lazily_test::json_member(op, "right"))));
      const bool equal = canonical_digest(left) == canonical_digest(right);
      REQUIRE(lazily_test::json_bool(lazily_test::json_member(step, "returns")) == equal,
              where + ": returns");
      outcomes.insert(equal);
      continue;
    }

    if (type == "digest_defined") {
      const ReplayValue value = value_from_fixture(lazily_test::json_member(
          values, lazily_test::json_string(lazily_test::json_member(op, "value"))));
      bool defined = true;
      try {
        canonical_digest(value);
      } catch (const ReplayEncodingError&) {
        defined = false;
      }
      REQUIRE(lazily_test::json_bool(lazily_test::json_member(step, "returns")) == defined,
              where + ": returns");
      expected.assert_key("outcome", std::string("encoding_error"));
      continue;
    }

    REQUIRE(false, where + ": unknown canonical encoding operation");
  }

  // Both outcomes really occurred. A runner that only ever saw `false` would
  // pass every inequality claim with a thoroughly broken encoding.
  REQUIRE(outcomes.size() == 2, fixture_id + ": the replay never observed both digest outcomes");
}

// -- obligation 3, the half the corpus cannot carry (`#lzreplayframing`) ------
//
// `canonical_encoding_equality.json` carries three member-framing rows, and
// only ONE of them is layout-independent. `[["a"],"b"]` vs `[["a","b"]]`
// collides under any unframed concatenation, because a nested container
// boundary has no tag to hide behind. The other two — `["a","sbc"]` vs
// `["as","bc"]` and the mapping analogue — collide only in a layout whose
// string tag is the byte `s`, and the corpus says so: a binding MAY choose its
// tags, so the pair that hides this binding's frame has to be constructed from
// this binding's bytes and asserted here.
//
// This binding's layout is `tag + decimal(len(body)) + ':' + body`
// (replay_detail::frame). Its string tag IS `s`, so the corpus's rows do fire
// here — but only against one of the two ways the length prefix can go. The
// prefix is TWO things, a decimal count and the `:` that terminates it, and
// removing them separately needs different colliding content:
//
//   * remove count AND terminator (`frame` becomes `tag + body`): the corpus's
//     `["a","sbc"]` / `["as","bc"]` pair collides, on `sassbc`.
//   * remove the COUNT only (`frame` becomes `tag + ':' + body`): the corpus's
//     pair does NOT collide — `s:as:sbc` vs `s:ass:bc` — because the surviving
//     `:` still separates the members. Hiding a frame that keeps its terminator
//     needs content that spells `s` AND content that ends in `:`, which is the
//     pair below: it collides on `s:as:s:bc`.
//
// So the pair asserted here is the one the corpus cannot ask for in this
// layout. The corpus still covers the whole-prefix removal — on step 8, and on
// the nested row, which reddens whenever a CONTAINER loses its count. What no
// corpus row reaches is the count going missing from LEAF members only, with
// the tag and terminator left in place; that is the defect this pair is for.
//
// The exact bytes are pinned rather than only the inequality. The inequality
// alone would keep passing after a layout change that made the pair
// uninteresting — the pair is chosen FOR these bytes, and a reader has to be
// able to see why. The inequality is asserted FIRST in each group so a
// length-dropping mutation reddens on the claim rather than on the pin.
//
// Three cold mutations of replay_detail::frame, each run from `make clean`:
//
//   frame -> tag + body                     corpus step 8 (["a","sbc"] vs
//                                           ["as","bc"]) reddens. Step 7, the
//                                           old member-framing row, still
//                                           PASSES — which is `#lzreplayframing`
//                                           reproduced in this binding.
//   frame -> tag + ':' + body               corpus step 10, the nested row,
//                                           reddens; steps 7-9 all pass.
//   Str case -> 's' + ':' + text            the WHOLE 14-step fixture passes
//   (containers still framed)               and only the pair below reddens.
//
// The third is why this block exists. It is a real defect — leaf members stop
// being length-framed — and no row the corpus can carry catches it here.
static void assert_local_member_framing() {
  const ReplayValue a_then_colon_bc =
      ReplayValue::seq({ReplayValue::text("a"), ReplayValue::text("s:bc")});
  const ReplayValue as_colon_then_bc =
      ReplayValue::seq({ReplayValue::text("as:"), ReplayValue::text("bc")});

  // Both sides carry the same member tags and the same content bytes in the
  // same order; only the counts say where one member stops. Drop the counts
  // from the string frame and both read `l9:s:as:s:bc`.
  REQUIRE(canonical_digest(a_then_colon_bc) != canonical_digest(as_colon_then_bc),
          "[\"a\",\"s:bc\"] and [\"as:\",\"bc\"] must differ: without the member "
          "length count they concatenate to the same bytes in THIS layout");
  REQUIRE(canonical_digest(a_then_colon_bc) == "l11:s1:as4:s:bc",
          "the canonical layout is tag + decimal(len) + ':' + body");
  REQUIRE(canonical_digest(as_colon_then_bc) == "l11:s3:as:s2:bc",
          "the canonical layout is tag + decimal(len) + ':' + body");

  // The mapping analogue, for the same reason obligation 3 asks for one: a
  // key is framed apart from its value, not merely juxtaposed with it.
  const ReplayValue map_a_colon_b =
      ReplayValue::map({{ReplayValue::text("a"), ReplayValue::text("s:b")}});
  const ReplayValue map_as_colon_b =
      ReplayValue::map({{ReplayValue::text("as:"), ReplayValue::text("b")}});
  REQUIRE(canonical_digest(map_a_colon_b) != canonical_digest(map_as_colon_b),
          "{\"a\":\"s:b\"} and {\"as:\":\"b\"} must differ: without the member "
          "length count they concatenate to the same bytes in THIS layout");
  REQUIRE(canonical_digest(map_a_colon_b) == "m10:s1:as3:s:b",
          "the canonical layout is tag + decimal(len) + ':' + body");
  REQUIRE(canonical_digest(map_as_colon_b) == "m10:s3:as:s1:b",
          "the canonical layout is tag + decimal(len) + ':' + body");

  // The nested boundary, pinned in bytes for the same reason. The corpus row
  // asserts the inequality; these two digests show WHERE it lives — the pair is
  // identical but for the inner container's own count, so an encoder that
  // framed members and forgot the container would fold them together.
  const ReplayValue nested_a_then_b =
      ReplayValue::seq({ReplayValue::seq({ReplayValue::text("a")}), ReplayValue::text("b")});
  const ReplayValue nested_ab =
      ReplayValue::seq({ReplayValue::seq({ReplayValue::text("a"), ReplayValue::text("b")})});
  REQUIRE(canonical_digest(nested_a_then_b) != canonical_digest(nested_ab),
          "[[\"a\"],\"b\"] and [[\"a\",\"b\"]] differ only in the inner "
          "container's length count");
  REQUIRE(canonical_digest(nested_a_then_b) == "l11:l4:s1:as1:b",
          "a nested container carries its own frame");
  REQUIRE(canonical_digest(nested_ab) == "l11:l8:s1:as1:b",
          "a nested container carries its own frame");
}

int main() {
  drive_harness_fixture("fingerprint_log_binding.json", 8);
  drive_harness_fixture("divergence_localization.json", 7);
  drive_encoding_fixture("canonical_encoding_equality.json", 14);
  assert_local_member_framing();

  REQUIRE_FIXTURES_LOADED(3);
  return 0;
}
