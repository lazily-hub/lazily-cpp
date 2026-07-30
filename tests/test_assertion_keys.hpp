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
// Usage:
//   lazily_test::AssertionKeys keys("presence.json#3 expected", expected);
//   keys.assert_key("present", cell.present(ctx), json_presence_map);
//   keys.assert_key("closed", actually_closed);          // bool, parsed by type
//   keys.assert_key_with("invalidates", [&](const Json& v) {
//     return json_bool(json_member(v, "present")) == !was;
//   });
//   keys.excuse_key("mode", "selects the strategy driven above, not a value");
//   keys.finish();

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
  static const std::set<std::string> keys = {"note", "notes", "comment",
                                             "description", "why"};
  return keys;
}

// Compact rendering of a fixture value, so a failure names what the corpus
// actually said rather than only which key disagreed.
inline std::string json_debug(const Json& value) {
  switch (value.type) {
    case Json::Type::Null: return "null";
    case Json::Type::Bool: return value.boolean ? "true" : "false";
    case Json::Type::Number: return value.number_token;
    case Json::Type::String: return "\"" + value.str + "\"";
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
inline std::string parse_fixture_scalar(const Json& v, std::string*) {
  return json_string(v);
}
inline double parse_fixture_scalar(const Json& v, double*) {
  return json_number(v);
}
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
inline unsigned long long parse_fixture_scalar(const Json& v,
                                               unsigned long long*) {
  return static_cast<unsigned long long>(json_u64(v));
}

class AssertionKeys {
 public:
  AssertionKeys(std::string where, const Json& object)
      : where_(std::move(where)), object_(&object) {
    REQUIRE(object.is_object(), "assertion block must be a JSON object");
    consumed_ = assertion_narrative_keys();
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
    for (const auto& kv : object_->object) out.push_back(kv.first);
    return out;
  }

  // Consume every key starting with `prefix` and return the matches. For the
  // corpus's parameterised spellings (`resync_after_epoch_10`), where the
  // suffix is data rather than a distinct assertion. Each match must still be
  // asserted or excused by name.
  std::vector<std::pair<std::string, const Json*>> with_prefix(
      const std::string& prefix) {
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
  template <typename T>
  void assert_key(const std::string& key, const T& actual) {
    const Json& want = required(key);
    asserted_.insert(key);
    if (!(actual == parse_fixture_scalar(want, static_cast<T*>(nullptr))))
      fail_mismatch(key, want);
  }

  // As `assert_key`, but a no-op when the fixture omits the key. Returns
  // whether the key was present, and therefore asserted.
  template <typename T, typename Parse>
  bool assert_key_if_present(const std::string& key, const T& actual,
                             Parse parse) {
    const Json* want = optional(key);
    if (want == nullptr) return false;
    asserted_.insert(key);
    if (!(actual == parse(*want))) fail_mismatch(key, *want);
    return true;
  }

  template <typename T>
  bool assert_key_if_present(const std::string& key, const T& actual) {
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
  template <typename Check>
  void assert_key_with(const std::string& key, Check check) {
    const Json& want = required(key);
    asserted_.insert(key);
    if (!check(want)) fail_mismatch(key, want);
  }

  // As `assert_key_with`, but a no-op when the fixture omits the key.
  template <typename Check>
  bool assert_key_with_if_present(const std::string& key, Check check) {
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
    REQUIRE(!reason.empty(),
            "excuse_key requires a non-empty reason -- an excuse without one "
            "is the allowlist this class exists to replace");
    consumed_.insert(key);
    excused_.emplace(key, reason);
  }

  void excuse_keys(std::initializer_list<const char*> keys,
                   const std::string& reason) {
    for (const char* key : keys) excuse_key(key, reason);
  }

  // -- verification -------------------------------------------------------

  // Abort when the fixture carried an assertion key this runner never asked
  // for, read but never compared, or excused while the excuse has gone stale.
  // Names the key and the fixture, because "some assertion went unread" is not
  // actionable.
  void finish() {
    if (finished_) return;
    finished_ = true;
    for (const auto& kv : object_->object) {
      const std::string& key = kv.first;
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
        std::cout << "FAIL: " << where_ << ": assertion key '" << key
                  << "' is excused (\"" << excused_.at(key)
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
        std::cout << "FAIL: " << where_ << ": assertion key '" << kv.first
                  << "' is excused (\"" << kv.second
                  << "\") but the fixture does not carry it. The excuse is "
                     "stale -- delete it (#lzconsumednotasserted)"
                  << std::endl;
        std::abort();
      }
    }
  }

 private:
  [[noreturn]] void fail_mismatch(const std::string& key,
                                  const Json& want) const {
    std::cout << "FAIL: " << where_ << ": assertion key '" << key
              << "' disagreed with the fixture value " << json_debug(want)
              << std::endl;
    std::abort();
  }

  std::string where_;
  const Json* object_;
  std::set<std::string> consumed_;
  std::set<std::string> asserted_;
  std::map<std::string, std::string> excused_;
  bool finished_ = false;
};

// Adapter so an existing `json_member(expected, "key")` call site records
// consumption unchanged. This is a RUNG 2 read: the value still has to reach an
// `assert_key*` call, or `finish()` refuses it as read-but-not-asserted.
inline const Json& json_member(AssertionKeys& keys, const std::string& key) {
  return keys.required(key);
}

}  // namespace lazily_test

#endif  // LAZILY_TESTS_TEST_ASSERTION_KEYS_HPP
