#ifndef LAZILY_IPC_HPP
#define LAZILY_IPC_HPP

#include <lazily/hlc.hpp>
#include <lazily/types.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace lazily {

// -- NodeKey: validated path for keyed-collection entries --

inline constexpr size_t kNodeKeyMaxLen = 1024;
inline constexpr size_t kNodeKeyMaxSegments = 32;

class NodeKey {
public:
  static std::optional<NodeKey> create(std::string_view path) {
    if (path.empty()) return std::nullopt;
    if (path.size() > kNodeKeyMaxLen) return std::nullopt;
    size_t segments = 1;
    for (size_t i = 0; i < path.size(); ++i) {
      if (path[i] == '/') {
        if (i == 0 || i == path.size() - 1 || path[i + 1] == '/') return std::nullopt;
        segments++;
        if (segments > kNodeKeyMaxSegments) return std::nullopt;
      }
    }
    return NodeKey(std::string(path));
  }
  const std::string& path() const { return path_; }
  std::string to_wire() const { return path_; }
  std::vector<std::string> segments() const {
    std::vector<std::string> result;
    size_t start = 0;
    while (start < path_.size()) {
      size_t end = path_.find('/', start);
      if (end == std::string::npos) end = path_.size();
      result.push_back(path_.substr(start, end - start));
      start = end + 1;
    }
    return result;
  }

private:
  explicit NodeKey(std::string path) : path_(std::move(path)) {}
  std::string path_;
};

// -- BlobBackendKind: which pluggable backend holds a blob (zero-copy transport) --
//
// Spec: lazily-spec/docs/zero-copy-transport.md. The descriptor (ShmBlobRef)
// carries this discriminator so a receiver routes resolution to the right
// backend. The field is OPTIONAL and its ABSENCE means `Shm` (a descriptor
// minted before the field existed resolves as POSIX shared memory); a PRESENT
// token outside the enum is refused — see `blob_backend_kind_from_str`
// (`#lzblobbackendstrict`).

enum class BlobBackendKind : uint8_t { Shm, Arrow, InProcess };

// INTENTIONALLY TOTAL — the trailing `return "shm"` covers a `BlobBackendKind`
// outside the three named values (only reachable by casting an integer into the
// enum). Keeping it total means an encoder never emits a frame with an empty
// backend token, which no decoder in any binding would accept.
//
// This does NOT contradict the decoder's strictness below. The encoder half of
// `#lzblobbackendstrict` is an OMISSION rule, not a token rule: a conforming
// encoder omits `backend` entirely when the value is `Shm`, so a pre-field
// descriptor round-trips byte-identically. That rule lives at the call site
// (`json_from_shm_blob_ref` in json_codec.hpp writes the field only when the
// backend is not `Shm`, and msgpack_codec.hpp serializes the same tree, so both
// spec wires inherit it). This function is only asked for a token once the
// caller has already decided to write one.
//
// Pinned by `blob_backend_strictness_is_pinned` in tests/test_transport.cpp and
// by the `reencoded_backend_field_present` expectations in
// tests/test_blob_backend_discriminator_conformance.cpp.
inline const char* blob_backend_kind_str(BlobBackendKind k) {
  switch (k) {
  case BlobBackendKind::Shm:
    return "shm";
  case BlobBackendKind::Arrow:
    return "arrow";
  case BlobBackendKind::InProcess:
    return "in_process";
  }
  return "shm";
}

// STRICT — a PRESENT `backend` token outside the enum is REFUSED, naming the
// offending token (`#lzblobbackendstrict`). ABSENCE stays lenient and is handled
// by the callers, which never reach this function when the field is missing:
// `ShmBlobRef::backend` default-initialises to `Shm`.
//
// That asymmetry is the whole clause. `backend` is optional and a conforming
// encoder OMITS it when the value is `Shm`, so absence is the
// forward-compatibility channel — it carries every descriptor minted before the
// field existed and MUST decode as `Shm`. A present token is a different fact:
// the producer named a backend, and there is no reading of an unrecognised name
// that is both safe and lenient.
//
// This function used to normalise an unknown token to `Shm`, on the argument
// that `ShmBackend::read_view` / `BlobArena::read_view` verify generation,
// epoch, length and checksum before returning bytes, so a foreign descriptor
// would yield an empty view rather than the wrong bytes. That argument inverts
// the `resolve_wrong_backend` theorem in
// lazily-spec/docs/zero-copy-transport.md: a descriptor of one kind never
// resolves against a different backend's table BECAUSE receivers route by kind.
// Reading an unknown kind as `Shm` *is* routing a non-shm descriptor into the
// shm table, and the verification then only *usually* catches it. "Usually" is
// the defect — the guarantee is structural, discharged by routing, and
// normalising downgrades it to a probabilistic one discharged by a 64-bit
// checksum against a backend this build genuinely resolves, so a collision
// returns BYTES. A refusal is a visible protocol error the peer recovers from
// by resync.
//
// Nor is an unknown token a newer peer: a backend joins the family by ADDING an
// enum value, which is a spec change carrying a fixture. An unrecognised token
// on the wire is a corrupt or non-conforming producer.
//
// `std::runtime_error`, not `std::invalid_argument` and not `assert`.
// `std::invalid_argument` derives from `std::logic_error`, which is exactly the
// hierarchy mistake `#lzspecdecoderbound` pinned in this suite: callers guard a
// decode with `catch (const std::runtime_error&)`, and a `logic_error` escapes
// it. `assert` is one build-flag edit from vanishing, and this is a wire
// refusal, not an internal invariant.
//
// Cost: this is a DECODE-BOUNDARY function. Its only callers are the three
// codecs' ShmBlobRef readers (json_codec.hpp, and codec.hpp's map + positional
// private framings), each called once per descriptor arriving on the wire.
// Nothing on a resolve or per-node reactive path calls it — `BlobRouter` and
// the backends switch on the already-decoded enum — so the refusal costs no
// per-node work in the reactive core.
//
// Pinned by `blob_backend_strictness_is_pinned` in tests/test_transport.cpp and
// replayed from ../lazily-spec/conformance/codec/blob_backend_discriminator.json
// by tests/test_blob_backend_discriminator_conformance.cpp.
inline BlobBackendKind blob_backend_kind_from_str(std::string_view s) {
  if (s == "shm") return BlobBackendKind::Shm;
  if (s == "arrow") return BlobBackendKind::Arrow;
  if (s == "in_process") return BlobBackendKind::InProcess;
  throw std::runtime_error("codec: unknown blob backend '" + std::string(s) + "'");
}

// -- ShmBlobRef: descriptor into a blob backend --

struct ShmBlobRef {
  int64_t offset;
  int64_t len;
  int64_t generation;
  int64_t epoch;
  int64_t checksum;
  BlobBackendKind backend = BlobBackendKind::Shm; // optional; defaults to Shm

  static bool validate(const ShmBlobRef& ref, std::optional<int64_t> max_len = std::nullopt) {
    if (ref.offset < 0 || ref.len < 0 || ref.generation < 0 || ref.epoch < 0 || ref.checksum < 0)
      return false;
    if (max_len && ref.len > *max_len) return false;
    return true;
  }
};

// -- NodeState: body of NodeSnapshot/NodeAdd --

struct NodeStatePayload {
  std::vector<uint8_t> bytes;
};
struct NodeStateSharedBlob {
  ShmBlobRef blob;
};
struct NodeStateOpaque {};

using NodeState = std::variant<NodeStatePayload, NodeStateSharedBlob, NodeStateOpaque>;

// -- IpcValue: cell payload --

struct IpcValueInline {
  std::vector<uint8_t> bytes;
};
struct IpcValueSharedBlob {
  ShmBlobRef blob;
};

using IpcValue = std::variant<IpcValueInline, IpcValueSharedBlob>;

inline bool ipc_value_equal(const IpcValue& a, const IpcValue& b) {
  if (a.index() != b.index()) return false;
  if (std::holds_alternative<IpcValueInline>(a)) {
    return std::get<IpcValueInline>(a).bytes == std::get<IpcValueInline>(b).bytes;
  }
  const auto& sa = std::get<IpcValueSharedBlob>(a).blob;
  const auto& sb = std::get<IpcValueSharedBlob>(b).blob;
  return sa.offset == sb.offset && sa.len == sb.len && sa.generation == sb.generation &&
         sa.epoch == sb.epoch && sa.checksum == sb.checksum;
}

// -- Snapshot types --

struct NodeSnapshot {
  NodeId node;
  std::string type_tag;
  NodeState state;
  std::optional<NodeKey> key;
};

struct EdgeSnapshot {
  NodeId dependent;
  NodeId dependency;
};

struct Snapshot {
  Epoch epoch;
  std::vector<NodeSnapshot> nodes;
  std::vector<EdgeSnapshot> edges;
  std::vector<NodeId> roots;
};

// -- DeltaOp variants --

struct DeltaOpCellSet {
  NodeId node;
  IpcValue payload;
};
struct DeltaOpSlotValue {
  NodeId node;
  IpcValue payload;
};
struct DeltaOpInvalidate {
  NodeId node;
};
struct DeltaOpNodeAdd {
  NodeId node;
  std::string type_tag;
  NodeState state;
  std::optional<NodeKey> key;
};
struct DeltaOpNodeRemove {
  NodeId node;
};
struct DeltaOpEdgeAdd {
  NodeId dependent;
  NodeId dependency;
};
struct DeltaOpEdgeRemove {
  NodeId dependent;
  NodeId dependency;
};

using DeltaOp = std::variant<DeltaOpCellSet, DeltaOpSlotValue, DeltaOpInvalidate, DeltaOpNodeAdd,
                             DeltaOpNodeRemove, DeltaOpEdgeAdd, DeltaOpEdgeRemove>;

// -- Delta apply status --

struct DeltaApplyStatusApply {
  Epoch new_epoch;
};
struct DeltaApplyStatusResync {
  Epoch last_epoch;
  Epoch base_epoch;
  Epoch epoch;
};

using DeltaApplyStatus = std::variant<DeltaApplyStatusApply, DeltaApplyStatusResync>;

// -- Delta --

struct Delta {
  Epoch base_epoch;
  Epoch epoch;
  std::vector<DeltaOp> ops;

  bool is_next_after(Epoch last_epoch) const {
    return base_epoch == last_epoch && epoch == base_epoch + 1;
  }

  DeltaApplyStatus apply_status(Epoch last_epoch) const {
    if (is_next_after(last_epoch)) return DeltaApplyStatusApply{epoch};
    return DeltaApplyStatusResync{last_epoch, base_epoch, epoch};
  }
};

inline Delta delta_next(Epoch base_epoch, std::vector<DeltaOp> ops) {
  return {base_epoch, base_epoch + 1, std::move(ops)};
}

// -- CRDT sync types --

struct StampFrontierEntry {
  PeerId peer;
  WireStamp stamp;
};

struct CrdtOp {
  NodeId node;
  std::optional<NodeKey> key;
  WireStamp stamp;
  IpcValue state;
};

struct CrdtSync {
  std::vector<StampFrontierEntry> frontier;
  std::vector<CrdtOp> ops;
};

// -- Reliable-sync control frames (`#lzsync`) --
//
// ResyncRequest and OutboxAck are the reverse (receiver -> sender) direction of
// the same bidirectional IpcMessage plane as Snapshot/Delta/CrdtSync — two new
// externally-tagged variants (FFI message kinds 4/5), NOT a side channel.
// Spec: lazily-spec/protocol.md § Reliable Sync, schemas/reliable-sync.json.

// Emitted by a ResyncCoordinator that detected a gap. Requests a fresh Snapshot
// covering `from_epoch` (the receiver's current last_epoch).
struct ResyncRequest {
  Epoch from_epoch;
};

// Emitted by a receiver to prove receipt through `through_epoch`. Advances the
// sender's DurableOutbox retention cursor; on reconnect it doubles as the resume
// cursor. A retention/cursor signal, NOT a domain delivery authority.
struct OutboxAck {
  Epoch through_epoch;
};

// -- IPC message envelope --

struct IpcMessageSnapshot {
  Snapshot value;
};
struct IpcMessageDelta {
  Delta value;
};
struct IpcMessageCrdtSync {
  CrdtSync value;
};
struct IpcMessageResyncRequest {
  ResyncRequest value;
};
struct IpcMessageOutboxAck {
  OutboxAck value;
};

using IpcMessage = std::variant<IpcMessageSnapshot, IpcMessageDelta, IpcMessageCrdtSync,
                                IpcMessageResyncRequest, IpcMessageOutboxAck>;

inline IpcMessage ipc_resync_request(Epoch from_epoch) {
  return IpcMessageResyncRequest{ResyncRequest{from_epoch}};
}
inline IpcMessage ipc_outbox_ack(Epoch through_epoch) {
  return IpcMessageOutboxAck{OutboxAck{through_epoch}};
}

// -- Wire-value equality --
//
// Structural equality over the whole IpcMessage tree (`#lzcppjsoncodec`). The
// codec conformance obligation is `decode(encode(decode(wire))) == decode(wire)`
// on the DECODED VALUES: comparing encoded bytes instead would let a codec that
// consistently drops a field pass, because the dropped field is absent from
// both sides. Only value equality can see the loss, since the left side came
// from the codec and the right side came from the fixture.
//
// Declared as members so `std::variant`'s own `operator==` picks them up for
// NodeState / IpcValue / DeltaOp / IpcMessage, and `std::optional<NodeKey>`
// compares through NodeKey.
//
// `ipc_value_equal` above predates this and deliberately ignores `backend`
// (it answers "same bytes?" for merge paths); wire equality does not, because a
// descriptor that resolves against a different backend is a different frame.

inline bool operator==(const NodeKey& a, const NodeKey& b) { return a.path() == b.path(); }
inline bool operator!=(const NodeKey& a, const NodeKey& b) { return !(a == b); }

inline bool operator==(const ShmBlobRef& a, const ShmBlobRef& b) {
  return a.offset == b.offset && a.len == b.len && a.generation == b.generation &&
         a.epoch == b.epoch && a.checksum == b.checksum && a.backend == b.backend;
}
inline bool operator!=(const ShmBlobRef& a, const ShmBlobRef& b) { return !(a == b); }

inline bool operator==(const NodeStatePayload& a, const NodeStatePayload& b) {
  return a.bytes == b.bytes;
}
inline bool operator==(const NodeStateSharedBlob& a, const NodeStateSharedBlob& b) {
  return a.blob == b.blob;
}
inline bool operator==(const NodeStateOpaque&, const NodeStateOpaque&) { return true; }

inline bool operator==(const IpcValueInline& a, const IpcValueInline& b) {
  return a.bytes == b.bytes;
}
inline bool operator==(const IpcValueSharedBlob& a, const IpcValueSharedBlob& b) {
  return a.blob == b.blob;
}

inline bool operator==(const NodeSnapshot& a, const NodeSnapshot& b) {
  return a.node == b.node && a.type_tag == b.type_tag && a.state == b.state && a.key == b.key;
}
inline bool operator==(const EdgeSnapshot& a, const EdgeSnapshot& b) {
  return a.dependent == b.dependent && a.dependency == b.dependency;
}
inline bool operator==(const Snapshot& a, const Snapshot& b) {
  return a.epoch == b.epoch && a.nodes == b.nodes && a.edges == b.edges && a.roots == b.roots;
}

inline bool operator==(const DeltaOpCellSet& a, const DeltaOpCellSet& b) {
  return a.node == b.node && a.payload == b.payload;
}
inline bool operator==(const DeltaOpSlotValue& a, const DeltaOpSlotValue& b) {
  return a.node == b.node && a.payload == b.payload;
}
inline bool operator==(const DeltaOpInvalidate& a, const DeltaOpInvalidate& b) {
  return a.node == b.node;
}
inline bool operator==(const DeltaOpNodeAdd& a, const DeltaOpNodeAdd& b) {
  return a.node == b.node && a.type_tag == b.type_tag && a.state == b.state && a.key == b.key;
}
inline bool operator==(const DeltaOpNodeRemove& a, const DeltaOpNodeRemove& b) {
  return a.node == b.node;
}
inline bool operator==(const DeltaOpEdgeAdd& a, const DeltaOpEdgeAdd& b) {
  return a.dependent == b.dependent && a.dependency == b.dependency;
}
inline bool operator==(const DeltaOpEdgeRemove& a, const DeltaOpEdgeRemove& b) {
  return a.dependent == b.dependent && a.dependency == b.dependency;
}
inline bool operator==(const Delta& a, const Delta& b) {
  return a.base_epoch == b.base_epoch && a.epoch == b.epoch && a.ops == b.ops;
}

inline bool operator==(const StampFrontierEntry& a, const StampFrontierEntry& b) {
  return a.peer == b.peer && a.stamp == b.stamp;
}
inline bool operator==(const CrdtOp& a, const CrdtOp& b) {
  return a.node == b.node && a.key == b.key && a.stamp == b.stamp && a.state == b.state;
}
inline bool operator==(const CrdtSync& a, const CrdtSync& b) {
  return a.frontier == b.frontier && a.ops == b.ops;
}

inline bool operator==(const ResyncRequest& a, const ResyncRequest& b) {
  return a.from_epoch == b.from_epoch;
}
inline bool operator==(const OutboxAck& a, const OutboxAck& b) {
  return a.through_epoch == b.through_epoch;
}

inline bool operator==(const IpcMessageSnapshot& a, const IpcMessageSnapshot& b) {
  return a.value == b.value;
}
inline bool operator==(const IpcMessageDelta& a, const IpcMessageDelta& b) {
  return a.value == b.value;
}
inline bool operator==(const IpcMessageCrdtSync& a, const IpcMessageCrdtSync& b) {
  return a.value == b.value;
}
inline bool operator==(const IpcMessageResyncRequest& a, const IpcMessageResyncRequest& b) {
  return a.value == b.value;
}
inline bool operator==(const IpcMessageOutboxAck& a, const IpcMessageOutboxAck& b) {
  return a.value == b.value;
}

// -- Permission boundary --

enum class OpKind { Read, Write, TriggerEffect };

struct RemoteOp {
  OpKind kind;
  NodeId node;
};

inline RemoteOp read_op(NodeId node) { return {OpKind::Read, node}; }
inline RemoteOp write_op(NodeId node) { return {OpKind::Write, node}; }
inline RemoteOp trigger_effect_op(NodeId node) { return {OpKind::TriggerEffect, node}; }

struct PermissionDenied {
  PeerId peer;
  RemoteOp op;
};

class PeerPermissions {
public:
  bool allow(PeerId peer, const RemoteOp& op) {
    return peers_[peer][op.kind].insert(op.node).second;
  }

  bool revoke(PeerId peer, const RemoteOp& op) {
    auto it = peers_.find(peer);
    if (it == peers_.end()) return false;
    auto kit = it->second.find(op.kind);
    if (kit == it->second.end()) return false;
    return kit->second.erase(op.node) > 0;
  }

  bool revoke_peer(PeerId peer) { return peers_.erase(peer) > 0; }

  bool is_allowed(PeerId peer, const RemoteOp& op) const {
    auto it = peers_.find(peer);
    if (it == peers_.end()) return false;
    auto kit = it->second.find(op.kind);
    if (kit == it->second.end()) return false;
    return kit->second.count(op.node) > 0;
  }

  bool can_read(PeerId peer, NodeId node) const { return is_allowed(peer, read_op(node)); }

  int peer_count() const { return static_cast<int>(peers_.size()); }

  std::vector<NodeId> filter_readable(PeerId peer, const std::vector<NodeId>& nodes) const {
    std::vector<NodeId> result;
    for (auto n : nodes) {
      if (can_read(peer, n)) result.push_back(n);
    }
    return result;
  }

private:
  std::unordered_map<PeerId, std::unordered_map<OpKind, std::unordered_set<NodeId>>> peers_;
};

// -- ShmBlobArena --

class ShmBlobArena {
public:
  explicit ShmBlobArena(Epoch epoch) : epoch_(epoch), generation_(0) {}

  Epoch epoch() const { return epoch_; }
  size_t length() const { return entries_.size(); }
  bool is_empty() const { return entries_.empty(); }

  ShmBlobRef write(const std::vector<uint8_t>& bytes) {
    generation_++;
    size_t offset = entries_.size();
    auto entry = std::make_shared<Entry>();
    entry->generation = generation_;
    entry->epoch = epoch_;
    entry->payload = bytes;
    entry->ref_count = 1;
    // Payload is immutable after write, so the checksum is computed once and
    // cached — read() validates against the cached value instead of recomputing
    // a full FNV-1a hash on every read (the bulk of large-blob read cost).
    entry->checksum_cached = Entry::compute_checksum(entry->payload);
    entries_.push_back(entry);
    return entry->to_ref(static_cast<int64_t>(offset));
  }

  std::vector<uint8_t> read(const ShmBlobRef& ref) const {
    const std::vector<uint8_t>* v = read_view(ref);
    return v ? *v : std::vector<uint8_t>{};
  }

  // Zero-copy read: returns a pointer to the (immutable) cached payload, or
  // nullptr if the descriptor is invalid/stale. No payload copy, no checksum
  // recompute — the hot path for large blobs transferred over the IPC plane.
  // The pointer is valid as long as the arena keeps the entry alive (entries are
  // shared_ptr-backed and never mutated in place).
  const std::vector<uint8_t>* read_view(const ShmBlobRef& ref) const {
    if (ref.offset < 0 || static_cast<size_t>(ref.offset) >= entries_.size()) return nullptr;
    auto& entry = entries_[ref.offset];
    if (!entry) return nullptr;
    if (entry->generation != ref.generation) return nullptr;
    if (entry->epoch != ref.epoch) return nullptr;
    if (static_cast<int64_t>(entry->payload.size()) != ref.len) return nullptr;
    if (entry->checksum_cached != ref.checksum) return nullptr;
    return &entry->payload;
  }

  void advance_epoch() {
    epoch_++;
    for (auto& e : entries_) {
      if (e) {
        e->epoch = epoch_;
      }
    }
  }

private:
  struct Entry {
    int64_t generation;
    Epoch epoch;
    std::vector<uint8_t> payload;
    int ref_count;
    int64_t checksum_cached = 0;

    static int64_t compute_checksum(const std::vector<uint8_t>& payload) {
      uint64_t hash = 0xcbf29ce484222325ULL;
      for (auto b : payload) {
        hash ^= b;
        hash *= 0x100000001b3ULL;
      }
      return static_cast<int64_t>(hash);
    }

    ShmBlobRef to_ref(int64_t offset) const {
      return {offset, static_cast<int64_t>(payload.size()), generation, epoch, checksum_cached};
    }
  };

  Epoch epoch_;
  int64_t generation_;
  std::vector<std::shared_ptr<Entry>> entries_;
};

// -- Capability negotiation --

enum class FfiCapability { Host, None };

struct BindingCapabilities {
  std::string binding = kBindingName;
  FfiCapability ffi = FfiCapability::Host;
  bool reactive_core = true;
  bool collections = true;
  bool state_machine = true;
  bool state_charts = true;
  bool ipc = true;
  bool crdt = true;
  bool permissions = true;
  bool capability_negotiation = true;
  bool async_ctx = true;
};

struct CapabilityCheck {
  bool ok;
  std::string field;
  std::string reason;
};

struct CapabilityHandshake {
  std::string protocol_id = kProtocolId;
  int protocol_major_version = kProtocolMajorVersion;
  // A closed `Codec`, not a free string (`#lzcppcodecdispatch`): the negotiated
  // token is what `codec_encode`/`codec_decode` dispatch on, so the codec the
  // peers agreed on is the codec that produces the bytes.
  Codec codec = kDefaultCodec;
  int64_t max_frame_size = kDefaultMaxFrameSize;
  bool fragmentation_supported = false;
  bool ordered_reliable = true;
  PeerId peer_id = 0;
  std::string session_id;
  std::vector<std::string> features;

  bool has_feature(const std::string& feature) const {
    for (auto& f : features) {
      if (f == feature) return true;
    }
    return false;
  }

  CapabilityCheck check_compatible(const CapabilityHandshake& other,
                                   const std::vector<std::string>& required_features) const {
    if (protocol_id != other.protocol_id) return {false, "protocol_id", "protocol id mismatch"};
    if (protocol_major_version != other.protocol_major_version)
      return {false, "protocol_major_version", "major version mismatch"};
    if (codec != other.codec)
      return {false, "codec",
              std::string("codec mismatch: ") + codec_token(codec) + " vs " +
                  codec_token(other.codec)};
    if (!ordered_reliable || !other.ordered_reliable)
      return {false, "ordered_reliable", "both peers must require ordered reliable"};
    for (auto& f : required_features) {
      if (!other.has_feature(f)) return {false, "features", "missing required feature: " + f};
    }
    return {true, "", ""};
  }

  bool is_compatible_with(const CapabilityHandshake& other) const {
    return check_compatible(other, {}).ok;
  }
};

inline CapabilityHandshake new_capability_handshake(PeerId peer_id, const std::string& session_id) {
  CapabilityHandshake h;
  h.peer_id = peer_id;
  h.session_id = session_id;
  return h;
}

} // namespace lazily

#endif // LAZILY_IPC_HPP
