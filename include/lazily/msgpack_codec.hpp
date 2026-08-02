#ifndef LAZILY_MSGPACK_CODEC_HPP
#define LAZILY_MSGPACK_CODEC_HPP

#include <lazily/json_codec.hpp>
#include <lazily/msgpack.hpp>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lazily {

// lazily IPC wire codec — `msgpack`, the CROSS-LANGUAGE BINARY DEFAULT
// (`#lzcppmsgpackwire`).
//
// protocol.md § Frame codecs makes `msgpack` MUST-level for every binding and
// spells out that shipping *a* MessagePack codec is not implementing it: the
// codec token names ONE wire — the externally tagged frame (`{"Snapshot": …}`)
// over named-field maps whose keys are the `json` field names, with the same
// omit-when-absent rule for optional fields. codec.hpp packs the same tree as
// an INTERNALLY tagged envelope (`{"type": 0, "value": …}`) with integer `kind`
// discriminators, and also offers a positional array form protocol.md excludes
// outright. That is a private framing that happens to use MessagePack; a peer
// that negotiated `msgpack` with lazily-cpp could not decode its frames. This
// header is the wire the token actually names.
//
// It is built ON json_codec.hpp rather than beside it, and that is the point:
// the two codecs differ only in how a value tree is serialized, never in the
// SHAPE of that tree. Deriving the msgpack frame from `json_from_ipc_message`
// makes the external tags, the field names, and both `NodeKey` rules
// (`NodeSnapshot`/`NodeAdd` omit an absent key, `CrdtOp` always writes it,
// `null` when unset) identical by construction — a second hand-written
// transcription of the same shape is exactly the drift that produced
// codec.hpp's divergence.
//
// Byte payloads are ARRAYS OF INTEGERS, not MessagePack `bin`. That is what the
// reference encoder produces (`rmp_serde` serializes `Vec<u8>` through serde's
// default seq impl) and what its decoder accepts, so emitting or accepting
// `bin` would put lazily-cpp outside the wire it claims to speak.
//
// NOT byte-canonical (§ Frame codecs): a MessagePack map's key order is
// encoder-defined, so conformance is `decode(encode(m)) == m` plus a decode of
// a peer's frame, never a golden byte string. This encoder happens to be
// deterministic — allowed, but not a property any peer may rely on.
//
// Failure mode matches the other two codecs: std::runtime_error.

[[noreturn]] inline void msgpack_codec_fail(const std::string& what) {
  throw std::runtime_error("msgpack codec: " + what);
}

// -- generic DOM <-> MessagePack bridge ---------------------------------------

inline void msgpack_pack_json(MsgPacker& packer, const JsonValue& value) {
  switch (value.type) {
  case JsonValue::Type::Null:
    packer.nil();
    return;
  case JsonValue::Type::Bool:
    packer.boolean(value.boolean);
    return;
  case JsonValue::Type::Int:
    packer.i64(value.integer);
    return;
  case JsonValue::Type::Double:
    // No `IpcMessage` field is floating point (§ IpcMessage: every field is an
    // integer, string, or byte sequence). Refusing here keeps a future
    // double-valued field from silently acquiring a wire form nothing agreed
    // on, rather than encoding one this packer cannot read back.
    msgpack_codec_fail("frames carry no floating-point fields");
  case JsonValue::Type::String:
    packer.str(value.string);
    return;
  case JsonValue::Type::Array:
    packer.array_header(static_cast<uint32_t>(value.array.size()));
    for (const auto& element : value.array)
      msgpack_pack_json(packer, element);
    return;
  case JsonValue::Type::Object:
    packer.map_header(static_cast<uint32_t>(value.object.size()));
    for (const auto& member : value.object) {
      packer.str(member.first);
      msgpack_pack_json(packer, member.second);
    }
    return;
  }
  msgpack_codec_fail("unknown JSON value type");
}

inline JsonValue msgpack_unpack_json(MsgUnpacker& unpacker) {
  switch (unpacker.peek_kind()) {
  case MsgUnpacker::Kind::Nil:
    unpacker.expect_nil();
    return JsonValue::null();
  case MsgUnpacker::Kind::Bool:
    return JsonValue::of_bool(unpacker.read_bool());
  case MsgUnpacker::Kind::Int:
    return JsonValue::of_int(unpacker.read_i64());
  case MsgUnpacker::Kind::Str:
    return JsonValue::of_string(unpacker.read_str());
  case MsgUnpacker::Kind::Bin:
    // A byte payload arrives as an array of integers on this wire. The
    // reference decoder rejects `bin` in the same position, so accepting it
    // here would make lazily-cpp read frames no conforming peer can produce
    // and no conforming peer can read — a private extension wearing the
    // `msgpack` token, which is the defect this header exists to remove.
    msgpack_codec_fail("byte payloads are arrays of integers on this wire, not msgpack `bin`");
  case MsgUnpacker::Kind::Array: {
    const uint32_t count = unpacker.read_array_header();
    JsonArray out;
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
      out.push_back(msgpack_unpack_json(unpacker));
    return JsonValue::of_array(std::move(out));
  }
  case MsgUnpacker::Kind::Map: {
    const uint32_t count = unpacker.read_map_header();
    JsonObject out;
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
      if (unpacker.peek_kind() != MsgUnpacker::Kind::Str)
        msgpack_codec_fail("named-field maps require string keys");
      std::string key = unpacker.read_str();
      out.emplace_back(std::move(key), msgpack_unpack_json(unpacker));
    }
    return JsonValue::of_object(std::move(out));
  }
  default:
    msgpack_codec_fail("unsupported MessagePack value in frame");
  }
}

// -- public API ---------------------------------------------------------------

// Schema-less view of a frame's bytes. The named-field rule is a property of
// the ENCODING, so it is invisible to any assertion over a decoded
// `IpcMessage`: a positional encoder round-trips every value correctly and is
// still non-conforming. Conformance runners introspect through this.
inline JsonValue msgpack_to_json(const uint8_t* data, size_t len) {
  MsgUnpacker unpacker(data, len);
  JsonValue value = msgpack_unpack_json(unpacker);
  if (!unpacker.eof()) msgpack_codec_fail("trailing bytes after frame");
  return value;
}
inline JsonValue msgpack_to_json(const std::vector<uint8_t>& bytes) {
  return msgpack_to_json(bytes.data(), bytes.size());
}

inline std::vector<uint8_t> encode_msgpack(const IpcMessage& message) {
  MsgPacker packer;
  msgpack_pack_json(packer, json_from_ipc_message(message));
  return std::move(packer).take();
}

inline IpcMessage decode_msgpack(const uint8_t* data, size_t len) {
  return json_to_ipc_message(msgpack_to_json(data, len));
}
inline IpcMessage decode_msgpack(const std::vector<uint8_t>& bytes) {
  return decode_msgpack(bytes.data(), bytes.size());
}

} // namespace lazily

#endif // LAZILY_MSGPACK_CODEC_HPP
