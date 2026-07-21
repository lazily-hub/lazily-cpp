// Compile-fail proof (`#lzcellkernel` §3/§4): writing a `Computed` must NOT
// compile. Built as a WILL_FAIL ctest — if this file ever compiles, the write
// protection has regressed and the test goes red.
//
// `set`/`merge` live only on `Source<T, M>`; `Computed<T>` is a distinct
// handle template with no such member, so the call below is a "no member named
// 'set'" error, with no shared base and no runtime gate.

#include <lazily/lazily.hpp>

using namespace lazily;

int main() {
  Context ctx;
  Computed<long long> f =
      ctx.computed<long long>([](Context&) { return 1LL; });
  f.set(ctx, 2);  // ERROR: no member named 'set' on Computed<T>
  return 0;
}
