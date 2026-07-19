// Canonical conformance-fixture loader (#lzspecconf).
//
// Conformance fixtures are owned by the sibling `lazily-spec` repo and are read
// from `../lazily-spec/conformance/<area>/` — never from a copy inside this
// repo. lazily-cpp previously vendored them under `tests/conformance/`, which
// meant the suite validated against whatever this repo happened to hold rather
// than against the spec. A vendored copy cannot drift-detect: elsewhere in the
// family a vendored fixture silently shrank to a third of its source.
//
// There is deliberately NO fallback to a local copy. A fallback is precisely
// what makes drift invisible — the suite would go green against stale data and
// nobody would learn. Absence is a SKIP (CTest exit 77) with an explicit
// message, and CI asserts the directory exists so a skip cannot pass silently.
//
// The spec directory is baked in at configure time from
// `LAZILY_SPEC_CONFORMANCE_DIR` (see tests/CMakeLists.txt) so the path resolves
// identically no matter what working directory ctest runs the binary from.
// Overridable at run time via the LAZILY_SPEC_CONFORMANCE_DIR env var.
//
// An absence guard cannot catch a test that loads nothing at all, so each
// suite ends with REQUIRE_FIXTURES_LOADED(n): a positive assertion that the
// expected number of DISTINCT canonical fixtures were actually opened.

#ifndef LAZILY_TESTS_TEST_SPEC_FIXTURE_HPP
#define LAZILY_TESTS_TEST_SPEC_FIXTURE_HPP

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <string>

#include "test_require.hpp"

#ifndef LAZILY_SPEC_CONFORMANCE_DIR
#error "LAZILY_SPEC_CONFORMANCE_DIR is not defined — configure via tests/CMakeLists.txt"
#endif

namespace lazily_test {

// Distinct canonical fixture paths opened by this binary.
inline std::set<std::string>& loaded_fixtures() {
  static std::set<std::string> loaded;
  return loaded;
}

// Root of the canonical conformance corpus (sibling lazily-spec checkout).
inline std::filesystem::path spec_conformance_dir() {
  if (const char* override_dir = std::getenv("LAZILY_SPEC_CONFORMANCE_DIR")) {
    if (*override_dir != '\0') return std::filesystem::path(override_dir);
  }
  return std::filesystem::path(LAZILY_SPEC_CONFORMANCE_DIR);
}

// Exit 77 (CTest SKIP) when the sibling spec checkout is absent. Called before
// any fixture read so a missing sibling is an explicit skip, never a pass.
inline void require_spec_checkout_or_skip(const std::string& area) {
  const auto dir = spec_conformance_dir() / area;
  if (!std::filesystem::is_directory(dir)) {
    std::cout << "SKIP: canonical conformance fixtures not found at " << dir
              << " — clone the lazily-spec sibling "
                 "(git clone https://github.com/lazily-hub/lazily-spec.git "
                 "../lazily-spec) to run the "
              << area << " conformance suite" << std::endl;
    std::exit(77);
  }
}

// Read a canonical fixture's raw text, recording that it was actually opened.
inline std::string spec_fixture_text(const std::string& area,
                                     const std::string& name) {
  require_spec_checkout_or_skip(area);
  const auto path = spec_conformance_dir() / area / name;
  std::ifstream input(path);
  REQUIRE(input,
          "canonical conformance fixture missing from the lazily-spec sibling "
          "— a conformance test must not pass without its fixture");
  loaded_fixtures().insert(path.string());
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

}  // namespace lazily_test

// Positive assertion that fixtures actually ran. An absence guard proves the
// corpus is present; it cannot prove this binary read any of it. Assert the
// exact distinct-fixture count so deleting or short-circuiting a fixture read
// turns the suite red instead of quietly shrinking its coverage.
#define REQUIRE_FIXTURES_LOADED(expected)                                     \
  do {                                                                        \
    const std::size_t actual_ = lazily_test::loaded_fixtures().size();        \
    if (actual_ != static_cast<std::size_t>(expected)) {                      \
      std::cout << "FAIL: expected " << (expected)                            \
                << " distinct canonical fixtures to be read, but "            \
                << actual_                                                    \
                << " were — the suite is not exercising the spec corpus it "  \
                   "claims to"                                                \
                << std::endl;                                                 \
      return 1;                                                               \
    }                                                                         \
  } while (0)

#endif  // LAZILY_TESTS_TEST_SPEC_FIXTURE_HPP
