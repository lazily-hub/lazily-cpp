// Compile-fail proof (`#lzcellkernel` fortification): the per-recompute
// `Compute` view must NOT be copyable, so it cannot be stored and later replayed
// to register a dependency edge against the wrong node. Built as a WILL_FAIL
// ctest — if this file ever compiles, the non-escapability guarantee (the C++
// analog of lazily-rs's lifetime + `!Send` binding) has regressed.
//
// Copy, move, and heap allocation are all deleted on `Compute`; this pins the
// copy ban specifically. The runtime `static_assert`s in
// `compute_fortification.cpp` cover move and copy-assign as well.

#include <lazily/lazily.hpp>

using namespace lazily;

int main() {
  Context ctx;
  Compute view(ctx, SlotId(0), 0);
  Compute escaped = view;  // ERROR: Compute copy constructor is deleted
  (void)escaped;
  return 0;
}
