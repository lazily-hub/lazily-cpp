#ifndef LAZILY_CODEC_HPP
#define LAZILY_CODEC_HPP

#include <lazily/hlc.hpp>
#include <lazily/ipc.hpp>
#include <lazily/msgpack.hpp>

#include <cstdint>
#include <optional>
#include <string>
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
  p.map_header(5);
  p.str("offset");     p.i64(r.offset);
  p.str("len");        p.i64(r.len);
  p.str("generation"); p.i64(r.generation);
  p.str("epoch");      p.i64(r.epoch);
  p.str("checksum");   p.i64(r.checksum);
}
inline ShmBlobRef unpack_shm_blob_ref(MsgUnpacker& u) {
  ShmBlobRef r{};
  uint32_t n = u.read_map_header();
  for (uint32_t i = 0; i < n; ++i) {
    std::string k = u.read_str();
    if (k == "offset") r.offset = u.read_i64();
    else if (k == "len") r.len = u.read_i64();
    else if (k == "generation") r.generation = u.read_i64();
    else if (k == "epoch") r.epoch = u.read_i64();
    else if (k == "checksum") r.checksum = u.read_i64();
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
    std::string k = u.read_str();
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
  std::string path = u.read_str();
  return NodeKey::create(path);  // validates; nullopt if invalid
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
    std::string k = u.read_str();
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
    std::string k = u.read_str();
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
    std::string k = u.read_str();
    if (k == "epoch") s.epoch = u.read_i64();
    else if (k == "nodes") {
      uint32_t m = u.read_array_header();
      s.nodes.reserve(m);
      for (uint32_t j = 0; j < m; ++j) {
        NodeSnapshot nd;
        uint32_t fn = u.read_map_header();
        for (uint32_t f = 0; f < fn; ++f) {
          std::string fk = u.read_str();
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
          std::string fk = u.read_str();
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
    std::string k = u.read_str();
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
          std::string fk = u.read_str();
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
    std::string k = u.read_str();
    if (k == "frontier") {
      uint32_t m = u.read_array_header();
      c.frontier.reserve(m);
      for (uint32_t j = 0; j < m; ++j) {
        StampFrontierEntry e{};
        uint32_t fn = u.read_map_header();
        for (uint32_t f = 0; f < fn; ++f) {
          std::string fk = u.read_str();
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
          std::string fk = u.read_str();
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

// ── Top-level IpcMessage ─────────────────────────────────────────────────────

inline void pack_ipc_message(MsgPacker& p, const IpcMessage& m) {
  // discriminator "type" first.
  if (std::holds_alternative<IpcMessageSnapshot>(m)) {
    p.map_header(2); p.str("type"); p.i64(0);
    p.str("value"); pack_snapshot(p, std::get<IpcMessageSnapshot>(m).value);
  } else if (std::holds_alternative<IpcMessageDelta>(m)) {
    p.map_header(2); p.str("type"); p.i64(1);
    p.str("value"); pack_delta(p, std::get<IpcMessageDelta>(m).value);
  } else {
    p.map_header(2); p.str("type"); p.i64(2);
    p.str("value"); pack_crdt_sync(p, std::get<IpcMessageCrdtSync>(m).value);
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
    std::string k = u.read_str();
    if (k == "type") {
      type = static_cast<int>(u.read_i64());
    } else if (k == "value") {
      switch (type) {
        case 0: return IpcMessageSnapshot{unpack_snapshot(u)};
        case 1: return IpcMessageDelta{unpack_delta(u)};
        case 2: return IpcMessageCrdtSync{unpack_crdt_sync(u)};
        default:
          throw std::runtime_error("codec: value before type (non-canonical) or unknown type");
      }
    } else {
      u.skip();
    }
  }
  throw std::runtime_error("codec: IpcMessage missing value");
}

// ── Public API ───────────────────────────────────────────────────────────────

inline std::vector<uint8_t> encode(const IpcMessage& m) {
  MsgPacker p;
  pack_ipc_message(p, m);
  return std::move(p).take();
}

inline IpcMessage decode(const uint8_t* data, size_t len) {
  MsgUnpacker u(data, len);
  return unpack_ipc_message(u);
}
inline IpcMessage decode(const std::vector<uint8_t>& b) {
  return decode(b.data(), b.size());
}

}  // namespace lazily

#endif  // LAZILY_CODEC_HPP
