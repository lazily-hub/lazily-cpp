#ifndef LAZILY_JSON_CODEC_HPP
#define LAZILY_JSON_CODEC_HPP

#include <lazily/hlc.hpp>
#include <lazily/ipc.hpp>
#include <lazily/json.hpp>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lazily {

// lazily IPC wire codec — `json`, the REFERENCE codec (`#lzcppjsoncodec`).
//
// protocol.md § Frame codecs: `json` is the required, dependency-free interop
// floor every binding MUST speak, the form the FFI baseline re-encodes to, and
// the form every conformance fixture is written in. Before this header
// lazily-cpp could not speak it at all — codec.hpp is a MessagePack codec, and
// tests/test_json.hpp is a fixture READER, so the binding could inspect a JSON
// frame and could not produce or consume one. That single gap is why
// `distributed/crdt_sync_frames.json` and `codec/frame_roundtrip_json.json`
// were both excused in scripts/check-conformance-coverage.sh.
//
// The wire shape is serde's EXTERNALLY TAGGED enum representation, which is
// what the canonical fixtures carry and what lazily-rs emits:
//
//   IpcMessage   {"Snapshot": {…}} / {"Delta": {…}} / {"CrdtSync": {…}} /
//                {"ResyncRequest": {…}} / {"OutboxAck": {…}}
//   NodeState    {"Payload": [bytes]} / {"SharedBlob": {…}} / "Opaque"
//   IpcValue     {"Inline": [bytes]} / {"SharedBlob": {…}}
//   DeltaOp      {"CellSet": {…}} / … / {"EdgeRemove": {…}}
//
// Note what that is NOT: codec.hpp's msgpack envelope is INTERNALLY tagged
// (`{"type": 0, "value": …}`) with integer discriminators. It is a good private
// codec and it is not this wire. Encoding the IpcMessage tree twice, in two
// shapes, is deliberate — the shapes are different protocols.
//
// Two rules the round-trip obligation turns on, both from protocol.md:
//   * an OPTIONAL `NodeKey` on NodeSnapshot / NodeAdd is OMITTED when absent in
//     self-describing codecs (§ NodeKey), so a pre-`key` decoder reads a
//     post-`key` frame unchanged;
//   * `CrdtOp.key` is written ALWAYS, `null` when unset — an anti-entropy op's
//     addressing is part of its merge identity — and a decoder must read that
//     null back as absent rather than as a key.
//
// Byte-canonical: field order is fixed by this encoder and json.hpp writes no
// whitespace, so one IpcMessage has exactly one byte form (§ Frame codecs).
//
// Failure mode matches the msgpack codec: std::runtime_error. A frame that does
// not decode is not a frame.

// -- shared helpers -----------------------------------------------------------

[[noreturn]] inline void json_codec_fail(const std::string& what) {
  throw std::runtime_error("json codec: " + what);
}

inline const JsonValue& json_required(const JsonValue& object, std::string_view key) {
  const JsonValue* found = object.find(key);
  if (found == nullptr) json_codec_fail("missing required field `" + std::string(key) + "`");
  return *found;
}

inline const JsonValue& json_required_array(const JsonValue& object, std::string_view key) {
  const JsonValue& value = json_required(object, key);
  if (!value.is_array()) json_codec_fail("field `" + std::string(key) + "` must be an array");
  return value;
}

// One externally tagged entry: `{"Tag": body}`.
inline const std::pair<std::string, JsonValue>& json_external_tag(const JsonValue& value,
                                                                  const char* what) {
  if (!value.is_object() || value.object.size() != 1)
    json_codec_fail(std::string(what) + " must be an externally tagged one-entry object");
  return value.object[0];
}

inline JsonValue json_tagged(const char* tag, JsonValue body) {
  JsonValue value = JsonValue::empty_object();
  value.set(tag, std::move(body));
  return value;
}

inline JsonValue json_from_bytes(const std::vector<uint8_t>& bytes) {
  JsonArray out;
  out.reserve(bytes.size());
  for (const uint8_t byte : bytes)
    out.push_back(JsonValue::of_int(static_cast<int64_t>(byte)));
  return JsonValue::of_array(std::move(out));
}

inline std::vector<uint8_t> json_to_bytes(const JsonValue& value) {
  if (!value.is_array()) json_codec_fail("byte payload must be a JSON array of integers");
  std::vector<uint8_t> out;
  out.reserve(value.array.size());
  for (const auto& element : value.array) {
    const int64_t byte = element.as_int();
    if (byte < 0 || byte > 255) json_codec_fail("byte payload element out of range");
    out.push_back(static_cast<uint8_t>(byte));
  }
  return out;
}

// -- ShmBlobRef ---------------------------------------------------------------

inline JsonValue json_from_shm_blob_ref(const ShmBlobRef& ref) {
  JsonValue out = JsonValue::empty_object();
  out.set("offset", JsonValue::of_int(ref.offset));
  out.set("len", JsonValue::of_int(ref.len));
  out.set("generation", JsonValue::of_int(ref.generation));
  out.set("epoch", JsonValue::of_int(ref.epoch));
  out.set("checksum", JsonValue::of_int(ref.checksum));
  // Omitted when default, so a descriptor written before the backend
  // discriminator existed round-trips byte-identically (§ zero-copy transport).
  if (ref.backend != BlobBackendKind::Shm)
    out.set("backend", JsonValue::of_string(blob_backend_kind_str(ref.backend)));
  return out;
}

inline ShmBlobRef json_to_shm_blob_ref(const JsonValue& value) {
  if (!value.is_object()) json_codec_fail("ShmBlobRef must be an object");
  ShmBlobRef ref{};
  ref.offset = json_required(value, "offset").as_int();
  ref.len = json_required(value, "len").as_int();
  ref.generation = json_required(value, "generation").as_int();
  ref.epoch = json_required(value, "epoch").as_int();
  ref.checksum = json_required(value, "checksum").as_int();
  const JsonValue* backend = value.find("backend");
  ref.backend = (backend == nullptr || backend->is_null())
                    ? BlobBackendKind::Shm
                    : blob_backend_kind_from_str(backend->as_string());
  return ref;
}

// -- WireStamp ----------------------------------------------------------------

inline JsonValue json_from_wire_stamp(const WireStamp& stamp) {
  JsonValue out = JsonValue::empty_object();
  out.set("wall_time", JsonValue::of_int(stamp.wall_time));
  out.set("logical", JsonValue::of_int(stamp.logical));
  // `peer`, never `peer_id` — the two spellings diverged once already.
  out.set("peer", JsonValue::of_int(stamp.peer));
  return out;
}

inline WireStamp json_to_wire_stamp(const JsonValue& value) {
  if (!value.is_object()) json_codec_fail("WireStamp must be an object");
  WireStamp stamp{};
  stamp.wall_time = json_required(value, "wall_time").as_int();
  stamp.logical = json_required(value, "logical").as_int();
  stamp.peer = json_required(value, "peer").as_int();
  return stamp;
}

// -- NodeKey ------------------------------------------------------------------

inline std::optional<NodeKey> json_to_optional_node_key(const JsonValue* value) {
  if (value == nullptr || value->is_null()) return std::nullopt;
  auto key = NodeKey::create(value->as_string());
  if (!key) json_codec_fail("`key` is not a valid NodeKey path");
  return key;
}

// -- NodeState / IpcValue -----------------------------------------------------

inline JsonValue json_from_node_state(const NodeState& state) {
  if (const auto* payload = std::get_if<NodeStatePayload>(&state))
    return json_tagged("Payload", json_from_bytes(payload->bytes));
  if (const auto* blob = std::get_if<NodeStateSharedBlob>(&state))
    return json_tagged("SharedBlob", json_from_shm_blob_ref(blob->blob));
  // The externally tagged UNIT variant is a bare string, NOT `{"Opaque": null}`
  // — the shape this variant is most likely to decay into under a re-encode.
  return JsonValue::of_string("Opaque");
}

inline NodeState json_to_node_state(const JsonValue& value) {
  if (value.is_string()) {
    if (value.string != "Opaque") json_codec_fail("unknown unit NodeState `" + value.string + "`");
    return NodeStateOpaque{};
  }
  const auto& tagged = json_external_tag(value, "NodeState");
  if (tagged.first == "Payload") return NodeStatePayload{json_to_bytes(tagged.second)};
  if (tagged.first == "SharedBlob") return NodeStateSharedBlob{json_to_shm_blob_ref(tagged.second)};
  json_codec_fail("unknown NodeState variant `" + tagged.first + "`");
}

inline JsonValue json_from_ipc_value(const IpcValue& value) {
  if (const auto* inline_value = std::get_if<IpcValueInline>(&value))
    return json_tagged("Inline", json_from_bytes(inline_value->bytes));
  return json_tagged("SharedBlob",
                     json_from_shm_blob_ref(std::get<IpcValueSharedBlob>(value).blob));
}

inline IpcValue json_to_ipc_value(const JsonValue& value) {
  const auto& tagged = json_external_tag(value, "IpcValue");
  if (tagged.first == "Inline") return IpcValueInline{json_to_bytes(tagged.second)};
  if (tagged.first == "SharedBlob") return IpcValueSharedBlob{json_to_shm_blob_ref(tagged.second)};
  json_codec_fail("unknown IpcValue variant `" + tagged.first + "`");
}

// -- Snapshot -----------------------------------------------------------------

inline JsonValue json_from_node_snapshot(const NodeSnapshot& node) {
  JsonValue out = JsonValue::empty_object();
  out.set("node", JsonValue::of_int(node.node));
  out.set("type_tag", JsonValue::of_string(node.type_tag));
  out.set("state", json_from_node_state(node.state));
  // Omitted when absent (§ NodeKey): self-describing codecs let a decoder that
  // predates the field read the frame unchanged.
  if (node.key) out.set("key", JsonValue::of_string(node.key->path()));
  return out;
}

inline NodeSnapshot json_to_node_snapshot(const JsonValue& value) {
  if (!value.is_object()) json_codec_fail("NodeSnapshot must be an object");
  NodeSnapshot node{};
  node.node = json_required(value, "node").as_int();
  node.type_tag = json_required(value, "type_tag").as_string();
  node.state = json_to_node_state(json_required(value, "state"));
  node.key = json_to_optional_node_key(value.find("key"));
  return node;
}

inline JsonValue json_from_edge_snapshot(const EdgeSnapshot& edge) {
  JsonValue out = JsonValue::empty_object();
  out.set("dependent", JsonValue::of_int(edge.dependent));
  out.set("dependency", JsonValue::of_int(edge.dependency));
  return out;
}

inline EdgeSnapshot json_to_edge_snapshot(const JsonValue& value) {
  if (!value.is_object()) json_codec_fail("EdgeSnapshot must be an object");
  EdgeSnapshot edge{};
  edge.dependent = json_required(value, "dependent").as_int();
  edge.dependency = json_required(value, "dependency").as_int();
  return edge;
}

inline JsonValue json_from_snapshot(const Snapshot& snapshot) {
  JsonValue out = JsonValue::empty_object();
  out.set("epoch", JsonValue::of_int(snapshot.epoch));
  JsonArray nodes;
  nodes.reserve(snapshot.nodes.size());
  for (const auto& node : snapshot.nodes)
    nodes.push_back(json_from_node_snapshot(node));
  out.set("nodes", JsonValue::of_array(std::move(nodes)));
  JsonArray edges;
  edges.reserve(snapshot.edges.size());
  for (const auto& edge : snapshot.edges)
    edges.push_back(json_from_edge_snapshot(edge));
  out.set("edges", JsonValue::of_array(std::move(edges)));
  JsonArray roots;
  roots.reserve(snapshot.roots.size());
  for (const auto root : snapshot.roots)
    roots.push_back(JsonValue::of_int(root));
  out.set("roots", JsonValue::of_array(std::move(roots)));
  return out;
}

inline Snapshot json_to_snapshot(const JsonValue& value) {
  if (!value.is_object()) json_codec_fail("Snapshot must be an object");
  Snapshot snapshot{};
  snapshot.epoch = json_required(value, "epoch").as_int();
  for (const auto& node : json_required_array(value, "nodes").array)
    snapshot.nodes.push_back(json_to_node_snapshot(node));
  for (const auto& edge : json_required_array(value, "edges").array)
    snapshot.edges.push_back(json_to_edge_snapshot(edge));
  for (const auto& root : json_required_array(value, "roots").array)
    snapshot.roots.push_back(root.as_int());
  return snapshot;
}

// -- Delta --------------------------------------------------------------------

inline JsonValue json_from_delta_op(const DeltaOp& op) {
  if (const auto* cell_set = std::get_if<DeltaOpCellSet>(&op)) {
    JsonValue body = JsonValue::empty_object();
    body.set("node", JsonValue::of_int(cell_set->node));
    body.set("payload", json_from_ipc_value(cell_set->payload));
    return json_tagged("CellSet", std::move(body));
  }
  if (const auto* slot_value = std::get_if<DeltaOpSlotValue>(&op)) {
    JsonValue body = JsonValue::empty_object();
    body.set("node", JsonValue::of_int(slot_value->node));
    body.set("payload", json_from_ipc_value(slot_value->payload));
    return json_tagged("SlotValue", std::move(body));
  }
  if (const auto* invalidate = std::get_if<DeltaOpInvalidate>(&op)) {
    JsonValue body = JsonValue::empty_object();
    body.set("node", JsonValue::of_int(invalidate->node));
    return json_tagged("Invalidate", std::move(body));
  }
  if (const auto* node_add = std::get_if<DeltaOpNodeAdd>(&op)) {
    JsonValue body = JsonValue::empty_object();
    body.set("node", JsonValue::of_int(node_add->node));
    body.set("type_tag", JsonValue::of_string(node_add->type_tag));
    body.set("state", json_from_node_state(node_add->state));
    if (node_add->key) body.set("key", JsonValue::of_string(node_add->key->path()));
    return json_tagged("NodeAdd", std::move(body));
  }
  if (const auto* node_remove = std::get_if<DeltaOpNodeRemove>(&op)) {
    JsonValue body = JsonValue::empty_object();
    body.set("node", JsonValue::of_int(node_remove->node));
    return json_tagged("NodeRemove", std::move(body));
  }
  if (const auto* edge_add = std::get_if<DeltaOpEdgeAdd>(&op)) {
    JsonValue body = JsonValue::empty_object();
    body.set("dependent", JsonValue::of_int(edge_add->dependent));
    body.set("dependency", JsonValue::of_int(edge_add->dependency));
    return json_tagged("EdgeAdd", std::move(body));
  }
  const auto& edge_remove = std::get<DeltaOpEdgeRemove>(op);
  JsonValue body = JsonValue::empty_object();
  body.set("dependent", JsonValue::of_int(edge_remove.dependent));
  body.set("dependency", JsonValue::of_int(edge_remove.dependency));
  return json_tagged("EdgeRemove", std::move(body));
}

inline DeltaOp json_to_delta_op(const JsonValue& value) {
  const auto& tagged = json_external_tag(value, "DeltaOp");
  const std::string& tag = tagged.first;
  const JsonValue& body = tagged.second;
  if (!body.is_object()) json_codec_fail("DeltaOp `" + tag + "` body must be an object");
  if (tag == "CellSet")
    return DeltaOpCellSet{json_required(body, "node").as_int(),
                          json_to_ipc_value(json_required(body, "payload"))};
  if (tag == "SlotValue")
    return DeltaOpSlotValue{json_required(body, "node").as_int(),
                            json_to_ipc_value(json_required(body, "payload"))};
  if (tag == "Invalidate") return DeltaOpInvalidate{json_required(body, "node").as_int()};
  if (tag == "NodeAdd")
    return DeltaOpNodeAdd{json_required(body, "node").as_int(),
                          json_required(body, "type_tag").as_string(),
                          json_to_node_state(json_required(body, "state")),
                          json_to_optional_node_key(body.find("key"))};
  if (tag == "NodeRemove") return DeltaOpNodeRemove{json_required(body, "node").as_int()};
  if (tag == "EdgeAdd")
    return DeltaOpEdgeAdd{json_required(body, "dependent").as_int(),
                          json_required(body, "dependency").as_int()};
  if (tag == "EdgeRemove")
    return DeltaOpEdgeRemove{json_required(body, "dependent").as_int(),
                             json_required(body, "dependency").as_int()};
  json_codec_fail("unknown DeltaOp variant `" + tag + "`");
}

inline JsonValue json_from_delta(const Delta& delta) {
  JsonValue out = JsonValue::empty_object();
  out.set("base_epoch", JsonValue::of_int(delta.base_epoch));
  out.set("epoch", JsonValue::of_int(delta.epoch));
  JsonArray ops;
  ops.reserve(delta.ops.size());
  for (const auto& op : delta.ops)
    ops.push_back(json_from_delta_op(op));
  out.set("ops", JsonValue::of_array(std::move(ops)));
  return out;
}

inline Delta json_to_delta(const JsonValue& value) {
  if (!value.is_object()) json_codec_fail("Delta must be an object");
  Delta delta{};
  delta.base_epoch = json_required(value, "base_epoch").as_int();
  delta.epoch = json_required(value, "epoch").as_int();
  for (const auto& op : json_required_array(value, "ops").array)
    delta.ops.push_back(json_to_delta_op(op));
  return delta;
}

// -- CrdtSync -----------------------------------------------------------------

inline JsonValue json_from_crdt_op(const CrdtOp& op) {
  JsonValue out = JsonValue::empty_object();
  out.set("node", JsonValue::of_int(op.node));
  // Written ALWAYS, `null` when unset — unlike NodeSnapshot.key. An
  // anti-entropy op's addressing is part of its merge identity, so the field is
  // present even when the producer assigned no key.
  out.set("key", op.key ? JsonValue::of_string(op.key->path()) : JsonValue::null());
  out.set("stamp", json_from_wire_stamp(op.stamp));
  out.set("state", json_from_ipc_value(op.state));
  return out;
}

inline CrdtOp json_to_crdt_op(const JsonValue& value) {
  if (!value.is_object()) json_codec_fail("CrdtOp must be an object");
  CrdtOp op{};
  op.node = json_required(value, "node").as_int();
  op.key = json_to_optional_node_key(value.find("key"));
  op.stamp = json_to_wire_stamp(json_required(value, "stamp"));
  op.state = json_to_ipc_value(json_required(value, "state"));
  return op;
}

inline JsonValue json_from_crdt_sync(const CrdtSync& sync) {
  JsonValue out = JsonValue::empty_object();
  // Frontier suppression (protocol.md, `#lzspecfrontiersuppress`): an empty
  // frontier is OMITTED from the wire, and an omitted frontier means "unchanged
  // since the last frame the receiver accepted" — not "the sender knows
  // nothing". Writing `"frontier": []` instead would be a different statement,
  // so the empty case must not be serialized.
  //
  // The frontier is a map on the wire encoded as pair ARRAYS (`[[peer, stamp]]`)
  // — serde's representation for `Vec<(u64, WireStamp)>`, which is what the
  // canonical fixtures carry.
  if (!sync.frontier.empty()) {
    JsonArray frontier;
    frontier.reserve(sync.frontier.size());
    for (const auto& entry : sync.frontier) {
      JsonArray pair;
      pair.push_back(JsonValue::of_int(entry.peer));
      pair.push_back(json_from_wire_stamp(entry.stamp));
      frontier.push_back(JsonValue::of_array(std::move(pair)));
    }
    out.set("frontier", JsonValue::of_array(std::move(frontier)));
  }
  JsonArray ops;
  ops.reserve(sync.ops.size());
  for (const auto& op : sync.ops)
    ops.push_back(json_from_crdt_op(op));
  out.set("ops", JsonValue::of_array(std::move(ops)));
  return out;
}

inline CrdtSync json_to_crdt_sync(const JsonValue& value) {
  if (!value.is_object()) json_codec_fail("CrdtSync must be an object");
  CrdtSync sync{};
  // Optional, per frontier suppression above: an absent `frontier` decodes as
  // empty so a suppressed frame round-trips, and so a decoder that predates the
  // relaxation is not required to see the field.
  if (const JsonValue* frontier = value.find("frontier")) {
    if (!frontier->is_null()) {
      if (!frontier->is_array()) json_codec_fail("field `frontier` must be an array");
      for (const auto& entry : frontier->array) {
        if (!entry.is_array() || entry.array.size() != 2)
          json_codec_fail("frontier entry must be a [peer, stamp] pair");
        sync.frontier.push_back(
            StampFrontierEntry{entry.array[0].as_int(), json_to_wire_stamp(entry.array[1])});
      }
    }
  }
  for (const auto& op : json_required_array(value, "ops").array)
    sync.ops.push_back(json_to_crdt_op(op));
  return sync;
}

// -- reliable-sync control frames ---------------------------------------------

inline JsonValue json_from_resync_request(const ResyncRequest& request) {
  JsonValue out = JsonValue::empty_object();
  out.set("from_epoch", JsonValue::of_int(request.from_epoch));
  return out;
}

inline ResyncRequest json_to_resync_request(const JsonValue& value) {
  if (!value.is_object()) json_codec_fail("ResyncRequest must be an object");
  return ResyncRequest{json_required(value, "from_epoch").as_int()};
}

inline JsonValue json_from_outbox_ack(const OutboxAck& ack) {
  JsonValue out = JsonValue::empty_object();
  out.set("through_epoch", JsonValue::of_int(ack.through_epoch));
  return out;
}

inline OutboxAck json_to_outbox_ack(const JsonValue& value) {
  if (!value.is_object()) json_codec_fail("OutboxAck must be an object");
  return OutboxAck{json_required(value, "through_epoch").as_int()};
}

// -- IpcMessage envelope ------------------------------------------------------

inline JsonValue json_from_ipc_message(const IpcMessage& message) {
  if (const auto* snapshot = std::get_if<IpcMessageSnapshot>(&message))
    return json_tagged("Snapshot", json_from_snapshot(snapshot->value));
  if (const auto* delta = std::get_if<IpcMessageDelta>(&message))
    return json_tagged("Delta", json_from_delta(delta->value));
  if (const auto* sync = std::get_if<IpcMessageCrdtSync>(&message))
    return json_tagged("CrdtSync", json_from_crdt_sync(sync->value));
  if (const auto* request = std::get_if<IpcMessageResyncRequest>(&message))
    return json_tagged("ResyncRequest", json_from_resync_request(request->value));
  return json_tagged("OutboxAck",
                     json_from_outbox_ack(std::get<IpcMessageOutboxAck>(message).value));
}

inline IpcMessage json_to_ipc_message(const JsonValue& value) {
  const auto& tagged = json_external_tag(value, "IpcMessage");
  const std::string& tag = tagged.first;
  if (tag == "Snapshot") return IpcMessageSnapshot{json_to_snapshot(tagged.second)};
  if (tag == "Delta") return IpcMessageDelta{json_to_delta(tagged.second)};
  if (tag == "CrdtSync") return IpcMessageCrdtSync{json_to_crdt_sync(tagged.second)};
  if (tag == "ResyncRequest") return IpcMessageResyncRequest{json_to_resync_request(tagged.second)};
  if (tag == "OutboxAck") return IpcMessageOutboxAck{json_to_outbox_ack(tagged.second)};
  json_codec_fail("unknown IpcMessage variant `" + tag + "`");
}

// The externally tagged variant name of a frame, for diagnostics and for
// runners that check a fixture's declared `variant` against what decoded.
inline const char* ipc_message_variant_name(const IpcMessage& message) {
  if (std::holds_alternative<IpcMessageSnapshot>(message)) return "Snapshot";
  if (std::holds_alternative<IpcMessageDelta>(message)) return "Delta";
  if (std::holds_alternative<IpcMessageCrdtSync>(message)) return "CrdtSync";
  if (std::holds_alternative<IpcMessageResyncRequest>(message)) return "ResyncRequest";
  return "OutboxAck";
}

inline const char* delta_op_variant_name(const DeltaOp& op) {
  if (std::holds_alternative<DeltaOpCellSet>(op)) return "CellSet";
  if (std::holds_alternative<DeltaOpSlotValue>(op)) return "SlotValue";
  if (std::holds_alternative<DeltaOpInvalidate>(op)) return "Invalidate";
  if (std::holds_alternative<DeltaOpNodeAdd>(op)) return "NodeAdd";
  if (std::holds_alternative<DeltaOpNodeRemove>(op)) return "NodeRemove";
  if (std::holds_alternative<DeltaOpEdgeAdd>(op)) return "EdgeAdd";
  return "EdgeRemove";
}

// -- public entry points ------------------------------------------------------

inline std::string encode_json(const IpcMessage& message) {
  return json_write(json_from_ipc_message(message));
}

inline IpcMessage decode_json(std::string_view text) {
  return json_to_ipc_message(json_parse(text));
}

} // namespace lazily

#endif // LAZILY_JSON_CODEC_HPP
