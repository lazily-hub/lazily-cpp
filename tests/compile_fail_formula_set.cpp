// Compile-fail proof (`#lzcellkernel` §3/§4): writing a `FormulaCell` must NOT
// compile. Built as a WILL_FAIL ctest — if this file ever compiles, the write
// protection has regressed and the test goes red.
//
// `set`/`merge` live only on `Cell<T, Source<M>>`; `Cell<T, Formula>` is a
// distinct partial specialization with no such member, so the call below is a
// "no member named 'set'" error, with no shared base and no runtime gate.

#include <lazily/lazily.hpp>

using namespace lazily;

int main() {
  Context ctx;
  FormulaCell<long long> f =
      ctx.formula<long long>([](Context&) { return 1LL; });
  f.set(ctx, 2);  // ERROR: no member named 'set' on Cell<T, Formula>
  return 0;
}
