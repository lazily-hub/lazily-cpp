// Assertion that survives NDEBUG.
//
// `tests/CMakeLists.txt` strips NDEBUG for this directory, so plain `assert()`
// does fire under the project's Release default. REQUIRE exists for checks that
// must hold regardless of how the tests are configured — chiefly fixture
// presence, where a stripped assertion does not merely skip a check but lets a
// conformance test "pass" while reading nothing at all.
//
// See #lzspecedgeindex: before the NDEBUG fix, 1,083 of this suite's 1,093
// assertions were compiled out and the suite reported green. Anything whose
// failure would be indistinguishable from success belongs here, not in assert().

#ifndef LAZILY_TESTS_TEST_REQUIRE_HPP
#define LAZILY_TESTS_TEST_REQUIRE_HPP

#include <cstdlib>
#include <iostream>

#define REQUIRE(cond, msg)                                              \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::cout << "FAIL: " << (msg) << " @" << __FILE__ << ":"         \
                << __LINE__ << std::endl;                               \
      std::abort();                                                     \
    }                                                                   \
  } while (0)

#endif  // LAZILY_TESTS_TEST_REQUIRE_HPP
