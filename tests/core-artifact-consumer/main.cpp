#include <lazily/core.hpp>

#include <cassert>
#include <iostream>
#include <string>

using namespace lazily;

struct ConsumerSchema {
  std::string label;
};

int main() {
  Context ctx;
  auto source = ctx.source<int>(1);
  auto computed = ctx.computed<int>([source](Compute& c) { return source.get(c) * 2; });
  int runs = 0;
  auto scope = ctx.scope();
  scope.effect([computed, &runs](Compute& c) -> CleanupFn {
    assert(computed.get(c) >= 2);
    ++runs;
    return {};
  });
  ctx.batch([&](Context& batch) {
    source.set(batch, 2);
    source.set(batch, 3);
  });
  assert(computed.get(ctx) == 6);
  assert(runs == 2);
  scope.end();
  source.set(ctx, 4);
  assert(runs == 2);
  auto typed = ctx.source<ConsumerSchema>({"typed"});
  assert(typed.get(ctx).label == "typed");
  std::cout << "lazily-cpp artifact consumer: OK\n";
}
