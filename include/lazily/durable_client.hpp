#ifndef LAZILY_DURABLE_CLIENT_HPP
#define LAZILY_DURABLE_CLIENT_HPP

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lazily {

inline constexpr std::uint64_t kDurableClientProtocolVersion = 1;
enum class DurableCapabilityTier { Core, Client, DurableHost, DistributedHost, AcceleratedHost };
struct DurableTierDeclaration {
  bool core = true;
  bool client = true;
  bool durable_host = false;
  bool distributed_host = false;
  bool accelerated_host = false;
};

/// Exact durable-envelope-v1 shape. Authority and broker metadata do not belong here.
struct DurableEnvelope {
  std::uint64_t protocol_version = kDurableClientProtocolVersion;
  std::string message_id;
  std::uint64_t schema_version = 0;
  std::uint64_t codec_version = 0;
  std::vector<std::uint8_t> payload;

  bool same_content(const DurableEnvelope& other) const {
    return protocol_version == other.protocol_version && schema_version == other.schema_version &&
           codec_version == other.codec_version && payload == other.payload;
  }
};

enum class EnvelopeValidation {
  Accepted,
  UnsupportedProtocolVersion,
  InvalidMessageId,
  InvalidSchemaVersion,
  InvalidCodecVersion,
};
inline EnvelopeValidation validate_durable_envelope(const DurableEnvelope& envelope) {
  if (envelope.protocol_version != kDurableClientProtocolVersion)
    return EnvelopeValidation::UnsupportedProtocolVersion;
  if (envelope.message_id.empty()) return EnvelopeValidation::InvalidMessageId;
  if (envelope.schema_version == 0 || envelope.schema_version > UINT32_MAX)
    return EnvelopeValidation::InvalidSchemaVersion;
  if (envelope.codec_version == 0 || envelope.codec_version > UINT32_MAX)
    return EnvelopeValidation::InvalidCodecVersion;
  return EnvelopeValidation::Accepted;
}

enum class DeliveryClassification { First, Duplicate, Conflict };
class DurableDeduplicator {
public:
  DeliveryClassification classify(const DurableEnvelope& envelope) {
    auto prior = seen_.find(envelope.message_id);
    if (prior == seen_.end()) {
      seen_.emplace(envelope.message_id, envelope);
      return DeliveryClassification::First;
    }
    return prior->second.same_content(envelope) ? DeliveryClassification::Duplicate
                                                : DeliveryClassification::Conflict;
  }

private:
  std::unordered_map<std::string, DurableEnvelope> seen_;
};

class DurableObservationOrder {
public:
  void observe(const DurableEnvelope& envelope) { message_ids_.push_back(envelope.message_id); }
  const std::vector<std::string>& message_ids() const { return message_ids_; }
  static constexpr bool owner_order_inferred() { return false; }

private:
  std::vector<std::string> message_ids_;
};

/// NATS publication acceptance. This is deliberately not a durable-host receipt.
struct BrokerPubAck {
  std::string stream;
  std::uint64_t sequence = 0;
  bool duplicate = false;
};
enum class DurableHostOutcome { Committed, Duplicate, Conflict, Rejected };
struct DurableHostReceipt {
  std::uint64_t protocol_version = kDurableClientProtocolVersion;
  std::string receipt_id;
  std::string message_id;
  DurableHostOutcome outcome = DurableHostOutcome::Rejected;
  std::uint64_t owner_position = 0;
  static constexpr bool transport_ack_equivalent = false;
};

enum class ProjectionCapability { CompleteHistory, LatestStateOnly };
struct ProjectionFingerprint {
  std::string projection_id;
  std::uint64_t source_position = 0;
  std::string fingerprint;
  ProjectionCapability completeness = ProjectionCapability::CompleteHistory;
  bool equivalent_to(const ProjectionFingerprint& other) const {
    return projection_id == other.projection_id && source_position == other.source_position &&
           fingerprint == other.fingerprint && completeness == other.completeness;
  }
  static constexpr bool may_authorize_transition = false;
};
template <typename T> struct DurableProjectionUpdate {
  ProjectionFingerprint fingerprint;
  std::uint64_t projection_version = 0;
  T value;
  static constexpr bool may_authorize_transition = false;
};
enum class ProjectionApplyStatus { Applied, Buffered, Duplicate, IdentityConflict, Invalid };

class NatsDurableClientTransport {
public:
  using Handler = std::function<void(const DurableEnvelope&)>;
  virtual ~NatsDurableClientTransport() = default;
  virtual BrokerPubAck publish(const std::string& subject, const DurableEnvelope& envelope) = 0;
  virtual void subscribe(const std::string& subject, Handler handler) = 0;
};

template <typename T> class DurableProjectionClient {
public:
  ProjectionApplyStatus apply(const DurableProjectionUpdate<T>& update) {
    if (update.fingerprint.projection_id.empty() || update.fingerprint.fingerprint.empty() ||
        update.projection_version == 0 ||
        update.fingerprint.completeness != ProjectionCapability::CompleteHistory)
      return ProjectionApplyStatus::Invalid;
    auto& state = owners_[update.fingerprint.projection_id];
    auto prior = state.fingerprints.find(update.fingerprint.source_position);
    if (prior != state.fingerprints.end())
      return prior->second == update.fingerprint.fingerprint
                 ? ProjectionApplyStatus::Duplicate
                 : ProjectionApplyStatus::IdentityConflict;
    auto buffered = state.pending.find(update.fingerprint.source_position);
    if (buffered != state.pending.end())
      return buffered->second.fingerprint.fingerprint == update.fingerprint.fingerprint
                 ? ProjectionApplyStatus::Duplicate
                 : ProjectionApplyStatus::IdentityConflict;
    if (update.fingerprint.source_position > state.source_position + 1) {
      state.pending.emplace(update.fingerprint.source_position, update);
      return ProjectionApplyStatus::Buffered;
    }
    if (update.fingerprint.source_position <= state.source_position)
      return ProjectionApplyStatus::IdentityConflict;
    if (update.projection_version <= state.projection_version)
      return ProjectionApplyStatus::Invalid;
    state.source_position = update.fingerprint.source_position;
    state.projection_version = update.projection_version;
    state.fingerprints.emplace(update.fingerprint.source_position, update.fingerprint.fingerprint);
    state.applied_positions.push_back(update.fingerprint.source_position);
    state.latest = update;
    while (true) {
      auto next = state.pending.find(state.source_position + 1);
      if (next == state.pending.end()) break;
      auto contiguous = std::move(next->second);
      state.pending.erase(next);
      state.source_position = contiguous.fingerprint.source_position;
      state.projection_version = contiguous.projection_version;
      state.fingerprints.emplace(contiguous.fingerprint.source_position,
                                 contiguous.fingerprint.fingerprint);
      state.applied_positions.push_back(contiguous.fingerprint.source_position);
      state.latest = std::move(contiguous);
    }
    return ProjectionApplyStatus::Applied;
  }
  std::optional<DurableProjectionUpdate<T>> latest(const std::string& owner_id) const {
    auto it = owners_.find(owner_id);
    return it == owners_.end() ? std::nullopt : it->second.latest;
  }
  static constexpr bool may_authorize_transition() { return false; }
  std::vector<std::uint64_t> applied_positions(const std::string& projection_id) const {
    auto it = owners_.find(projection_id);
    return it == owners_.end() ? std::vector<std::uint64_t>{} : it->second.applied_positions;
  }

private:
  struct OwnerState {
    std::uint64_t source_position = 0;
    std::uint64_t projection_version = 0;
    std::unordered_map<std::uint64_t, std::string> fingerprints;
    std::unordered_map<std::uint64_t, DurableProjectionUpdate<T>> pending;
    std::vector<std::uint64_t> applied_positions;
    std::optional<DurableProjectionUpdate<T>> latest;
  };
  std::unordered_map<std::string, OwnerState> owners_;
};

template <typename Ingress, typename Projection> class DurableClient {
public:
  using Encoder = std::function<std::vector<std::uint8_t>(const Ingress&)>;
  using Decoder =
      std::function<DurableProjectionUpdate<Projection>(const std::vector<std::uint8_t>&)>;
  DurableClient(NatsDurableClientTransport& transport, Encoder encoder, Decoder decoder)
      : transport_(transport), encoder_(std::move(encoder)), decoder_(std::move(decoder)) {}

  BrokerPubAck publish(const std::string& subject, std::string message_id,
                       std::uint64_t schema_version, std::uint64_t codec_version,
                       const Ingress& value) {
    DurableEnvelope envelope{1, std::move(message_id), schema_version, codec_version,
                             encoder_(value)};
    if (validate_durable_envelope(envelope) != EnvelopeValidation::Accepted)
      throw std::invalid_argument("invalid durable envelope");
    return transport_.publish(subject, envelope);
  }
  struct ObserveResult {
    EnvelopeValidation validation;
    std::optional<DeliveryClassification> classification;
    std::optional<ProjectionApplyStatus> projection;
  };
  ObserveResult observe(const DurableEnvelope& envelope) {
    auto validation = validate_durable_envelope(envelope);
    if (validation != EnvelopeValidation::Accepted) return {validation, std::nullopt, std::nullopt};
    observation_order_.observe(envelope);
    auto classification = deduplicator_.classify(envelope);
    if (classification != DeliveryClassification::First)
      return {validation, classification, std::nullopt};
    return {validation, classification, projection_.apply(decoder_(envelope.payload))};
  }
  DurableProjectionClient<Projection>& projection() { return projection_; }
  DurableDeduplicator& deduplicator() { return deduplicator_; }
  const DurableObservationOrder& observation_order() const { return observation_order_; }
  static constexpr bool may_authorize_transition() { return false; }

private:
  NatsDurableClientTransport& transport_;
  Encoder encoder_;
  Decoder decoder_;
  DurableDeduplicator deduplicator_;
  DurableObservationOrder observation_order_;
  DurableProjectionClient<Projection> projection_;
};

} // namespace lazily
#endif
