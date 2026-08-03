// Consumption AND assertion tracking for a conformance fixture's assertion
// block (`assertions` / `expected` / `expect`) -- `#lzassertunknownkeys`
// (rung 2) extended by `#lzconsumednotasserted` (rung 3).
//
// The failure this exists to prevent sits one level below "was the fixture
// replayed". A runner that reads named keys out of an assertion object and lets
// anything it does not recognise fall through reports the fixture as replayed
// while never checking the field the fixture exists for: the wire round-trips,
// the suite goes green, and the assertion proves nothing.
//
// Three rungs:
//
//   1. `REQUIRE_FIXTURES_LOADED` proves the fixture bytes were OPENED.
//   2. the consumed set proves every key in the block was READ.
//   3. the asserted set proves every read key reached a COMPARISON AGAINST THE
//      FIXTURE'S OWN VALUE.
//
// Rung 2 alone is satisfied by a read-then-discard: a loop that reads
// `block[key]` and then `continue`s past it; a value bound and never compared;
// an arm that reads the key but compares against a hardcoded literal, so
// editing the fixture changes nothing. All three mark the key consumed and
// prove nothing.
//
// A key becomes ASSERTED only by going through `assert_key` / `assert_key_with`
// (or their `_if_present` forms), which hand the caller the fixture's own value
// and perform -- or run -- the comparison. There is no path that marks a key
// asserted without the fixture's value reaching a check.
//
// Where a key genuinely cannot be checked at this call site -- it selects a
// code path rather than naming a value, or the fact is proven by a different
// runner -- `excuse_key(key, reason)` says so out loud with a non-empty reason.
// The excuse is enforced in BOTH directions, exactly as the coverage allowlist
// is: excusing a key that the same block also asserts, or excusing a key the
// block does not carry, is a failure, because the excuse has gone stale and now
// hides nothing.
//
// Every accessor marks its key consumed whether or not the key is present, so
// an *optional* assertion is still a declaration that this runner knows about
// the key. `finish()` then aborts, naming the fixture and the offending key.
//
// A fourth kind of key needs neither path: a PROSE key, whose value is an
// English paragraph stating an obligation and carrying nothing a runner can
// compare against observed behaviour (`#lzprosekeyconvention`). The corpus --
// never the binding -- declares which keys those are, in `assertions.prose`.
// Such a key is DISCHARGED: `prose_key(key, {...})` names the executable
// assertion keys that carry its obligation, and the ledger below checks that
// the run really asserted them. Asserting a paragraph (or a tally derived from
// one) pins wording rather than behaviour -- a copy-edit reddens the run and a
// library regression does not -- and excusing it with free text is the
// unfalsifiable default the convention exists to remove. Both now fail.
//
// Usage:
//   lazily_test::AssertionKeys keys("presence.json#3 expected", expected);
//   keys.assert_key("present", cell.present(ctx), json_presence_map);
//   keys.assert_key("closed", actually_closed);          // bool, parsed by type
//   keys.assert_key_with("invalidates", [&](const Json& v) {
//     return json_bool(json_member(v, "present")) == !was;
//   });
//   keys.excuse_key("mode", "selects the strategy driven above, not a value");
//   keys.prose_key("clause", {"backends", "scenario_count"});
//   keys.finish();
//   lazily_test::verify_prose("codec/blob_backend_discriminator.json");

#ifndef LAZILY_TESTS_TEST_ASSERTION_KEYS_HPP
#define LAZILY_TESTS_TEST_ASSERTION_KEYS_HPP

#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "test_json.hpp"
#include "test_require.hpp"

namespace lazily_test {

// Prose keys that carry no assertion and are documentation only.
inline const std::set<std::string>& assertion_narrative_keys() {
  static const std::set<std::string> keys = {"note", "notes", "comment", "description", "why"};
  return keys;
}

// -- the prose ledger (`#lzprosekeyconvention`) ---------------------------
//
// Fixture-scoped, not block-scoped, because an obligation stated in
// `assertions` is routinely carried by a per-scenario `expect` key:
// `epoch_disambiguation` is discharged by `expect.frame_epoch` and
// `expect.blob_epoch`, asserted long after the `assertions` block itself went
// out of scope. A named key is therefore matched BY NAME in any block of that
// fixture, and the check happens when the replay is finished.

// One claim: "`epoch_disambiguation` is carried by `frame_epoch`, `blob_epoch`".
// Unlike "`epoch_disambiguation` is prose", that is a statement about the run,
// so the ledger can falsify it.
struct ProseDischarge {
  std::string block;              // the `where_` of the block that claimed it
  std::string key;                // the prose key being discharged
  std::vector<std::string> names; // the executable keys said to carry it
};

struct ProseLedger {
  std::map<std::string, std::set<std::string>> asserted;     // fixture -> keys asserted anywhere
  std::map<std::string, std::set<std::string>> declared;     // fixture -> assertions.prose
  std::map<std::string, std::vector<ProseDischarge>> claims; // fixture -> pending discharges
  std::set<std::string> verified;

  // The teardown that makes a run which NEVER verifies fail. A discharge
  // registered and never checked has proven exactly as much as an unconsumed
  // key, so it is reported the same way -- a message naming the fixture,
  // followed by `abort()`. Never a throw: this destructor runs at exit, and in
  // the general destructor case a throw during unwinding terminates the process
  // without the diagnostic that makes the failure actionable.
  ~ProseLedger() {
    for (const auto& kv : claims) {
      if (kv.second.empty() || verified.count(kv.first) != 0) continue;
      std::cout << "FAIL: " << kv.first << ": " << kv.second.size()
                << " prose discharge(s) were registered (first: '" << kv.second.front().key
                << "') but verify_prose(\"" << kv.first
                << "\") was never called, so the claim that they are carried by "
                   "executable keys was never checked against the run. An "
                   "unverified discharge is as bad as an unconsumed key "
                   "(#lzprosekeyconvention)"
                << std::endl;
      std::abort();
    }
  }
};

inline ProseLedger& prose_ledger() {
  static ProseLedger ledger;
  return ledger;
}

// Every conformance runner names its blocks `<fixture id> <block path>` --
// `codec/blob_backend_discriminator.json scenarios[3].expect` -- so the leading
// token says which fixture's ledger this block contributes to. One rule, one
// place, rather than a fixture argument threaded through every existing call
// site.
inline std::string fixture_of(const std::string& where) {
  const std::size_t space = where.find(' ');
  return space == std::string::npos ? where : where.substr(0, space);
}

// Compact rendering of a fixture value, so a failure names what the corpus
// actually said rather than only which key disagreed.
inline std::string json_debug(const Json& value) {
  switch (value.type) {
  case Json::Type::Null:
    return "null";
  case Json::Type::Bool:
    return value.boolean ? "true" : "false";
  case Json::Type::Number:
    return value.number_token;
  case Json::Type::String:
    return "\"" + value.str + "\"";
  case Json::Type::Array: {
    std::string out = "[";
    for (std::size_t i = 0; i < value.array.size(); ++i) {
      if (i != 0) out += ",";
      out += json_debug(*value.array[i]);
    }
    return out + "]";
  }
  default: {
    std::string out = "{";
    bool first = true;
    for (const auto& kv : value.object) {
      if (!first) out += ",";
      first = false;
      out += "\"" + kv.first + "\":" + json_debug(*kv.second);
    }
    return out + "}";
  }
  }
}

// Parse a fixture scalar into the type of the value being compared. Tag
// dispatch rather than a trait, so the mapping is one visible overload per
// spelling and an unsupported type is a compile error naming the call site
// instead of a silent narrowing.
inline bool parse_fixture_scalar(const Json& v, bool*) { return json_bool(v); }
inline std::string parse_fixture_scalar(const Json& v, std::string*) { return json_string(v); }
inline double parse_fixture_scalar(const Json& v, double*) { return json_number(v); }
inline int parse_fixture_scalar(const Json& v, int*) {
  REQUIRE(v.type == Json::Type::Number, "expected JSON number");
  return static_cast<int>(v.as_int());
}
inline long parse_fixture_scalar(const Json& v, long*) {
  REQUIRE(v.type == Json::Type::Number, "expected JSON number");
  return static_cast<long>(v.as_int());
}
inline long long parse_fixture_scalar(const Json& v, long long*) {
  REQUIRE(v.type == Json::Type::Number, "expected JSON number");
  return v.as_int();
}
inline unsigned parse_fixture_scalar(const Json& v, unsigned*) {
  return static_cast<unsigned>(json_u64(v));
}
inline unsigned long parse_fixture_scalar(const Json& v, unsigned long*) {
  return static_cast<unsigned long>(json_u64(v));
}
inline unsigned long long parse_fixture_scalar(const Json& v, unsigned long long*) {
  return static_cast<unsigned long long>(json_u64(v));
}

class AssertionKeys {
public:
  AssertionKeys(std::string where, const Json& object)
      : where_(std::move(where)), object_(&object) {
    REQUIRE(object.is_object(), "assertion block must be a JSON object");
    consumed_ = assertion_narrative_keys();
    fixture_ = fixture_of(where_);
    // The corpus decides which keys are paragraphs; this binding does not. Read
    // WITHOUT consuming -- `prose` is consumed by the discharge-set comparison
    // in `verify_prose`, which is what makes a forgotten key fail rather than
    // vanish.
    if (const Json* prose = object_->find("prose")) {
      for (const auto& name : json_array(*prose))
        prose_declared_.insert(json_string(*name));
      prose_ledger().declared[fixture_] = prose_declared_;
    }
  }

  // Non-copyable: the consumed set is the point, and a copy would split it.
  AssertionKeys(const AssertionKeys&) = delete;
  AssertionKeys& operator=(const AssertionKeys&) = delete;

  // Scope-bound, so a runner cannot forget the check on an early `continue`.
  ~AssertionKeys() { finish(); }

  // -- rung 2: reads ------------------------------------------------------
  //
  // These mark the key CONSUMED only. A key read through them and never routed
  // into an `assert_key*` call fails `finish()` as read-but-not-asserted, so
  // they are for driving the replay (op payloads, discriminators) and for
  // reaching nested structure -- never for checking a value by hand.

  // Consume `key` and return it; aborts when the fixture does not carry it.
  const Json& required(const std::string& key) {
    consumed_.insert(key);
    const Json* value = object_->find(key);
    if (value == nullptr) {
      std::cout << "FAIL: " << where_ << ": required assertion key '" << key
                << "' is missing from the fixture" << std::endl;
      std::abort();
    }
    return *value;
  }

  // Drop-in for `Json::find`, so an existing optional-lookup call site records
  // consumption unchanged.
  const Json* find(const std::string& key) { return optional(key); }

  // Consume `key`; null when the fixture does not carry it.
  const Json* optional(const std::string& key) {
    consumed_.insert(key);
    return object_->find(key);
  }

  // Consume `key` and report whether the fixture carries it.
  bool has(const std::string& key) {
    consumed_.insert(key);
    return object_->find(key) != nullptr;
  }

  // Names of the keys the fixture carries, for blocks whose KEYS are data (a
  // per-node id, a replica name). Enumeration only: the values stay reachable
  // exclusively through the accessors above, so listing a key still does not
  // satisfy it.
  std::vector<std::string> keys() const {
    std::vector<std::string> out;
    for (const auto& kv : object_->object)
      out.push_back(kv.first);
    return out;
  }

  // Consume every key starting with `prefix` and return the matches. For the
  // corpus's parameterised spellings (`resync_after_epoch_10`), where the
  // suffix is data rather than a distinct assertion. Each match must still be
  // asserted or excused by name.
  std::vector<std::pair<std::string, const Json*>> with_prefix(const std::string& prefix) {
    std::vector<std::pair<std::string, const Json*>> out;
    for (const auto& kv : object_->object) {
      if (kv.first.rfind(prefix, 0) == 0) {
        consumed_.insert(kv.first);
        out.emplace_back(kv.first, kv.second.get());
      }
    }
    return out;
  }

  // -- rung 3: assertions -------------------------------------------------

  // Compare `actual` against the fixture's value for `key`, parsed by `parse`.
  // Marks the key asserted. Aborts when the fixture omits the key.
  template <typename T, typename Parse>
  void assert_key(const std::string& key, const T& actual, Parse parse) {
    const Json& want = required(key);
    asserted_.insert(key);
    if (!(actual == parse(want))) fail_mismatch(key, want);
  }

  // Scalar form: the fixture value is parsed as `actual`'s own type.
  template <typename T> void assert_key(const std::string& key, const T& actual) {
    const Json& want = required(key);
    asserted_.insert(key);
    if (!(actual == parse_fixture_scalar(want, static_cast<T*>(nullptr)))) fail_mismatch(key, want);
  }

  // As `assert_key`, but a no-op when the fixture omits the key. Returns
  // whether the key was present, and therefore asserted.
  template <typename T, typename Parse>
  bool assert_key_if_present(const std::string& key, const T& actual, Parse parse) {
    const Json* want = optional(key);
    if (want == nullptr) return false;
    asserted_.insert(key);
    if (!(actual == parse(*want))) fail_mismatch(key, *want);
    return true;
  }

  template <typename T> bool assert_key_if_present(const std::string& key, const T& actual) {
    const Json* want = optional(key);
    if (want == nullptr) return false;
    asserted_.insert(key);
    if (!(actual == parse_fixture_scalar(*want, static_cast<T*>(nullptr))))
      fail_mismatch(key, *want);
    return true;
  }

  // For comparisons that are not equality -- a tolerance, set containment, a
  // walk into a nested sub-object. `check` receives the fixture's own value and
  // returns whether the library agreed. Marks the key asserted.
  template <typename Check> void assert_key_with(const std::string& key, Check check) {
    const Json& want = required(key);
    asserted_.insert(key);
    if (!check(want)) fail_mismatch(key, want);
  }

  // As `assert_key_with`, but a no-op when the fixture omits the key.
  template <typename Check> bool assert_key_with_if_present(const std::string& key, Check check) {
    const Json* want = optional(key);
    if (want == nullptr) return false;
    asserted_.insert(key);
    if (!check(*want)) fail_mismatch(key, *want);
    return true;
  }

  // -- declared exceptions ------------------------------------------------

  // Declare that `key` carries nothing this call site can compare against, and
  // say why. Enforced in both directions by `finish()`: an excuse for a key the
  // same block also asserts, or for a key the block does not carry, is stale
  // and fails.
  void excuse_key(const std::string& key, const std::string& reason) {
    REQUIRE(!reason.empty(), "excuse_key requires a non-empty reason -- an excuse without one "
                             "is the allowlist this class exists to replace");
    consumed_.insert(key);
    excused_.emplace(key, reason);
  }

  void excuse_keys(std::initializer_list<const char*> keys, const std::string& reason) {
    for (const char* key : keys)
      excuse_key(key, reason);
  }

  // -- prose keys (`#lzprosekeyconvention`) -------------------------------

  // Discharge `key` -- a paragraph the corpus declared in `assertions.prose` --
  // by naming the executable assertion keys that carry its obligation. The
  // naming is a claim about THIS FIXTURE'S RUN, verified by `verify_prose`:
  // every named key must have reached an `assert_key*` call in some block of
  // the same fixture.
  //
  // Checked here, where the call site is still on the stack:
  //   rule 3 -- a key the corpus did not declare prose cannot be discharged;
  //   rule 5 -- a discharge naming nothing discharges nothing;
  //   rule 7 -- a paragraph cannot be discharged by another paragraph.
  void prose_key(const std::string& key, std::initializer_list<const char*> discharged_by) {
    if (object_->find("prose") == nullptr) {
      std::cout << "FAIL: " << where_ << ": prose_key('" << key
                << "') but this block carries no `prose` array. Which keys are "
                   "paragraphs is declared BY THE CORPUS -- a binding that "
                   "decides for itself is the drift this convention removes "
                   "(#lzprosekeyconvention)"
                << std::endl;
      std::abort();
    }
    if (prose_declared_.count(key) == 0) {
      std::cout << "FAIL: " << where_ << ": assertion key '" << key
                << "' is discharged as prose but the fixture does not list it in "
                   "`assertions.prose`. A key carrying a comparable value must be "
                   "asserted, not discharged (#lzprosekeyconvention rule 3)"
                << std::endl;
      std::abort();
    }
    if (discharged_by.size() == 0) {
      std::cout << "FAIL: " << where_ << ": prose key '" << key
                << "' is discharged by NO executable key. A discharge naming "
                   "nothing is the free-text excuse under another name "
                   "(#lzprosekeyconvention rule 5)"
                << std::endl;
      std::abort();
    }
    ProseDischarge claim;
    claim.block = where_;
    claim.key = key;
    for (const char* name : discharged_by) {
      if (prose_declared_.count(name) != 0) {
        std::cout << "FAIL: " << where_ << ": prose key '" << key << "' is discharged by '" << name
                  << "', which is itself declared prose. A paragraph cannot carry "
                     "another paragraph's obligation (#lzprosekeyconvention rule 7)"
                  << std::endl;
        std::abort();
      }
      claim.names.emplace_back(name);
    }
    consumed_.insert(key);
    discharged_.insert(key);
    prose_discharged_any_ = true;
    prose_ledger().claims[fixture_].push_back(std::move(claim));
  }

  // -- verification -------------------------------------------------------

  // Abort when the fixture carried an assertion key this runner never asked
  // for, read but never compared, or excused while the excuse has gone stale.
  // Names the key and the fixture, because "some assertion went unread" is not
  // actionable.
  void finish() {
    if (finished_) return;
    finished_ = true;
    // Publish this block's assertions to the fixture ledger BEFORE any verdict,
    // so a discharge naming a key asserted here is checkable from a block that
    // has already gone out of scope.
    if (!asserted_.empty()) {
      auto& fixture_asserted = prose_ledger().asserted[fixture_];
      fixture_asserted.insert(asserted_.begin(), asserted_.end());
    }
    for (const auto& kv : object_->object) {
      const std::string& key = kv.first;
      if (key == "prose" && !prose_declared_.empty()) {
        // `prose` is consumed and asserted by the discharged-set comparison in
        // `verify_prose`, which the ledger's teardown makes mandatory. A block
        // that declares paragraphs and discharges none never gets there.
        if (!prose_discharged_any_) {
          std::cout << "FAIL: " << where_
                    << ": the fixture declares `assertions.prose` but this runner "
                       "discharged no prose key. Replaying a fixture without "
                       "discharging its paragraphs reports green while the "
                       "obligations they state are checked by nothing -- name the "
                       "executable keys via prose_key (#lzprosekeyconvention)"
                    << std::endl;
          std::abort();
        }
        continue;
      }
      // A declared paragraph outranks the by-name annotation exemption: an
      // `assertions.note` the corpus declared prose is an obligation, and a
      // reserved name is exactly the place no runner can be made to discharge
      // one.
      if (prose_declared_.count(key) != 0) {
        if (asserted_.count(key) != 0) {
          std::cout << "FAIL: " << where_ << ": prose key '" << key
                    << "' was ASSERTED. Comparing an English paragraph -- or a "
                       "tally derived from one -- against observed behaviour pins "
                       "wording, not behaviour: a copy-edit reddens the run and a "
                       "library regression does not. Discharge it via prose_key "
                       "(#lzprosekeyconvention rule 1)"
                    << std::endl;
          std::abort();
        }
        if (excused_.count(key) != 0) {
          std::cout << "FAIL: " << where_ << ": prose key '" << key << "' was excused (\""
                    << excused_.at(key)
                    << "\"). A free-text reason is unfalsifiable and is "
                       "indistinguishable from the undocumented default this "
                       "convention removes -- discharge it by naming the "
                       "executable keys instead (#lzprosekeyconvention rule 2)"
                    << std::endl;
          std::abort();
        }
        if (discharged_.count(key) == 0) {
          std::cout << "FAIL: " << where_ << ": assertion key '" << key
                    << "' is declared in `assertions.prose` but this runner "
                       "discharged nothing for it. A forgotten paragraph must fail "
                       "rather than vanish (#lzprosekeyconvention rule 4)"
                    << std::endl;
          std::abort();
        }
        continue;
      }
      if (assertion_narrative_keys().count(key) != 0) continue;
      if (consumed_.count(key) == 0) {
        std::cout << "FAIL: " << where_ << ": assertion key '" << key
                  << "' is present in the fixture but was never consumed by "
                     "this runner. Replaying a fixture without evaluating its "
                     "assertion reports green while proving nothing -- "
                     "implement the assertion rather than ignoring the key "
                     "(#lzassertunknownkeys)"
                  << std::endl;
        std::abort();
      }
      const bool is_excused = excused_.count(key) != 0;
      const bool is_asserted = asserted_.count(key) != 0;
      if (is_excused && is_asserted) {
        std::cout << "FAIL: " << where_ << ": assertion key '" << key << "' is excused (\""
                  << excused_.at(key)
                  << "\") and also asserted in the same replay. The excuse is "
                     "stale and now hides nothing -- delete it "
                     "(#lzconsumednotasserted)"
                  << std::endl;
        std::abort();
      }
      if (is_excused) continue;
      if (!is_asserted) {
        std::cout << "FAIL: " << where_ << ": assertion key '" << key
                  << "' was read but never compared against the fixture's own "
                     "value ("
                  << json_debug(*kv.second)
                  << "). A key that is read and discarded proves nothing the "
                     "fixture could disprove -- assert it via assert_key / "
                     "assert_key_with, or excuse_key it with a reason "
                     "(#lzconsumednotasserted)"
                  << std::endl;
        std::abort();
      }
    }
    // The other direction: an excuse for a key the block does not carry has
    // outlived the shape it was written for.
    for (const auto& kv : excused_) {
      if (object_->find(kv.first) == nullptr) {
        std::cout << "FAIL: " << where_ << ": assertion key '" << kv.first << "' is excused (\""
                  << kv.second
                  << "\") but the fixture does not carry it. The excuse is "
                     "stale -- delete it (#lzconsumednotasserted)"
                  << std::endl;
        std::abort();
      }
    }
  }

private:
  [[noreturn]] void fail_mismatch(const std::string& key, const Json& want) const {
    std::cout << "FAIL: " << where_ << ": assertion key '" << key
              << "' disagreed with the fixture value " << json_debug(want) << std::endl;
    std::abort();
  }

  std::string where_;
  std::string fixture_;
  const Json* object_;
  std::set<std::string> consumed_;
  std::set<std::string> asserted_;
  std::map<std::string, std::string> excused_;
  std::set<std::string> prose_declared_;
  std::set<std::string> discharged_;
  bool prose_discharged_any_ = false;
  bool finished_ = false;
};

// Fixture-end verification of the prose ledger (`#lzprosekeyconvention`). Call
// it once the fixture's replay is finished and every block has gone out of
// scope; the ledger's own teardown fails a run that never does.
//
// Checked here, because only a finished run knows what it asserted:
//   rule 1 -- a declared paragraph asserted by ANY block of this fixture;
//   rule 6 -- a discharge naming a key this fixture's run did not assert;
//   rule 4 -- the discharged set against `assertions.prose` itself. That
//             comparison is what consumes and asserts the `prose` key.
inline void verify_prose(const std::string& fixture) {
  auto& ledger = prose_ledger();
  const auto declared_it = ledger.declared.find(fixture);
  if (declared_it == ledger.declared.end()) {
    std::cout << "FAIL: " << fixture
              << ": verify_prose was called but no block of this fixture read an "
                 "`assertions.prose` array. Either the assertion block was never "
                 "built, or the fixture id does not match the one its blocks were "
                 "named with (#lzprosekeyconvention)"
              << std::endl;
    std::abort();
  }
  const std::set<std::string>& declared = declared_it->second;
  const std::set<std::string>& asserted = ledger.asserted[fixture];

  for (const auto& key : declared) {
    if (asserted.count(key) != 0) {
      std::cout << "FAIL: " << fixture << ": prose key '" << key
                << "' reached an assert_key* call somewhere in this fixture's "
                   "replay. A paragraph is discharged, never asserted "
                   "(#lzprosekeyconvention rule 1)"
                << std::endl;
      std::abort();
    }
  }

  std::set<std::string> discharged;
  for (const auto& claim : ledger.claims[fixture]) {
    for (const auto& name : claim.names) {
      if (declared.count(name) != 0) {
        std::cout << "FAIL: " << claim.block << ": prose key '" << claim.key
                  << "' is discharged by '" << name
                  << "', which is itself declared prose (#lzprosekeyconvention rule 7)"
                  << std::endl;
        std::abort();
      }
      // THE rule. The excuse is falsifiable because the ledger can check it:
      // "`epoch_disambiguation` is discharged by `frame_epoch` and `blob_epoch`"
      // is a claim about the run; "`epoch_disambiguation` is prose" is not.
      if (asserted.count(name) == 0) {
        std::cout << "FAIL: " << claim.block << ": prose key '" << claim.key
                  << "' is discharged by '" << name
                  << "', which this fixture's run never asserted. The obligation "
                     "the paragraph states is carried by nothing "
                     "(#lzprosekeyconvention rule 6)"
                  << std::endl;
        std::abort();
      }
    }
    discharged.insert(claim.key);
  }

  if (discharged != declared) {
    std::string missing;
    for (const auto& key : declared)
      if (discharged.count(key) == 0) missing += (missing.empty() ? "" : ", ") + key;
    std::string extra;
    for (const auto& key : discharged)
      if (declared.count(key) == 0) extra += (extra.empty() ? "" : ", ") + key;
    std::cout << "FAIL: " << fixture << ": the discharged prose set differs from `assertions.prose`"
              << (missing.empty() ? "" : " -- declared but never discharged: " + missing)
              << (extra.empty() ? "" : " -- discharged but not declared: " + extra)
              << " (#lzprosekeyconvention rule 4)" << std::endl;
    std::abort();
  }
  ledger.verified.insert(fixture);
}

// Adapter so an existing `json_member(expected, "key")` call site records
// consumption unchanged. This is a RUNG 2 read: the value still has to reach an
// `assert_key*` call, or `finish()` refuses it as read-but-not-asserted.
inline const Json& json_member(AssertionKeys& keys, const std::string& key) {
  return keys.required(key);
}

} // namespace lazily_test

#endif // LAZILY_TESTS_TEST_ASSERTION_KEYS_HPP
