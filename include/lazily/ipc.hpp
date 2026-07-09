#ifndef LAZILY_IPC_HPP
#define LAZILY_IPC_HPP

#include <lazily/hlc.hpp>
#include <lazily/types.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
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
  static std::optional<NodeKey> create(const std::string& path) {
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
    return NodeKey(path);
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
  explicit NodeKey(const std::string& path) : path_(path) {}
  std::string path_;
};

// -- ShmBlobRef: descriptor into shared-memory arena --

struct ShmBlobRef {
  int64_t offset;
  int64_t len;
  int64_t generation;
  int64_t epoch;
  int64_t checksum;

  static bool validate(const ShmBlobRef& ref, std::optional<int64_t> max_len = std::nullopt) {
    if (ref.offset < 0 || ref.len < 0 || ref.generation < 0 || ref.epoch < 0 || ref.checksum < 0)
      return false;
    if (max_len && ref.len > *max_len) return false;
    return true;
  }
};

// -- NodeState: body of NodeSnapshot/NodeAdd --

struct NodeStatePayload { std::vector<uint8_t> bytes; };
struct NodeStateSharedBlob { ShmBlobRef blob; };
struct NodeStateOpaque {};

using NodeState = std::variant<NodeStatePayload, NodeStateSharedBlob, NodeStateOpaque>;

// -- IpcValue: cell payload --

struct IpcValueInline { std::vector<uint8_t> bytes; };
struct IpcValueSharedBlob { ShmBlobRef blob; };

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

struct DeltaOpCellSet { NodeId node; IpcValue payload; };
struct DeltaOpSlotValue { NodeId node; IpcValue payload; };
struct DeltaOpInvalidate { NodeId node; };
struct DeltaOpNodeAdd { NodeId node; std::string type_tag; NodeState state; std::optional<NodeKey> key; };
struct DeltaOpNodeRemove { NodeId node; };
struct DeltaOpEdgeAdd { NodeId dependent; NodeId dependency; };
struct DeltaOpEdgeRemove { NodeId dependent; NodeId dependency; };

using DeltaOp = std::variant<DeltaOpCellSet, DeltaOpSlotValue, DeltaOpInvalidate,
                              DeltaOpNodeAdd, DeltaOpNodeRemove,
                              DeltaOpEdgeAdd, DeltaOpEdgeRemove>;

// -- Delta apply status --

struct DeltaApplyStatusApply { Epoch new_epoch; };
struct DeltaApplyStatusResync { Epoch last_epoch; Epoch base_epoch; Epoch epoch; };

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
    if (is_next_after(last_epoch))
      return DeltaApplyStatusApply{epoch};
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

// -- IPC message envelope --

struct IpcMessageSnapshot { Snapshot value; };
struct IpcMessageDelta { Delta value; };
struct IpcMessageCrdtSync { CrdtSync value; };

using IpcMessage = std::variant<IpcMessageSnapshot, IpcMessageDelta, IpcMessageCrdtSync>;

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

  bool can_read(PeerId peer, NodeId node) const {
    return is_allowed(peer, read_op(node));
  }

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
    if (ref.offset < 0 || static_cast<size_t>(ref.offset) >= entries_.size()) return {};
    auto& entry = entries_[ref.offset];
    if (!entry) return {};
    if (entry->generation != ref.generation) return {};
    if (entry->epoch != ref.epoch) return {};
    if (static_cast<int64_t>(entry->payload.size()) != ref.len) return {};
    if (entry->checksum_cached != ref.checksum) return {};
    return entry->payload;
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
      return {offset, static_cast<int64_t>(payload.size()), generation, epoch,
              checksum_cached};
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
  std::string codec = kDefaultCodec;
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
    if (protocol_id != other.protocol_id)
      return {false, "protocol_id", "protocol id mismatch"};
    if (protocol_major_version != other.protocol_major_version)
      return {false, "protocol_major_version", "major version mismatch"};
    if (codec != other.codec)
      return {false, "codec", "codec mismatch"};
    if (!ordered_reliable || !other.ordered_reliable)
      return {false, "ordered_reliable", "both peers must require ordered reliable"};
    for (auto& f : required_features) {
      if (!other.has_feature(f))
        return {false, "features", "missing required feature: " + f};
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

}  // namespace lazily

#endif  // LAZILY_IPC_HPP
