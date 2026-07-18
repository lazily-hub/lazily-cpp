#ifndef LAZILY_CODEC_HPP
#define LAZILY_CODEC_HPP

#include <lazily/hlc.hpp>
#include <lazily/ipc.hpp>
#include <lazily/msgpack.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lazily {

// lazily IPC wire codec — MessagePack.
//
// Encodes the closed IpcMessage variant tree (Snapshot / Delta / CrdtSync and
// their sub-types) as self-describing msgpack MAPs with short string keys.
// Unknown keys are skipped on decode for forward compatibility (older readers
// can ignore newer fields; the protocol is versioned via kProtocolMajorVersion).
// Variant types carry a discriminator field ("type"/"op"/"kind") written FIRST
// so the decoder can pick the shape before reading the remaining fields.
//
// This is the foundational serialization layer the IPC / distributed paths were
// missing (ffi.hpp previously accepted frames as-is). msgpack was chosen for
// schema-less flexibility (vs protobuf's rigid schema); capnproto was not needed
// since lazily wire types are integer/string/bytes only (no zero-copy struct
// layout requirement at this layer).

// ── Shared helpers ───────────────────────────────────────────────────────────

inline void pack_shm_blob_ref(MsgPacker& p, const ShmBlobRef& r) {
  p.map_header(6);
  p.str("offset");     p.i64(r.offset);
  p.str("len");        p.i64(r.len);
  p.str("generation"); p.i64(r.generation);
  p.str("epoch");      p.i64(r.epoch);
  p.str("checksum");   p.i64(r.checksum);
  p.str("backend");    p.str(blob_backend_kind_str(r.backend));
}
inline ShmBlobRef unpack_shm_blob_ref(MsgUnpacker& u) {
  ShmBlobRef r{};
  uint32_t n = u.read_map_header();
  for (uint32_t i = 0; i < n; ++i) {
    std::string_view k = u.read_str_view();
    if (k == "offset") r.offset = u.read_i64();
    else if (k == "len") r.len = u.read_i64();
    else if (k == "generation") r.generation = u.read_i64();
    else if (k == "epoch") r.epoch = u.read_i64();
    else if (k == "checksum") r.checksum = u.read_i64();
    else if (k == "backend") r.backend = blob_backend_kind_from_str(u.read_str_view());
    else u.skip();
  }
  return r;
}

inline void pack_wire_stamp(MsgPacker& p, const WireStamp& s) {
  p.map_header(3);
  p.str("wall_time"); p.i64(s.wall_time);
  p.str("logical");   p.i64(s.logical);
  p.str("peer");      p.i64(s.peer);
}
inline WireStamp unpack_wire_stamp(MsgUnpacker& u) {
  WireStamp s{};
  uint32_t n = u.read_map_header();
  for (uint32_t i = 0; i < n; ++i) {
    std::string_view k = u.read_str_view();
    if (k == "wall_time") s.wall_time = u.read_i64();
    else if (k == "logical") s.logical = u.read_i64();
    else if (k == "peer") s.peer = u.read_i64();
    else u.skip();
  }
  return s;
}

inline void pack_optional_node_key(MsgPacker& p, const std::optional<NodeKey>& k) {
  if (k) p.str(k->path());
  else p.nil();
}
inline std::optional<NodeKey> unpack_optional_node_key(MsgUnpacker& u) {
  if (u.peek_kind() == MsgUnpacker::Kind::Nil) {
    u.expect_nil();
    return std::nullopt;
  }
  return NodeKey::create(u.read_str_view());  // validates; nullopt if invalid
}

inline void pack_node_state(MsgPacker& p, const NodeState& s) {
  // discriminator first
  if (std::holds_alternative<NodeStatePayload>(s)) {
    p.map_header(2);
    p.str("kind"); p.i64(0);
    p.str("bytes"); p.bin(std::get<NodeStatePayload>(s).bytes);
  } else if (std::holds_alternative<NodeStateSharedBlob>(s)) {
    p.map_header(2);
    p.str("kind"); p.i64(1);
    p.str("blob"); pack_shm_blob_ref(p, std::get<NodeStateSharedBlob>(s).blob);
  } else {
    p.map_header(1);
    p.str("kind"); p.i64(2);
  }
}
inline NodeState unpack_node_state(MsgUnpacker& u) {
  uint32_t n = u.read_map_header();
  int kind = -1;
  std::vector<uint8_t> bytes;
  ShmBlobRef blob{};
  for (uint32_t i = 0; i < n; ++i) {
    std::string_view k = u.read_str_view();
    if (k == "kind") kind = static_cast<int>(u.read_i64());
    else if (k == "bytes") bytes = u.read_bin();
    else if (k == "blob") blob = unpack_shm_blob_ref(u);
    else u.skip();
  }
  if (kind == 0) return NodeStatePayload{std::move(bytes)};
  if (kind == 1) return NodeStateSharedBlob{blob};
  return NodeStateOpaque{};
}

inline void pack_ipc_value(MsgPacker& p, const IpcValue& v) {
  if (std::holds_alternative<IpcValueInline>(v)) {
    p.map_header(2);
    p.str("kind"); p.i64(0);
    p.str("bytes"); p.bin(std::get<IpcValueInline>(v).bytes);
  } else {
    p.map_header(2);
    p.str("kind"); p.i64(1);
    p.str("blob"); pack_shm_blob_ref(p, std::get<IpcValueSharedBlob>(v).blob);
  }
}
inline IpcValue unpack_ipc_value(MsgUnpacker& u) {
  uint32_t n = u.read_map_header();
  int kind = -1;
  std::vector<uint8_t> bytes;
  ShmBlobRef blob{};
  for (uint32_t i = 0; i < n; ++i) {
    std::string_view k = u.read_str_view();
    if (k == "kind") kind = static_cast<int>(u.read_i64());
    else if (k == "bytes") bytes = u.read_bin();
    else if (k == "blob") blob = unpack_shm_blob_ref(u);
    else u.skip();
  }
  if (kind == 0) return IpcValueInline{std::move(bytes)};
  return IpcValueSharedBlob{blob};
}

// ── Snapshot ─────────────────────────────────────────────────────────────────

inline void pack_snapshot(MsgPacker& p, const Snapshot& s) {
  // ~64 B/node typical (map header + 4 keyed fields incl. short NodeKey path),
  // ~24 B/edge, ~9 B/root. Reserve once so push_back avoids logarithmic
  // re-growths over the course of the message (#lzcppreservehint).
  p.reserve_hint(16 + s.nodes.size() * 64 + s.edges.size() * 24 + s.roots.size() * 9);
  p.map_header(4);
  p.str("epoch"); p.i64(s.epoch);
  p.str("nodes"); {
    p.array_header(static_cast<uint32_t>(s.nodes.size()));
    for (auto& nd : s.nodes) {
      p.map_header(4);
      p.str("node"); p.i64(nd.node);
      p.str("type_tag"); p.str(nd.type_tag);
      p.str("state"); pack_node_state(p, nd.state);
      p.str("key"); pack_optional_node_key(p, nd.key);
    }
  }
  p.str("edges"); {
    p.array_header(static_cast<uint32_t>(s.edges.size()));
    for (auto& e : s.edges) {
      p.map_header(2);
      p.str("dependent"); p.i64(e.dependent);
      p.str("dependency"); p.i64(e.dependency);
    }
  }
  p.str("roots"); {
    p.array_header(static_cast<uint32_t>(s.roots.size()));
    for (auto r : s.roots) p.i64(r);
  }
}
inline Snapshot unpack_snapshot(MsgUnpacker& u) {
  Snapshot s{};
  uint32_t n = u.read_map_header();
  for (uint32_t i = 0; i < n; ++i) {
    std::string_view k = u.read_str_view();
    if (k == "epoch") s.epoch = u.read_i64();
    else if (k == "nodes") {
      uint32_t m = u.read_array_header();
      s.nodes.reserve(m);
      for (uint32_t j = 0; j < m; ++j) {
        NodeSnapshot nd;
        uint32_t fn = u.read_map_header();
        for (uint32_t f = 0; f < fn; ++f) {
          std::string_view fk = u.read_str_view();
          if (fk == "node") nd.node = u.read_i64();
          else if (fk == "type_tag") nd.type_tag = u.read_str();
          else if (fk == "state") nd.state = unpack_node_state(u);
          else if (fk == "key") nd.key = unpack_optional_node_key(u);
          else u.skip();
        }
        s.nodes.push_back(std::move(nd));
      }
    } else if (k == "edges") {
      uint32_t m = u.read_array_header();
      s.edges.reserve(m);
      for (uint32_t j = 0; j < m; ++j) {
        EdgeSnapshot e;
        uint32_t fn = u.read_map_header();
        for (uint32_t f = 0; f < fn; ++f) {
          std::string_view fk = u.read_str_view();
          if (fk == "dependent") e.dependent = u.read_i64();
          else if (fk == "dependency") e.dependency = u.read_i64();
          else u.skip();
        }
        s.edges.push_back(e);
      }
    } else if (k == "roots") {
      uint32_t m = u.read_array_header();
      s.roots.reserve(m);
      for (uint32_t j = 0; j < m; ++j) s.roots.push_back(u.read_i64());
    } else {
      u.skip();
    }
  }
  return s;
}

// ── Delta ────────────────────────────────────────────────────────────────────

inline void pack_delta(MsgPacker& p, const Delta& d) {
  // DeltaOp encodings are ~24–80 B each (discriminator + node id + payload/
  // state). Reserve once at the top so push_back avoids re-growths
  // (#lzcppreservehint).
  p.reserve_hint(16 + d.ops.size() * 64);
  p.map_header(3);
  p.str("base_epoch"); p.i64(d.base_epoch);
  p.str("epoch"); p.i64(d.epoch);
  p.str("ops"); {
    p.array_header(static_cast<uint32_t>(d.ops.size()));
    for (auto& op : d.ops) {
      // discriminator "op" written first.
      if (std::holds_alternative<DeltaOpCellSet>(op)) {
        auto& v = std::get<DeltaOpCellSet>(op);
        p.map_header(3); p.str("op"); p.i64(0);
        p.str("node"); p.i64(v.node);
        p.str("payload"); pack_ipc_value(p, v.payload);
      } else if (std::holds_alternative<DeltaOpSlotValue>(op)) {
        auto& v = std::get<DeltaOpSlotValue>(op);
        p.map_header(3); p.str("op"); p.i64(1);
        p.str("node"); p.i64(v.node);
        p.str("payload"); pack_ipc_value(p, v.payload);
      } else if (std::holds_alternative<DeltaOpInvalidate>(op)) {
        auto& v = std::get<DeltaOpInvalidate>(op);
        p.map_header(2); p.str("op"); p.i64(2);
        p.str("node"); p.i64(v.node);
      } else if (std::holds_alternative<DeltaOpNodeAdd>(op)) {
        auto& v = std::get<DeltaOpNodeAdd>(op);
        p.map_header(5); p.str("op"); p.i64(3);
        p.str("node"); p.i64(v.node);
        p.str("type_tag"); p.str(v.type_tag);
        p.str("state"); pack_node_state(p, v.state);
        p.str("key"); pack_optional_node_key(p, v.key);
      } else if (std::holds_alternative<DeltaOpNodeRemove>(op)) {
        auto& v = std::get<DeltaOpNodeRemove>(op);
        p.map_header(2); p.str("op"); p.i64(4);
        p.str("node"); p.i64(v.node);
      } else if (std::holds_alternative<DeltaOpEdgeAdd>(op)) {
        auto& v = std::get<DeltaOpEdgeAdd>(op);
        p.map_header(3); p.str("op"); p.i64(5);
        p.str("dependent"); p.i64(v.dependent);
        p.str("dependency"); p.i64(v.dependency);
      } else {
        auto& v = std::get<DeltaOpEdgeRemove>(op);
        p.map_header(3); p.str("op"); p.i64(6);
        p.str("dependent"); p.i64(v.dependent);
        p.str("dependency"); p.i64(v.dependency);
      }
    }
  }
}
inline Delta unpack_delta(MsgUnpacker& u) {
  Delta d{};
  uint32_t n = u.read_map_header();
  for (uint32_t i = 0; i < n; ++i) {
    std::string_view k = u.read_str_view();
    if (k == "base_epoch") d.base_epoch = u.read_i64();
    else if (k == "epoch") d.epoch = u.read_i64();
    else if (k == "ops") {
      uint32_t m = u.read_array_header();
      d.ops.reserve(m);
      for (uint32_t j = 0; j < m; ++j) {
        uint32_t fn = u.read_map_header();
        int op_kind = -1;
        // First pass: read all fields generically into temporaries.
        NodeId node = 0, dependent = 0, dependency = 0;
        std::string type_tag;
        NodeState state = NodeStateOpaque{};
        std::optional<NodeKey> key;
        IpcValue payload = IpcValueInline{};
        for (uint32_t f = 0; f < fn; ++f) {
          std::string_view fk = u.read_str_view();
          if (fk == "op") op_kind = static_cast<int>(u.read_i64());
          else if (fk == "node") node = u.read_i64();
          else if (fk == "dependent") dependent = u.read_i64();
          else if (fk == "dependency") dependency = u.read_i64();
          else if (fk == "type_tag") type_tag = u.read_str();
          else if (fk == "state") state = unpack_node_state(u);
          else if (fk == "key") key = unpack_optional_node_key(u);
          else if (fk == "payload") payload = unpack_ipc_value(u);
          else u.skip();
        }
        switch (op_kind) {
          case 0: d.ops.push_back(DeltaOpCellSet{node, std::move(payload)}); break;
          case 1: d.ops.push_back(DeltaOpSlotValue{node, std::move(payload)}); break;
          case 2: d.ops.push_back(DeltaOpInvalidate{node}); break;
          case 3: d.ops.push_back(DeltaOpNodeAdd{node, std::move(type_tag), std::move(state), std::move(key)}); break;
          case 4: d.ops.push_back(DeltaOpNodeRemove{node}); break;
          case 5: d.ops.push_back(DeltaOpEdgeAdd{dependent, dependency}); break;
          case 6: d.ops.push_back(DeltaOpEdgeRemove{dependent, dependency}); break;
          default: throw std::runtime_error("codec: unknown DeltaOp kind");
        }
      }
    } else {
      u.skip();
    }
  }
  return d;
}

// ── CrdtSync ─────────────────────────────────────────────────────────────────

inline void pack_crdt_sync(MsgPacker& p, const CrdtSync& c) {
  // ~32 B/frontier entry, ~64 B/op (node + key + stamp + state payload).
  p.reserve_hint(16 + c.frontier.size() * 32 + c.ops.size() * 64);
  p.map_header(2);
  p.str("frontier"); {
    p.array_header(static_cast<uint32_t>(c.frontier.size()));
    for (auto& e : c.frontier) {
      p.map_header(2);
      p.str("peer"); p.i64(e.peer);
      p.str("stamp"); pack_wire_stamp(p, e.stamp);
    }
  }
  p.str("ops"); {
    p.array_header(static_cast<uint32_t>(c.ops.size()));
    for (auto& o : c.ops) {
      p.map_header(4);
      p.str("node"); p.i64(o.node);
      p.str("key"); pack_optional_node_key(p, o.key);
      p.str("stamp"); pack_wire_stamp(p, o.stamp);
      p.str("state"); pack_ipc_value(p, o.state);
    }
  }
}
inline CrdtSync unpack_crdt_sync(MsgUnpacker& u) {
  CrdtSync c{};
  uint32_t n = u.read_map_header();
  for (uint32_t i = 0; i < n; ++i) {
    std::string_view k = u.read_str_view();
    if (k == "frontier") {
      uint32_t m = u.read_array_header();
      c.frontier.reserve(m);
      for (uint32_t j = 0; j < m; ++j) {
        StampFrontierEntry e{};
        uint32_t fn = u.read_map_header();
        for (uint32_t f = 0; f < fn; ++f) {
          std::string_view fk = u.read_str_view();
          if (fk == "peer") e.peer = u.read_i64();
          else if (fk == "stamp") e.stamp = unpack_wire_stamp(u);
          else u.skip();
        }
        c.frontier.push_back(e);
      }
    } else if (k == "ops") {
      uint32_t m = u.read_array_header();
      c.ops.reserve(m);
      for (uint32_t j = 0; j < m; ++j) {
        CrdtOp o{};
        uint32_t fn = u.read_map_header();
        for (uint32_t f = 0; f < fn; ++f) {
          std::string_view fk = u.read_str_view();
          if (fk == "node") o.node = u.read_i64();
          else if (fk == "key") o.key = unpack_optional_node_key(u);
          else if (fk == "stamp") o.stamp = unpack_wire_stamp(u);
          else if (fk == "state") o.state = unpack_ipc_value(u);
          else u.skip();
        }
        c.ops.push_back(std::move(o));
      }
    } else {
      u.skip();
    }
  }
  return c;
}

// ── Reliable-sync control frames (`#lzsync`) ─────────────────────────────────

inline void pack_resync_request(MsgPacker& p, const ResyncRequest& r) {
  p.map_header(1); p.str("from_epoch"); p.i64(r.from_epoch);
}
inline ResyncRequest unpack_resync_request(MsgUnpacker& u) {
  uint32_t n = u.read_map_header();
  ResyncRequest r{0};
  for (uint32_t i = 0; i < n; ++i) {
    std::string_view k = u.read_str_view();
    if (k == "from_epoch") r.from_epoch = u.read_i64();
    else u.skip();
  }
  return r;
}

inline void pack_outbox_ack(MsgPacker& p, const OutboxAck& a) {
  p.map_header(1); p.str("through_epoch"); p.i64(a.through_epoch);
}
inline OutboxAck unpack_outbox_ack(MsgUnpacker& u) {
  uint32_t n = u.read_map_header();
  OutboxAck a{0};
  for (uint32_t i = 0; i < n; ++i) {
    std::string_view k = u.read_str_view();
    if (k == "through_epoch") a.through_epoch = u.read_i64();
    else u.skip();
  }
  return a;
}

// ── Top-level IpcMessage ─────────────────────────────────────────────────────

inline void pack_ipc_message(MsgPacker& p, const IpcMessage& m) {
  // discriminator "type" first.
  if (std::holds_alternative<IpcMessageSnapshot>(m)) {
    p.map_header(2); p.str("type"); p.i64(0);
    p.str("value"); pack_snapshot(p, std::get<IpcMessageSnapshot>(m).value);
  } else if (std::holds_alternative<IpcMessageDelta>(m)) {
    p.map_header(2); p.str("type"); p.i64(1);
    p.str("value"); pack_delta(p, std::get<IpcMessageDelta>(m).value);
  } else if (std::holds_alternative<IpcMessageCrdtSync>(m)) {
    p.map_header(2); p.str("type"); p.i64(2);
    p.str("value"); pack_crdt_sync(p, std::get<IpcMessageCrdtSync>(m).value);
  } else if (std::holds_alternative<IpcMessageResyncRequest>(m)) {
    p.map_header(2); p.str("type"); p.i64(3);
    p.str("value"); pack_resync_request(p, std::get<IpcMessageResyncRequest>(m).value);
  } else {
    p.map_header(2); p.str("type"); p.i64(4);
    p.str("value"); pack_outbox_ack(p, std::get<IpcMessageOutboxAck>(m).value);
  }
}
inline IpcMessage unpack_ipc_message(MsgUnpacker& u) {
  uint32_t n = u.read_map_header();
  int type = -1;
  // The canonical encoding writes "type" first; decode generically: read fields,
  // capture type, and defer value decode. Since value follows type on the wire,
  // read in encounter order.
  // We loop; if we hit "value" before "type" (non-canonical), we must buffer —
  // but our encoder always writes type first, so a single pass suffices.
  for (uint32_t i = 0; i < n; ++i) {
    std::string_view k = u.read_str_view();
    if (k == "type") {
      type = static_cast<int>(u.read_i64());
    } else if (k == "value") {
      switch (type) {
        case 0: return IpcMessageSnapshot{unpack_snapshot(u)};
        case 1: return IpcMessageDelta{unpack_delta(u)};
        case 2: return IpcMessageCrdtSync{unpack_crdt_sync(u)};
        case 3: return IpcMessageResyncRequest{unpack_resync_request(u)};
        case 4: return IpcMessageOutboxAck{unpack_outbox_ack(u)};
        default:
          throw std::runtime_error("codec: value before type (non-canonical) or unknown type");
      }
    } else {
      u.skip();
    }
  }
  throw std::runtime_error("codec: IpcMessage missing value");
}

// ═══════════════════════════════════════════════════════════════════════════
// Positional array codec (#lzcpppositionalcodec).
//
// Schema-versioned positional MessagePack arrays as a wire-size- and decode-
// cost-efficient alternative to the string-keyed MAPs above. Every type that
// has a `pack_*` also has a `pack_*_positional`: same field order, but keys
// are dropped and a schema-version integer leads each frame so the layout can
// evolve. Variants carry their discriminator inline (second array element).
//
// Wire shape:
//   WireStamp        : array(4); i64(schema); i64(wall_time); i64(logical); i64(peer)
//   ShmBlobRef       : array(7); i64(schema); i64(offset); i64(len);
//                          i64(generation); i64(epoch); i64(checksum); str(backend)
//   NodeState        : array(3); i64(schema); i64(kind); <bin|blobref|nil>
//   IpcValue         : array(3); i64(schema); i64(kind); <bin|blobref>
//   NodeSnapshot     : array(5); i64(schema); i64(node); str(type_tag);
//                          <state-pos>; <nil|str(key)>
//   EdgeSnapshot     : array(3); i64(schema); i64(dependent); i64(dependency)
//   Snapshot         : array(5); i64(schema); i64(epoch);
//                          array(NodeSnapshot-pos); array(Edge-pos); array(i64)
//   StampFrontierEnt : array(3); i64(schema); i64(peer); <WireStamp-pos>
//   CrdtOp           : array(5); i64(schema); i64(node); <nil|str(key)>;
//                          <WireStamp-pos>; <IpcValue-pos>
//   CrdtSync         : array(3); i64(schema); array(frontier); array(ops)
//   DeltaOp          : array(2); i64(op_kind); <op-specific fields array>
//   Delta            : array(4); i64(schema); i64(base_epoch); i64(epoch);
//                          array(DeltaOp-pos)
//   ResyncRequest    : array(2); i64(schema); i64(from_epoch)
//   OutboxAck        : array(2); i64(schema); i64(through_epoch)
//   IpcMessage env   : array(3); i64(codec_version); i64(type_disc);
//                          <value-positional>
//
// Decode is format-tolerant: `decode()` peeks the top-level byte — a MAP kind
// routes to the legacy string-keyed path, an ARRAY kind routes to positional.
// So positional-encoded frames interoperate with legacy readers that call
// `decode()`, and vice versa.
// ═══════════════════════════════════════════════════════════════════════════

// Single codec-version discriminator for the whole positional envelope. Bump
// when any positional schema changes; old readers reject newer frames with a
// runtime_error instead of misinterpreting fields.
inline constexpr int64_t kPositionalCodecVersion = 1;

// Per-type schema versions allow individual types to evolve without bumping
// the envelope version. They are written but currently all 1; readers reject
// unknown values.
inline constexpr int64_t kWireStampPosSchema = 1;
inline constexpr int64_t kShmBlobRefPosSchema = 1;
inline constexpr int64_t kNodeStatePosSchema = 1;
inline constexpr int64_t kIpcValuePosSchema = 1;
inline constexpr int64_t kNodeSnapshotPosSchema = 1;
inline constexpr int64_t kEdgeSnapshotPosSchema = 1;
inline constexpr int64_t kSnapshotPosSchema = 1;
inline constexpr int64_t kStampFrontierEntryPosSchema = 1;
inline constexpr int64_t kCrdtOpPosSchema = 1;
inline constexpr int64_t kCrdtSyncPosSchema = 1;
inline constexpr int64_t kDeltaPosSchema = 1;
inline constexpr int64_t kResyncRequestPosSchema = 1;
inline constexpr int64_t kOutboxAckPosSchema = 1;

// ── Helpers for optional<NodeKey> positional representation ──
//
// Encoded as nil when absent, or the raw path str when present (a NodeKey is
// just a validated path string on the wire).
inline void pack_optional_node_key_positional(MsgPacker& p,
                                              const std::optional<NodeKey>& k) {
  if (k) p.str(k->path());
  else p.nil();
}
inline std::optional<NodeKey> unpack_optional_node_key_positional(MsgUnpacker& u) {
  if (u.peek_kind() == MsgUnpacker::Kind::Nil) {
    u.expect_nil();
    return std::nullopt;
  }
  return NodeKey::create(u.read_str_view());
}

// ── WireStamp ──
inline void pack_wire_stamp_positional(MsgPacker& p, const WireStamp& s) {
  p.array_header(4);
  p.i64(kWireStampPosSchema);
  p.i64(s.wall_time);
  p.i64(s.logical);
  p.i64(s.peer);
}
inline WireStamp unpack_wire_stamp_positional(MsgUnpacker& u) {
  uint32_t n = u.read_array_header();
  if (n < 4) throw std::runtime_error("codec: WireStamp positional arity < 4");
  int64_t schema = u.read_i64();
  if (schema != kWireStampPosSchema)
    throw std::runtime_error("codec: WireStamp positional schema mismatch");
  WireStamp s{};
  s.wall_time = u.read_i64();
  s.logical = u.read_i64();
  s.peer = u.read_i64();
  for (uint32_t i = 4; i < n; ++i) u.skip();  // forward-compat extra fields
  return s;
}

// ── ShmBlobRef ──
inline void pack_shm_blob_ref_positional(MsgPacker& p, const ShmBlobRef& r) {
  p.array_header(7);
  p.i64(kShmBlobRefPosSchema);
  p.i64(r.offset);
  p.i64(r.len);
  p.i64(r.generation);
  p.i64(r.epoch);
  p.i64(r.checksum);
  p.str(blob_backend_kind_str(r.backend));
}
inline ShmBlobRef unpack_shm_blob_ref_positional(MsgUnpacker& u) {
  uint32_t n = u.read_array_header();
  if (n < 7) throw std::runtime_error("codec: ShmBlobRef positional arity < 7");
  int64_t schema = u.read_i64();
  if (schema != kShmBlobRefPosSchema)
    throw std::runtime_error("codec: ShmBlobRef positional schema mismatch");
  ShmBlobRef r{};
  r.offset = u.read_i64();
  r.len = u.read_i64();
  r.generation = u.read_i64();
  r.epoch = u.read_i64();
  r.checksum = u.read_i64();
  r.backend = blob_backend_kind_from_str(u.read_str_view());
  for (uint32_t i = 7; i < n; ++i) u.skip();
  return r;
}

// ── NodeState ──
inline void pack_node_state_positional(MsgPacker& p, const NodeState& s) {
  if (std::holds_alternative<NodeStatePayload>(s)) {
    p.array_header(3);
    p.i64(kNodeStatePosSchema);
    p.i64(0);
    p.bin(std::get<NodeStatePayload>(s).bytes);
  } else if (std::holds_alternative<NodeStateSharedBlob>(s)) {
    p.array_header(3);
    p.i64(kNodeStatePosSchema);
    p.i64(1);
    pack_shm_blob_ref_positional(p, std::get<NodeStateSharedBlob>(s).blob);
  } else {
    p.array_header(3);
    p.i64(kNodeStatePosSchema);
    p.i64(2);
    p.nil();
  }
}
inline NodeState unpack_node_state_positional(MsgUnpacker& u) {
  uint32_t n = u.read_array_header();
  if (n < 3) throw std::runtime_error("codec: NodeState positional arity < 3");
  int64_t schema = u.read_i64();
  if (schema != kNodeStatePosSchema)
    throw std::runtime_error("codec: NodeState positional schema mismatch");
  int64_t kind = u.read_i64();
  NodeState state = NodeStateOpaque{};
  if (kind == 0) {
    NodeStatePayload pl;
    pl.bytes = u.read_bin();
    state = std::move(pl);
  } else if (kind == 1) {
    ShmBlobRef blob = unpack_shm_blob_ref_positional(u);
    state = NodeStateSharedBlob{blob};
  } else if (kind == 2) {
    u.expect_nil();
  } else {
    throw std::runtime_error("codec: NodeState positional unknown kind");
  }
  for (uint32_t i = 3; i < n; ++i) u.skip();
  return state;
}

// ── IpcValue ──
inline void pack_ipc_value_positional(MsgPacker& p, const IpcValue& v) {
  if (std::holds_alternative<IpcValueInline>(v)) {
    p.array_header(3);
    p.i64(kIpcValuePosSchema);
    p.i64(0);
    p.bin(std::get<IpcValueInline>(v).bytes);
  } else {
    p.array_header(3);
    p.i64(kIpcValuePosSchema);
    p.i64(1);
    pack_shm_blob_ref_positional(p, std::get<IpcValueSharedBlob>(v).blob);
  }
}
inline IpcValue unpack_ipc_value_positional(MsgUnpacker& u) {
  uint32_t n = u.read_array_header();
  if (n < 3) throw std::runtime_error("codec: IpcValue positional arity < 3");
  int64_t schema = u.read_i64();
  if (schema != kIpcValuePosSchema)
    throw std::runtime_error("codec: IpcValue positional schema mismatch");
  int64_t kind = u.read_i64();
  IpcValue value = IpcValueInline{};
  if (kind == 0) {
    IpcValueInline inl;
    inl.bytes = u.read_bin();
    value = std::move(inl);
  } else if (kind == 1) {
    ShmBlobRef blob = unpack_shm_blob_ref_positional(u);
    value = IpcValueSharedBlob{blob};
  } else {
    throw std::runtime_error("codec: IpcValue positional unknown kind");
  }
  for (uint32_t i = 3; i < n; ++i) u.skip();
  return value;
}

// ── NodeSnapshot / EdgeSnapshot ──
inline void pack_node_snapshot_positional(MsgPacker& p, const NodeSnapshot& nd) {
  p.array_header(5);
  p.i64(kNodeSnapshotPosSchema);
  p.i64(nd.node);
  p.str(nd.type_tag);
  pack_node_state_positional(p, nd.state);
  pack_optional_node_key_positional(p, nd.key);
}
inline NodeSnapshot unpack_node_snapshot_positional(MsgUnpacker& u) {
  uint32_t n = u.read_array_header();
  if (n < 5) throw std::runtime_error("codec: NodeSnapshot positional arity < 5");
  int64_t schema = u.read_i64();
  if (schema != kNodeSnapshotPosSchema)
    throw std::runtime_error("codec: NodeSnapshot positional schema mismatch");
  NodeSnapshot nd;
  nd.node = u.read_i64();
  nd.type_tag = u.read_str();
  nd.state = unpack_node_state_positional(u);
  nd.key = unpack_optional_node_key_positional(u);
  for (uint32_t i = 5; i < n; ++i) u.skip();
  return nd;
}

inline void pack_edge_snapshot_positional(MsgPacker& p, const EdgeSnapshot& e) {
  p.array_header(3);
  p.i64(kEdgeSnapshotPosSchema);
  p.i64(e.dependent);
  p.i64(e.dependency);
}
inline EdgeSnapshot unpack_edge_snapshot_positional(MsgUnpacker& u) {
  uint32_t n = u.read_array_header();
  if (n < 3) throw std::runtime_error("codec: EdgeSnapshot positional arity < 3");
  int64_t schema = u.read_i64();
  if (schema != kEdgeSnapshotPosSchema)
    throw std::runtime_error("codec: EdgeSnapshot positional schema mismatch");
  EdgeSnapshot e;
  e.dependent = u.read_i64();
  e.dependency = u.read_i64();
  for (uint32_t i = 3; i < n; ++i) u.skip();
  return e;
}

// ── Snapshot ──
inline void pack_snapshot_positional(MsgPacker& p, const Snapshot& s) {
  // Same per-element sizing rationale as the string-keyed packer.
  p.reserve_hint(16 + s.nodes.size() * 32 + s.edges.size() * 12 + s.roots.size() * 9);
  p.array_header(5);
  p.i64(kSnapshotPosSchema);
  p.i64(s.epoch);
  p.array_header(static_cast<uint32_t>(s.nodes.size()));
  for (auto& nd : s.nodes) pack_node_snapshot_positional(p, nd);
  p.array_header(static_cast<uint32_t>(s.edges.size()));
  for (auto& e : s.edges) pack_edge_snapshot_positional(p, e);
  p.array_header(static_cast<uint32_t>(s.roots.size()));
  for (auto r : s.roots) p.i64(r);
}
inline Snapshot unpack_snapshot_positional(MsgUnpacker& u) {
  uint32_t n = u.read_array_header();
  if (n < 5) throw std::runtime_error("codec: Snapshot positional arity < 5");
  int64_t schema = u.read_i64();
  if (schema != kSnapshotPosSchema)
    throw std::runtime_error("codec: Snapshot positional schema mismatch");
  Snapshot s;
  s.epoch = u.read_i64();
  uint32_t m = u.read_array_header();
  s.nodes.reserve(m);
  for (uint32_t i = 0; i < m; ++i)
    s.nodes.push_back(unpack_node_snapshot_positional(u));
  m = u.read_array_header();
  s.edges.reserve(m);
  for (uint32_t i = 0; i < m; ++i)
    s.edges.push_back(unpack_edge_snapshot_positional(u));
  m = u.read_array_header();
  s.roots.reserve(m);
  for (uint32_t i = 0; i < m; ++i) s.roots.push_back(u.read_i64());
  for (uint32_t i = 5; i < n; ++i) u.skip();
  return s;
}

// ── StampFrontierEntry / CrdtOp / CrdtSync ──
inline void pack_stamp_frontier_entry_positional(MsgPacker& p,
                                                 const StampFrontierEntry& e) {
  p.array_header(3);
  p.i64(kStampFrontierEntryPosSchema);
  p.i64(e.peer);
  pack_wire_stamp_positional(p, e.stamp);
}
inline StampFrontierEntry unpack_stamp_frontier_entry_positional(MsgUnpacker& u) {
  uint32_t n = u.read_array_header();
  if (n < 3)
    throw std::runtime_error("codec: StampFrontierEntry positional arity < 3");
  int64_t schema = u.read_i64();
  if (schema != kStampFrontierEntryPosSchema)
    throw std::runtime_error("codec: StampFrontierEntry positional schema mismatch");
  StampFrontierEntry e{};
  e.peer = u.read_i64();
  e.stamp = unpack_wire_stamp_positional(u);
  for (uint32_t i = 3; i < n; ++i) u.skip();
  return e;
}

inline void pack_crdt_op_positional(MsgPacker& p, const CrdtOp& o) {
  p.array_header(5);
  p.i64(kCrdtOpPosSchema);
  p.i64(o.node);
  pack_optional_node_key_positional(p, o.key);
  pack_wire_stamp_positional(p, o.stamp);
  pack_ipc_value_positional(p, o.state);
}
inline CrdtOp unpack_crdt_op_positional(MsgUnpacker& u) {
  uint32_t n = u.read_array_header();
  if (n < 5) throw std::runtime_error("codec: CrdtOp positional arity < 5");
  int64_t schema = u.read_i64();
  if (schema != kCrdtOpPosSchema)
    throw std::runtime_error("codec: CrdtOp positional schema mismatch");
  CrdtOp o;
  o.node = u.read_i64();
  o.key = unpack_optional_node_key_positional(u);
  o.stamp = unpack_wire_stamp_positional(u);
  o.state = unpack_ipc_value_positional(u);
  for (uint32_t i = 5; i < n; ++i) u.skip();
  return o;
}

inline void pack_crdt_sync_positional(MsgPacker& p, const CrdtSync& c) {
  p.reserve_hint(16 + c.frontier.size() * 24 + c.ops.size() * 48);
  p.array_header(3);
  p.i64(kCrdtSyncPosSchema);
  p.array_header(static_cast<uint32_t>(c.frontier.size()));
  for (auto& e : c.frontier) pack_stamp_frontier_entry_positional(p, e);
  p.array_header(static_cast<uint32_t>(c.ops.size()));
  for (auto& o : c.ops) pack_crdt_op_positional(p, o);
}
inline CrdtSync unpack_crdt_sync_positional(MsgUnpacker& u) {
  uint32_t n = u.read_array_header();
  if (n < 3) throw std::runtime_error("codec: CrdtSync positional arity < 3");
  int64_t schema = u.read_i64();
  if (schema != kCrdtSyncPosSchema)
    throw std::runtime_error("codec: CrdtSync positional schema mismatch");
  CrdtSync c;
  uint32_t m = u.read_array_header();
  c.frontier.reserve(m);
  for (uint32_t i = 0; i < m; ++i)
    c.frontier.push_back(unpack_stamp_frontier_entry_positional(u));
  m = u.read_array_header();
  c.ops.reserve(m);
  for (uint32_t i = 0; i < m; ++i)
    c.ops.push_back(unpack_crdt_op_positional(u));
  for (uint32_t i = 3; i < n; ++i) u.skip();
  return c;
}

// ── DeltaOp / Delta ──
//
// DeltaOp variants have different field counts, so the positional form is a
// 2-element array: [op_kind, fields-array]. The fields-array arity is
// op-specific and documented inline below.
inline void pack_delta_op_positional(MsgPacker& p, const DeltaOp& op) {
  p.array_header(2);
  if (std::holds_alternative<DeltaOpCellSet>(op)) {
    auto& v = std::get<DeltaOpCellSet>(op);
    p.i64(0);
    p.array_header(2);
    p.i64(v.node);
    pack_ipc_value_positional(p, v.payload);
  } else if (std::holds_alternative<DeltaOpSlotValue>(op)) {
    auto& v = std::get<DeltaOpSlotValue>(op);
    p.i64(1);
    p.array_header(2);
    p.i64(v.node);
    pack_ipc_value_positional(p, v.payload);
  } else if (std::holds_alternative<DeltaOpInvalidate>(op)) {
    auto& v = std::get<DeltaOpInvalidate>(op);
    p.i64(2);
    p.array_header(1);
    p.i64(v.node);
  } else if (std::holds_alternative<DeltaOpNodeAdd>(op)) {
    auto& v = std::get<DeltaOpNodeAdd>(op);
    p.i64(3);
    p.array_header(4);
    p.i64(v.node);
    p.str(v.type_tag);
    pack_node_state_positional(p, v.state);
    pack_optional_node_key_positional(p, v.key);
  } else if (std::holds_alternative<DeltaOpNodeRemove>(op)) {
    auto& v = std::get<DeltaOpNodeRemove>(op);
    p.i64(4);
    p.array_header(1);
    p.i64(v.node);
  } else if (std::holds_alternative<DeltaOpEdgeAdd>(op)) {
    auto& v = std::get<DeltaOpEdgeAdd>(op);
    p.i64(5);
    p.array_header(2);
    p.i64(v.dependent);
    p.i64(v.dependency);
  } else {
    auto& v = std::get<DeltaOpEdgeRemove>(op);
    p.i64(6);
    p.array_header(2);
    p.i64(v.dependent);
    p.i64(v.dependency);
  }
}
inline DeltaOp unpack_delta_op_positional(MsgUnpacker& u) {
  uint32_t n = u.read_array_header();
  if (n < 2) throw std::runtime_error("codec: DeltaOp positional arity < 2");
  int64_t op_kind = u.read_i64();
  uint32_t m = u.read_array_header();
  switch (op_kind) {
    case 0: {
      if (m < 2) throw std::runtime_error("codec: DeltaOpCellSet fields < 2");
      DeltaOpCellSet v;
      v.node = u.read_i64();
      v.payload = unpack_ipc_value_positional(u);
      for (uint32_t i = 2; i < m; ++i) u.skip();
      return v;
    }
    case 1: {
      if (m < 2) throw std::runtime_error("codec: DeltaOpSlotValue fields < 2");
      DeltaOpSlotValue v;
      v.node = u.read_i64();
      v.payload = unpack_ipc_value_positional(u);
      for (uint32_t i = 2; i < m; ++i) u.skip();
      return v;
    }
    case 2: {
      if (m < 1) throw std::runtime_error("codec: DeltaOpInvalidate fields < 1");
      DeltaOpInvalidate v;
      v.node = u.read_i64();
      for (uint32_t i = 1; i < m; ++i) u.skip();
      return v;
    }
    case 3: {
      if (m < 4) throw std::runtime_error("codec: DeltaOpNodeAdd fields < 4");
      DeltaOpNodeAdd v;
      v.node = u.read_i64();
      v.type_tag = u.read_str();
      v.state = unpack_node_state_positional(u);
      v.key = unpack_optional_node_key_positional(u);
      for (uint32_t i = 4; i < m; ++i) u.skip();
      return v;
    }
    case 4: {
      if (m < 1) throw std::runtime_error("codec: DeltaOpNodeRemove fields < 1");
      DeltaOpNodeRemove v;
      v.node = u.read_i64();
      for (uint32_t i = 1; i < m; ++i) u.skip();
      return v;
    }
    case 5: {
      if (m < 2) throw std::runtime_error("codec: DeltaOpEdgeAdd fields < 2");
      DeltaOpEdgeAdd v;
      v.dependent = u.read_i64();
      v.dependency = u.read_i64();
      for (uint32_t i = 2; i < m; ++i) u.skip();
      return v;
    }
    case 6: {
      if (m < 2) throw std::runtime_error("codec: DeltaOpEdgeRemove fields < 2");
      DeltaOpEdgeRemove v;
      v.dependent = u.read_i64();
      v.dependency = u.read_i64();
      for (uint32_t i = 2; i < m; ++i) u.skip();
      return v;
    }
    default:
      throw std::runtime_error("codec: unknown DeltaOp kind (positional)");
  }
}

inline void pack_delta_positional(MsgPacker& p, const Delta& d) {
  p.reserve_hint(16 + d.ops.size() * 32);
  p.array_header(4);
  p.i64(kDeltaPosSchema);
  p.i64(d.base_epoch);
  p.i64(d.epoch);
  p.array_header(static_cast<uint32_t>(d.ops.size()));
  for (auto& op : d.ops) pack_delta_op_positional(p, op);
}
inline Delta unpack_delta_positional(MsgUnpacker& u) {
  uint32_t n = u.read_array_header();
  if (n < 4) throw std::runtime_error("codec: Delta positional arity < 4");
  int64_t schema = u.read_i64();
  if (schema != kDeltaPosSchema)
    throw std::runtime_error("codec: Delta positional schema mismatch");
  Delta d;
  d.base_epoch = u.read_i64();
  d.epoch = u.read_i64();
  uint32_t m = u.read_array_header();
  d.ops.reserve(m);
  for (uint32_t i = 0; i < m; ++i)
    d.ops.push_back(unpack_delta_op_positional(u));
  for (uint32_t i = 4; i < n; ++i) u.skip();
  return d;
}

// ── ResyncRequest / OutboxAck ──
inline void pack_resync_request_positional(MsgPacker& p, const ResyncRequest& r) {
  p.array_header(2);
  p.i64(kResyncRequestPosSchema);
  p.i64(r.from_epoch);
}
inline ResyncRequest unpack_resync_request_positional(MsgUnpacker& u) {
  uint32_t n = u.read_array_header();
  if (n < 2) throw std::runtime_error("codec: ResyncRequest positional arity < 2");
  int64_t schema = u.read_i64();
  if (schema != kResyncRequestPosSchema)
    throw std::runtime_error("codec: ResyncRequest positional schema mismatch");
  ResyncRequest r{0};
  r.from_epoch = u.read_i64();
  for (uint32_t i = 2; i < n; ++i) u.skip();
  return r;
}

inline void pack_outbox_ack_positional(MsgPacker& p, const OutboxAck& a) {
  p.array_header(2);
  p.i64(kOutboxAckPosSchema);
  p.i64(a.through_epoch);
}
inline OutboxAck unpack_outbox_ack_positional(MsgUnpacker& u) {
  uint32_t n = u.read_array_header();
  if (n < 2) throw std::runtime_error("codec: OutboxAck positional arity < 2");
  int64_t schema = u.read_i64();
  if (schema != kOutboxAckPosSchema)
    throw std::runtime_error("codec: OutboxAck positional schema mismatch");
  OutboxAck a{0};
  a.through_epoch = u.read_i64();
  for (uint32_t i = 2; i < n; ++i) u.skip();
  return a;
}

// ── Top-level IpcMessage positional envelope ──
inline void pack_ipc_message_positional(MsgPacker& p, const IpcMessage& m) {
  p.array_header(3);
  p.i64(kPositionalCodecVersion);
  if (std::holds_alternative<IpcMessageSnapshot>(m)) {
    p.i64(0);
    pack_snapshot_positional(p, std::get<IpcMessageSnapshot>(m).value);
  } else if (std::holds_alternative<IpcMessageDelta>(m)) {
    p.i64(1);
    pack_delta_positional(p, std::get<IpcMessageDelta>(m).value);
  } else if (std::holds_alternative<IpcMessageCrdtSync>(m)) {
    p.i64(2);
    pack_crdt_sync_positional(p, std::get<IpcMessageCrdtSync>(m).value);
  } else if (std::holds_alternative<IpcMessageResyncRequest>(m)) {
    p.i64(3);
    pack_resync_request_positional(p, std::get<IpcMessageResyncRequest>(m).value);
  } else {
    p.i64(4);
    pack_outbox_ack_positional(p, std::get<IpcMessageOutboxAck>(m).value);
  }
}
inline IpcMessage unpack_ipc_message_positional(MsgUnpacker& u) {
  uint32_t n = u.read_array_header();
  if (n < 3)
    throw std::runtime_error("codec: IpcMessage positional arity < 3");
  int64_t codec_version = u.read_i64();
  if (codec_version != kPositionalCodecVersion)
    throw std::runtime_error(
        "codec: positional codec version mismatch (incompatible envelope)");
  int64_t type_disc = u.read_i64();
  IpcMessage m;
  switch (type_disc) {
    case 0:
      m = IpcMessageSnapshot{unpack_snapshot_positional(u)};
      break;
    case 1:
      m = IpcMessageDelta{unpack_delta_positional(u)};
      break;
    case 2:
      m = IpcMessageCrdtSync{unpack_crdt_sync_positional(u)};
      break;
    case 3:
      m = IpcMessageResyncRequest{unpack_resync_request_positional(u)};
      break;
    case 4:
      m = IpcMessageOutboxAck{unpack_outbox_ack_positional(u)};
      break;
    default:
      throw std::runtime_error("codec: IpcMessage positional unknown type");
  }
  for (uint32_t i = 3; i < n; ++i) u.skip();
  return m;
}

// ── Public API ───────────────────────────────────────────────────────────────

// Default encoder: legacy string-keyed msgpack maps (backward compatible).
inline std::vector<uint8_t> encode(const IpcMessage& m) {
  MsgPacker p;
  pack_ipc_message(p, m);
  return std::move(p).take();
}

// Positional encoder: schema-versioned arrays (#lzcpppositionalcodec).
// ~2–3× smaller on the wire than `encode()` for typical Snapshot/CrdtSync
// frames because per-field string keys are dropped.
inline std::vector<uint8_t> encode_positional(const IpcMessage& m) {
  MsgPacker p;
  pack_ipc_message_positional(p, m);
  return std::move(p).take();
}

// Format-tolerant decoder. Peeks the top-level msgpack byte: a MAP routes to
// the legacy string-keyed path; an ARRAY routes to the positional path.
// Existing callers that produce/consume string-keyed frames are unaffected;
// positional-encoded frames interoperate without code changes.
inline IpcMessage decode(const uint8_t* data, size_t len) {
  MsgUnpacker u(data, len);
  if (u.peek_kind() == MsgUnpacker::Kind::Array) {
    return unpack_ipc_message_positional(u);
  }
  return unpack_ipc_message(u);
}
inline IpcMessage decode(const std::vector<uint8_t>& b) {
  return decode(b.data(), b.size());
}

}  // namespace lazily

#endif  // LAZILY_CODEC_HPP
