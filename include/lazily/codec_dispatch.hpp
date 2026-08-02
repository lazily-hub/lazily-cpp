#ifndef LAZILY_CODEC_DISPATCH_HPP
#define LAZILY_CODEC_DISPATCH_HPP

#include <lazily/ipc.hpp>
#include <lazily/json_codec.hpp>
#include <lazily/msgpack_codec.hpp>
#include <lazily/types.hpp>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace lazily {

// Negotiated-codec dispatch (`#lzcppcodecdispatch`).
//
// Before this header, lazily-cpp could negotiate a codec token and then had no
// supported way to USE it: `CapabilityHandshake.codec` was a free string
// compared for equality, and the three encoders were unrelated free functions.
// The one named plain `encode` is codec.hpp's PRIVATE internal framing, so the
// obvious call at a boundary that had just agreed on `msgpack` emitted the
// wrong wire — the same trap `#lzcppmsgpackwire` closed one layer down, where
// the wire itself was wrong. Fixing the wire without fixing the selection would
// leave the defect reachable by anyone who reached for the shorter name.
//
// So the negotiated token, not the call site, picks the encoder. `Codec` is a
// closed enum over the tokens this binding actually speaks (types.hpp), and the
// private framing is not one of them: it is not a codec token, and it must stay
// unreachable from a negotiated one. `codec.hpp`'s `encode`/`decode` remain
// available for what they are — same-binding serialization, chiefly the durable
// outbox — and are simply never what a handshake selects.
//
// Frames are `std::vector<uint8_t>` for every codec. `json` is text on the
// wire, but a transport carries bytes, and giving one codec a different return
// type would put the branch back at the call site.

inline std::vector<uint8_t> codec_encode(Codec codec, const IpcMessage& message) {
  switch (codec) {
  case Codec::Json: {
    const std::string text = encode_json(message);
    return std::vector<uint8_t>(text.begin(), text.end());
  }
  case Codec::MsgPack:
    return encode_msgpack(message);
  }
  throw std::runtime_error("codec dispatch: unknown codec");
}

inline IpcMessage codec_decode(Codec codec, const uint8_t* data, size_t len) {
  switch (codec) {
  case Codec::Json:
    return decode_json(std::string_view(reinterpret_cast<const char*>(data), len));
  case Codec::MsgPack:
    return decode_msgpack(data, len);
  }
  throw std::runtime_error("codec dispatch: unknown codec");
}
inline IpcMessage codec_decode(Codec codec, const std::vector<uint8_t>& bytes) {
  return codec_decode(codec, bytes.data(), bytes.size());
}

// The seam that makes a negotiated session actually speak what it negotiated.
// Prefer these over naming a codec by hand on any boundary that ran a
// handshake: the session already holds the answer, and re-deciding it at the
// call site is how the token and the bytes drift apart.
inline std::vector<uint8_t> negotiated_encode(const CapabilityHandshake& session,
                                              const IpcMessage& message) {
  return codec_encode(session.codec, message);
}

inline IpcMessage negotiated_decode(const CapabilityHandshake& session, const uint8_t* data,
                                    size_t len) {
  return codec_decode(session.codec, data, len);
}
inline IpcMessage negotiated_decode(const CapabilityHandshake& session,
                                    const std::vector<uint8_t>& bytes) {
  return codec_decode(session.codec, bytes.data(), bytes.size());
}

} // namespace lazily

#endif // LAZILY_CODEC_DISPATCH_HPP
