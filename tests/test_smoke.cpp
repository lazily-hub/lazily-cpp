#include <lazily/lazily.hpp>

#include <cassert>

int main() {
  lazily::Context ctx;
  assert(lazily::kProtocolVersion == 1);
  return 0;
}
