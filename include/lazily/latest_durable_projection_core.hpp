#ifndef LAZILY_LATEST_DURABLE_PROJECTION_CORE_HPP
#define LAZILY_LATEST_DURABLE_PROJECTION_CORE_HPP

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lazily {

/// Latest unclaimed projection for one key.
template <typename T> struct LatestDurableDesired {
  std::uint64_t epoch = 0;
  T value{};
  bool operator==(const LatestDurableDesired& other) const {
    return epoch == other.epoch && value == other.value;
  }
  bool operator!=(const LatestDurableDesired& other) const { return !(*this == other); }
};

/// One generation-fenced sink attempt.
template <typename K, typename T> struct LatestDurableEnvelope {
  std::uint64_t generation = 0;
  K key{};
  std::uint64_t epoch = 0;
  T value{};
  bool operator==(const LatestDurableEnvelope& other) const {
    return generation == other.generation && key == other.key && epoch == other.epoch &&
           value == other.value;
  }
  bool operator!=(const LatestDurableEnvelope& other) const { return !(*this == other); }
};

/// Immutable diagnostic image of one keyed projection.
template <typename K, typename T> struct LatestDurableEntry {
  K key{};
  std::optional<LatestDurableDesired<T>> desired;
  std::optional<LatestDurableEnvelope<K, T>> inflight;
  std::optional<std::uint64_t> durable_through;
  bool operator==(const LatestDurableEntry& other) const {
    return key == other.key && desired == other.desired && inflight == other.inflight &&
           durable_through == other.durable_through;
  }
  bool operator!=(const LatestDurableEntry& other) const { return !(*this == other); }
};

enum class LatestDurableUpsertKind {
  Accepted,
  Unchanged,
  AlreadyDurable,
  StaleEpoch,
  EpochConflict,
};

struct LatestDurableUpsertResult {
  LatestDurableUpsertKind kind = LatestDurableUpsertKind::Accepted;
  std::optional<std::uint64_t> current;
};

enum class LatestDurableClaimKind { Claimed, Empty, Busy, StaleGeneration };

template <typename K, typename T> struct LatestDurableClaimResult {
  LatestDurableClaimKind kind = LatestDurableClaimKind::Empty;
  std::optional<LatestDurableEnvelope<K, T>> envelope;
  std::optional<std::uint64_t> current;
};

enum class LatestDurableAckKind { Advanced, Unchanged, UnknownEpoch, StaleGeneration };

struct LatestDurableAckResult {
  LatestDurableAckKind kind = LatestDurableAckKind::UnknownEpoch;
  std::optional<std::uint64_t> durable_through;
  std::optional<std::uint64_t> current;
};

enum class LatestDurableFailureKind { Pending, Superseded, UnknownEpoch, StaleGeneration };

struct LatestDurableFailureResult {
  LatestDurableFailureKind kind = LatestDurableFailureKind::UnknownEpoch;
  std::optional<std::uint64_t> current;
};

enum class LatestDurableReconnectKind { Advanced, Unchanged, StaleGeneration };

struct LatestDurableReconnectResult {
  LatestDurableReconnectKind kind = LatestDurableReconnectKind::Unchanged;
  std::uint64_t generation = 0;
  std::size_t requeued = 0;
  std::size_t superseded = 0;
};

/// Pure keyed latest-durable projection state machine (lazily-spec v0.38.0).
///
/// A newer desired epoch replaces pending state only. The exact generation/epoch
/// token owns an in-flight value until acknowledgement, retryable failure, or a
/// reconnect fence. No graph writes or sink I/O occur here.
template <typename K, typename T, typename Hash = std::hash<K>, typename Equal = std::equal_to<K>>
class LatestDurableProjectionCore {
  struct State {
    std::optional<LatestDurableDesired<T>> desired;
    std::optional<LatestDurableEnvelope<K, T>> inflight;
    std::optional<std::uint64_t> durable_through;
  };

public:
  explicit LatestDurableProjectionCore(std::uint64_t initial_generation)
      : generation_(initial_generation) {}

  std::uint64_t generation() const { return generation_; }
  std::vector<K> known_keys() const { return keys_; }

  LatestDurableEntry<K, T> snapshot(const K& key) const {
    const auto it = entries_.find(key);
    if (it == entries_.end()) return LatestDurableEntry<K, T>{key, {}, {}, {}};
    return LatestDurableEntry<K, T>{key, it->second.desired, it->second.inflight,
                                    it->second.durable_through};
  }

  std::vector<LatestDurableEntry<K, T>> snapshots() const {
    std::vector<LatestDurableEntry<K, T>> out;
    out.reserve(keys_.size());
    for (const auto& key : keys_)
      out.push_back(snapshot(key));
    return out;
  }

  std::optional<std::uint64_t> durable_through(const K& key) const {
    const auto it = entries_.find(key);
    return it == entries_.end() ? std::optional<std::uint64_t>{} : it->second.durable_through;
  }

  LatestDurableUpsertResult upsert_desired(K key, std::uint64_t epoch, T value) {
    State& state = ensure(key);
    if (state.durable_through && epoch <= *state.durable_through)
      return {LatestDurableUpsertKind::AlreadyDurable, state.durable_through};

    std::optional<std::uint64_t> newest;
    if (state.desired) newest = state.desired->epoch;
    if (state.inflight && (!newest || state.inflight->epoch > *newest))
      newest = state.inflight->epoch;
    if (newest) {
      if (epoch < *newest) return {LatestDurableUpsertKind::StaleEpoch, newest};
      if (epoch == *newest) {
        const T& retained = state.desired && state.desired->epoch == epoch ? state.desired->value
                                                                           : state.inflight->value;
        return {retained == value ? LatestDurableUpsertKind::Unchanged
                                  : LatestDurableUpsertKind::EpochConflict,
                {}};
      }
    }

    state.desired = LatestDurableDesired<T>{epoch, std::move(value)};
    return {LatestDurableUpsertKind::Accepted, {}};
  }

  LatestDurableClaimResult<K, T> claim(const K& key, std::uint64_t generation) {
    if (generation != generation_)
      return {LatestDurableClaimKind::StaleGeneration, {}, generation_};
    const auto it = entries_.find(key);
    if (it == entries_.end()) return {LatestDurableClaimKind::Empty, {}, {}};
    State& state = it->second;
    if (state.inflight) return {LatestDurableClaimKind::Busy, {}, {}};
    if (!state.desired) return {LatestDurableClaimKind::Empty, {}, {}};
    LatestDurableEnvelope<K, T> envelope{generation, key, state.desired->epoch,
                                         std::move(state.desired->value)};
    state.desired.reset();
    state.inflight = envelope;
    return {LatestDurableClaimKind::Claimed, std::move(envelope), {}};
  }

  LatestDurableAckResult ack_applied(const K& key, std::uint64_t generation, std::uint64_t epoch) {
    if (generation != generation_) return {LatestDurableAckKind::StaleGeneration, {}, generation_};
    const auto it = entries_.find(key);
    if (it == entries_.end() || !it->second.inflight || it->second.inflight->epoch != epoch) {
      const auto durable =
          it == entries_.end() ? std::optional<std::uint64_t>{} : it->second.durable_through;
      return durable && epoch <= *durable
                 ? LatestDurableAckResult{LatestDurableAckKind::Unchanged, durable, {}}
                 : LatestDurableAckResult{LatestDurableAckKind::UnknownEpoch, {}, {}};
    }
    State& state = it->second;
    state.inflight.reset();
    const auto previous = state.durable_through;
    const std::uint64_t frontier = previous ? std::max(*previous, epoch) : epoch;
    state.durable_through = frontier;
    return {previous && epoch <= *previous ? LatestDurableAckKind::Unchanged
                                           : LatestDurableAckKind::Advanced,
            frontier,
            {}};
  }

  LatestDurableFailureResult fail_retryable(const K& key, std::uint64_t generation,
                                            std::uint64_t epoch) {
    if (generation != generation_) return {LatestDurableFailureKind::StaleGeneration, generation_};
    const auto it = entries_.find(key);
    if (it == entries_.end() || !it->second.inflight || it->second.inflight->epoch != epoch)
      return {LatestDurableFailureKind::UnknownEpoch, {}};
    State& state = it->second;
    auto failed = std::move(*state.inflight);
    state.inflight.reset();
    if (state.desired && state.desired->epoch > failed.epoch)
      return {LatestDurableFailureKind::Superseded, {}};
    state.desired = LatestDurableDesired<T>{failed.epoch, std::move(failed.value)};
    return {LatestDurableFailureKind::Pending, {}};
  }

  LatestDurableReconnectResult reconnect(std::uint64_t new_generation) {
    if (new_generation < generation_)
      return {LatestDurableReconnectKind::StaleGeneration, generation_, 0, 0};
    if (new_generation == generation_)
      return {LatestDurableReconnectKind::Unchanged, generation_, 0, 0};
    std::size_t requeued = 0;
    std::size_t superseded = 0;
    for (auto& pair : entries_) {
      State& state = pair.second;
      if (!state.inflight) continue;
      auto flight = std::move(*state.inflight);
      state.inflight.reset();
      if (state.desired && state.desired->epoch > flight.epoch)
        ++superseded;
      else {
        state.desired = LatestDurableDesired<T>{flight.epoch, std::move(flight.value)};
        ++requeued;
      }
    }
    generation_ = new_generation;
    return {LatestDurableReconnectKind::Advanced, generation_, requeued, superseded};
  }

private:
  State& ensure(const K& key) {
    const auto it = entries_.find(key);
    if (it != entries_.end()) return it->second;
    keys_.push_back(key);
    return entries_.emplace(key, State{}).first->second;
  }

  std::uint64_t generation_;
  std::unordered_map<K, State, Hash, Equal> entries_;
  std::vector<K> keys_;
};

} // namespace lazily

#endif // LAZILY_LATEST_DURABLE_PROJECTION_CORE_HPP
