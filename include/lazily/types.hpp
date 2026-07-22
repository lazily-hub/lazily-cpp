#ifndef LAZILY_TYPES_HPP
#define LAZILY_TYPES_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace lazily {

using NodeId = int64_t;
using PeerId = int64_t;
using Epoch = int64_t;

struct SlotId {
  uint64_t value;
  // constexpr so SlotId is a literal type — usable in constant expressions and
  // compile-time sentinels (#lzcellkernel).
  constexpr SlotId() : value(0) {}
  explicit constexpr SlotId(uint64_t v) : value(v) {}
  constexpr bool operator==(const SlotId& o) const { return value == o.value; }
  constexpr bool operator!=(const SlotId& o) const { return value != o.value; }
  constexpr bool operator<(const SlotId& o) const { return value < o.value; }
};

inline constexpr int kProtocolVersion = 1;
inline constexpr const char* kProtocolId = "lazily-ipc";
inline constexpr int kProtocolMajorVersion = 1;
inline constexpr const char* kDefaultCodec = "msgpack";
inline constexpr int64_t kDefaultMaxFrameSize = 1 << 20;
inline constexpr const char* kBindingName = "lazily-cpp";

}  // namespace lazily

namespace std {
template <>
struct hash<lazily::SlotId> {
  size_t operator()(const lazily::SlotId& id) const noexcept {
    return hash<uint64_t>{}(id.value);
  }
};
}  // namespace std

#endif  // LAZILY_TYPES_HPP
