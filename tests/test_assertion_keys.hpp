// Consumption tracking for a conformance fixture's assertion block
// (`assertions` / `expected` / `expect`) -- `#lzassertunknownkeys`.
//
// The failure this exists to prevent sits one level below "was the fixture
// replayed". A runner that reads named keys out of an assertion object and lets
// anything it does not recognise fall through reports the fixture as replayed
// while never checking the field the fixture exists for: the wire round-trips,
// the suite goes green, and the assertion proves nothing.
//
// `REQUIRE_FIXTURES_LOADED` proves the bytes were opened. It cannot prove the
// keys inside them were consumed. This does.
//
// Every accessor marks its key consumed whether or not the key is present, so
// an *optional* assertion is still a declaration that this runner knows about
// the key. `finish()` then aborts, naming the fixture and the offending key,
// when the block carried one the runner never asked for.
//
// Usage:
//   lazily_test::AssertionKeys keys("presence.json#3 expected", expected);
//   step(json_presence_map(keys.required("present")), ...);
//   keys.finish();

#ifndef LAZILY_TESTS_TEST_ASSERTION_KEYS_HPP
#define LAZILY_TESTS_TEST_ASSERTION_KEYS_HPP

#include <initializer_list>
#include <iostream>
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

  // Declare keys consumed without reading them here. Only for keys a DIFFERENT
  // code path in the same replay evaluates -- never to silence a key nothing
  // evaluates, which is the allowlist this class exists to replace.
  void consume(std::initializer_list<const char*> keys) {
    for (const char* key : keys) consumed_.insert(key);
  }

  // Consume every key starting with `prefix` and return the matches. For the
  // corpus's parameterised spellings (`resync_after_epoch_10`), where the
  // suffix is data rather than a distinct assertion.
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

  // Abort when the fixture carried an assertion key this runner never asked
  // for. Names the key and the fixture, because "some assertion went unread" is
  // not actionable.
  void finish() {
    if (finished_) return;
    finished_ = true;
    for (const auto& kv : object_->object) {
      if (consumed_.count(kv.first) == 0) {
        std::cout << "FAIL: " << where_ << ": assertion key '" << kv.first
                  << "' is present in the fixture but was never consumed by "
                     "this runner. Replaying a fixture without evaluating its "
                     "assertion reports green while proving nothing -- implement "
                     "the assertion rather than ignoring the key "
                     "(#lzassertunknownkeys)"
                  << std::endl;
        std::abort();
      }
    }
  }

 private:
  std::string where_;
  const Json* object_;
  std::set<std::string> consumed_;
  bool finished_ = false;
};

// Adapter so an existing `json_member(expected, "key")` call site records
// consumption unchanged: swapping the `expected` binding for an `AssertionKeys`
// is then the whole conversion, and every read that already exists keeps
// working while the block gains its unconsumed-key guard.
inline const Json& json_member(AssertionKeys& keys, const std::string& key) {
  return keys.required(key);
}

}  // namespace lazily_test

#endif  // LAZILY_TESTS_TEST_ASSERTION_KEYS_HPP
