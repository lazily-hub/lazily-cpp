// Wires the sibling lazily-formal Lean 4 model into the C++ test suite.
//
// The C++ state-chart / reactive / collection / CRDT code mirrors universal
// theorems in the sibling lazily-formal submodule (LazilyFormal.StateChart /
// StateMachine / Reactive / Collection / Tree / Reconciliation /
// AsyncSlotState). Those theorems are only trustworthy if the model compiles,
// so this test runs `lake build` when the sibling checkout + Lean toolchain are
// present (full repo checkout / CI) and SKIPs gracefully otherwise (packaged
// consumer, shallow clone, no Lean toolchain) so the C++-only tests still run.
// CI uses a full checkout + elan, so the proofs are verified there.
//
// CTest treats exit code 77 as "skipped" (see SKIP_RETURN_CODE in
// tests/CMakeLists.txt).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// CTest SKIP_RETURN_CODE — keep in sync with tests/CMakeLists.txt.
constexpr int kSkip = 77;

// A real lazily-formal checkout ships these two markers.
bool is_formal_dir(const fs::path &dir) {
  std::error_code ec;
  return fs::is_regular_file(dir / "lakefile.lean", ec) &&
         fs::is_directory(dir / "LazilyFormal", ec);
}

// Honors LAZILY_FORMAL_PATH, then the in-repo submodule layout
// (src/lazily-cpp <-> src/lazily-formal) baked in at configure time via the
// LAZILY_FORMAL_DIR compile definition.
std::string resolve_formal_dir() {
  std::vector<fs::path> candidates;
  if (const char *env = std::getenv("LAZILY_FORMAL_PATH"); env && *env) {
    candidates.emplace_back(env);
  }
#ifdef LAZILY_FORMAL_DIR
  candidates.emplace_back(LAZILY_FORMAL_DIR);
#endif
  for (const auto &c : candidates) {
    if (is_formal_dir(c)) {
      std::error_code ec;
      auto canon = fs::weakly_canonical(c, ec);
      return (ec ? c : canon).string();
    }
  }
  return {};
}

bool has_lake() {
  // `lake --version` returns 0 when the toolchain is on PATH.
  return std::system("lake --version >/dev/null 2>&1") == 0;
}

} // namespace

int main() {
  const std::string formal = resolve_formal_dir();
  if (formal.empty()) {
    std::puts("[formal-check] SKIP — lazily-formal sibling not present. "
              "Clone with --recurse-submodules (or set LAZILY_FORMAL_PATH) "
              "to enable Lean proof verification.");
    return kSkip;
  }
  if (!has_lake()) {
    std::puts("[formal-check] SKIP — `lake` (Lean toolchain) not on PATH. "
              "Install Lean via elan (https://lean-lang.org/lean4/doc/setup.html) "
              "to enable proof verification.");
    return kSkip;
  }

  std::printf("[formal-check] building lazily-formal at %s ...\n", formal.c_str());
  const std::string cmd = "cd \"" + formal + "\" && lake build";
  const int rc = std::system(cmd.c_str());
  if (rc != 0) {
    std::printf("[formal-check] FAIL — `lake build` exited %d.\n", rc);
    return 1;
  }

  std::puts("[formal-check] OK — all Lean proofs in lazily-formal compile.");
  return 0;
}
