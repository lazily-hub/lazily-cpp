#ifndef LAZILY_TYPES_HPP
#define LAZILY_TYPES_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
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
inline constexpr int64_t kDefaultMaxFrameSize = 1 << 20;
inline constexpr const char* kBindingName = "lazily-cpp";

// The frame codecs this binding speaks, as a CLOSED type rather than a free
// string (`#lzcppcodecdispatch`).
//
// protocol.md § Frame codecs defines the codec token as naming one wire, and
// `#lzcppmsgpackwire` was the cost of forgetting that: this binding advertised
// `msgpack` while a differently-shaped MessagePack framing sat behind the
// obvious `encode`/`decode` names. Negotiating a `std::string` and then leaving
// the caller to pick an encoder by hand keeps that trap open — the encoder is
// chosen by whichever free function reads best at the call site, and nothing
// checks it against what the peers agreed. A negotiated `Codec` selects the
// encoder (`codec_encode` / `codec_decode`, include/lazily/codec_dispatch.hpp),
// so the token and the bytes cannot drift apart.
//
// `postcard` is deliberately absent: protocol.md makes it a MAY for two peers
// sharing a Rust struct layout, and this binding does not implement it, so
// `codec_from_token("postcard")` fails closed rather than silently resolving to
// something else. codec.hpp's private internal framing is absent for the
// opposite reason — it is not a codec token at all and must stay unreachable
// from a negotiated one.
enum class Codec {
  // The REFERENCE codec: dependency-free interop floor, FFI baseline form,
  // byte-canonical. include/lazily/json_codec.hpp.
  Json,
  // The CROSS-LANGUAGE BINARY DEFAULT: externally tagged frames over
  // named-field maps. include/lazily/msgpack_codec.hpp.
  MsgPack,
};

// The token a peer advertises for `codec`. Written as an exhaustive switch, not
// as `codec == Codec::Json ? "json" : "msgpack"`: the comment above says
// `postcard` is a MAY this binding has not implemented, so `Codec` is a closed
// enum that is EXPECTED to gain a variant. Under the ternary the new variant
// would have advertised itself as `msgpack` and the handshake would have
// succeeded against a peer speaking a wire nobody agreed on — the exact
// token/bytes drift `#lzcppmsgpackwire` cost. A variant with no token here is a
// programming error in this binding, so it throws rather than guessing.
inline constexpr const char* codec_token(Codec codec) {
  switch (codec) {
  case Codec::Json:
    return "json";
  case Codec::MsgPack:
    return "msgpack";
  }
  throw std::invalid_argument("lazily: Codec variant has no negotiated token");
}

// nullopt for any token this binding does not speak, including the ones
// protocol.md defines but lazily-cpp has not implemented. A caller that cannot
// name the peer's codec must fail the handshake, not guess at a default.
inline std::optional<Codec> codec_from_token(std::string_view token) {
  if (token == "json") return Codec::Json;
  if (token == "msgpack") return Codec::MsgPack;
  return std::nullopt;
}

inline constexpr Codec kDefaultCodec = Codec::MsgPack;

} // namespace lazily

namespace std {
template <> struct hash<lazily::SlotId> {
  size_t operator()(const lazily::SlotId& id) const noexcept { return hash<uint64_t>{}(id.value); }
};
} // namespace std

#endif // LAZILY_TYPES_HPP
