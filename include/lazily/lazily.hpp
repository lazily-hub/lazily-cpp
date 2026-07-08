#ifndef LAZILY_LAZILY_HPP
#define LAZILY_LAZILY_HPP

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace lazily {

inline constexpr int kProtocolVersion = 1;

class Context {
 public:
  Context() = default;
};

}  // namespace lazily

#endif  // LAZILY_LAZILY_HPP
