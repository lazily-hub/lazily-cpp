#ifndef LAZILY_MEMBERSHIP_HPP
#define LAZILY_MEMBERSHIP_HPP

// Membership + failure detection (`#lzmemb`).
//
// C++17 port of `lazily-rs/src/membership.rs` (see also
// `lazily-spec/docs/membership.md` and the formal model
// `lazily-formal/LazilyFormal/Membership.lean`). A `MembershipCell` is a
// reactive view of the live peer set, backed by SWIM-style heartbeats + a
// **Phi-accrual** failure detector. Per-peer state is `Alive | Suspect | Dead |
// Left`; the derived `PeerSet` is the `Alive` peers.
//
// The pure compute **core** (`MembershipCore` + `PhiAccrual`) is the
// Phi-accrual math + SWIM state machine over plain state; the reactive cell
// (`MembershipCell`) projects the alive set onto a `Cell` so `PeerSet`
// invalidates only when the set changes. The peer id is generic (`P`, requires
// `<` / `==`); the distributed plane plugs in `PeerId`.
//
// The phi formula is copied bit-for-bit from the Rust reference (Akka-style
// logistic approximation of the normal CDF) so fixture state transitions cross
// the same thresholds across every binding.

#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include <lazily/context.hpp>

namespace lazily {

// Per-peer liveness state (SWIM).
enum class PeerState {
  // Heartbeats current; a valid CRDT sync target.
  Alive,
  // Phi crossed the threshold; awaiting a refuting heartbeat or the timeout.
  Suspect,
  // Suspect long enough to declare failed.
  Dead,
  // Gracefully departed.
  Left,
};

inline const char* peer_state_name(PeerState s) {
  switch (s) {
    case PeerState::Alive:
      return "Alive";
    case PeerState::Suspect:
      return "Suspect";
    case PeerState::Dead:
      return "Dead";
    case PeerState::Left:
      return "Left";
  }
  return "Alive";
}

// A diff event over the membership cell.
template <typename P>
struct PeerChangeEvent {
  enum class Kind { Joined, Left, StateChanged };

  Kind kind;
  P peer;
  // Only meaningful when `kind == StateChanged`.
  PeerState from;
  PeerState to;

  static PeerChangeEvent joined(P peer) {
    return PeerChangeEvent{Kind::Joined, std::move(peer), PeerState::Alive,
                           PeerState::Alive};
  }
  static PeerChangeEvent left(P peer) {
    return PeerChangeEvent{Kind::Left, std::move(peer), PeerState::Alive,
                           PeerState::Alive};
  }
  static PeerChangeEvent state_changed(P peer, PeerState from, PeerState to) {
    return PeerChangeEvent{Kind::StateChanged, std::move(peer), from, to};
  }

  bool operator==(const PeerChangeEvent& o) const {
    if (kind != o.kind || peer != o.peer) return false;
    if (kind == Kind::StateChanged) return from == o.from && to == o.to;
    return true;
  }
  bool operator!=(const PeerChangeEvent& o) const { return !(*this == o); }
};

// Tunables for the failure detector + SWIM state machine.
struct MembershipConfig {
  // `phi > phi_threshold` marks a peer `Suspect`.
  double phi_threshold = 8.0;
  // Ticks a peer stays `Suspect` before being declared `Dead`.
  uint64_t suspect_timeout = 5;
  // Sliding window size for heartbeat inter-arrival samples.
  size_t max_samples = 100;
  // Floor on the sample standard deviation (avoids div-by-zero).
  double min_std = 0.1;
};

namespace membership_detail {
inline uint64_t saturating_sub(uint64_t a, uint64_t b) {
  return a >= b ? a - b : 0;
}
}  // namespace membership_detail

// Phi-accrual failure detector over a sliding window of heartbeat inter-arrival
// times. `phi` is bit-portable across bindings via the Akka-style logistic
// approximation of the normal CDF.
class PhiAccrual {
 public:
  PhiAccrual(size_t max_samples, double min_std)
      : max_samples_(max_samples < 1 ? 1 : max_samples), min_std_(min_std) {}

  // Record a heartbeat arrival, appending its inter-arrival sample.
  void heartbeat(uint64_t now) {
    if (last_heartbeat_) {
      double interval =
          static_cast<double>(membership_detail::saturating_sub(now, *last_heartbeat_));
      window_.push_back(interval);
      while (window_.size() > max_samples_) window_.pop_front();
    }
    last_heartbeat_ = now;
  }

  // The suspicion level at `now`. `0.0` when there is no estimate yet.
  double phi(uint64_t now) const {
    if (!last_heartbeat_) return 0.0;
    if (window_.empty()) return 0.0;
    double elapsed =
        static_cast<double>(membership_detail::saturating_sub(now, *last_heartbeat_));
    double mean = this->mean();
    double std = this->std(mean);
    double y = (elapsed - mean) / std;
    double e = std::exp(-y * (1.5976 + 0.070566 * y * y));
    if (elapsed > mean) {
      return -std::log10(e / (1.0 + e));
    } else {
      return -std::log10(1.0 - 1.0 / (1.0 + e));
    }
  }

 private:
  std::deque<double> window_;
  size_t max_samples_;
  double min_std_;
  std::optional<uint64_t> last_heartbeat_;

  double mean() const {
    double n = static_cast<double>(window_.size());
    double sum = 0.0;
    for (double x : window_) sum += x;
    return sum / n;
  }

  double std(double mean) const {
    double n = static_cast<double>(window_.size());
    double var = 0.0;
    for (double x : window_) var += (x - mean) * (x - mean);
    var /= n;
    double s = std::sqrt(var);
    return s > min_std_ ? s : min_std_;
  }
};

// The pure membership compute core: the SWIM state machine over a keyed peer
// map, driven by heartbeats and a logical clock. Emits `PeerChangeEvent`s.
template <typename P>
class MembershipCore {
 public:
  using Event = PeerChangeEvent<P>;

  explicit MembershipCore(MembershipConfig config) : config_(config) {}

  // The current alive peer set (the reactive `PeerSet`).
  std::set<P> alive_set() const {
    std::set<P> out;
    for (const auto& kv : peers_) {
      if (kv.second.state == PeerState::Alive) out.insert(kv.first);
    }
    return out;
  }

  // The state of a known peer.
  std::optional<PeerState> state(const P& peer) const {
    auto it = peers_.find(peer);
    if (it == peers_.end()) return std::nullopt;
    return it->second.state;
  }

  // Join a peer (or refresh a re-joining one): `Alive` with a fresh detector.
  std::vector<Event> join(P peer, uint64_t now) {
    PhiAccrual detector = new_detector();
    detector.heartbeat(now);
    auto it = peers_.find(peer);
    bool known = it != peers_.end();
    std::optional<PeerState> prev;
    if (known) prev = it->second.state;

    PeerRecord record{PeerState::Alive, detector, std::nullopt};
    peers_.insert_or_assign(peer, std::move(record));

    if (!known) return {Event::joined(peer)};
    if (prev && *prev == PeerState::Alive) return {};
    if (prev) return {Event::state_changed(peer, *prev, PeerState::Alive)};
    return {};
  }

  // Record a heartbeat. An unknown peer is a join; a `Suspect`/`Dead` peer
  // returns to `Alive` (SWIM refutation).
  std::vector<Event> heartbeat(P peer, uint64_t now) {
    auto it = peers_.find(peer);
    if (it == peers_.end()) return join(peer, now);
    it->second.detector.heartbeat(now);
    PeerState from = it->second.state;
    if (from != PeerState::Alive && from != PeerState::Left) {
      it->second.state = PeerState::Alive;
      it->second.suspect_since = std::nullopt;
      return {Event::state_changed(peer, from, PeerState::Alive)};
    }
    return {};
  }

  // Graceful departure.
  std::vector<Event> leave(P peer, uint64_t /*now*/) {
    auto it = peers_.find(peer);
    if (it == peers_.end()) return {};
    if (it->second.state == PeerState::Left) return {};
    it->second.state = PeerState::Left;
    it->second.suspect_since = std::nullopt;
    return {Event::left(peer)};
  }

  // Advance the clock: escalate `Alive -> Suspect` (phi crossed) and
  // `Suspect -> Dead` (timeout elapsed).
  std::vector<Event> tick(uint64_t now) {
    double threshold = config_.phi_threshold;
    uint64_t timeout = config_.suspect_timeout;
    std::vector<Event> events;
    for (auto& kv : peers_) {
      const P& peer = kv.first;
      PeerRecord& record = kv.second;
      switch (record.state) {
        case PeerState::Alive: {
          if (record.detector.phi(now) > threshold) {
            record.state = PeerState::Suspect;
            record.suspect_since = now;
            events.push_back(
                Event::state_changed(peer, PeerState::Alive, PeerState::Suspect));
          }
          break;
        }
        case PeerState::Suspect: {
          bool expired =
              record.suspect_since &&
              membership_detail::saturating_sub(now, *record.suspect_since) >= timeout;
          if (expired) {
            record.state = PeerState::Dead;
            events.push_back(
                Event::state_changed(peer, PeerState::Suspect, PeerState::Dead));
          }
          break;
        }
        case PeerState::Dead:
        case PeerState::Left:
          break;
      }
    }
    return events;
  }

 private:
  struct PeerRecord {
    PeerState state;
    PhiAccrual detector;
    std::optional<uint64_t> suspect_since;
  };

  MembershipConfig config_;
  // BTreeMap analogue: ordered map keeps `tick` iteration + `alive_set`
  // deterministic in key order.
  std::map<P, PeerRecord> peers_;

  PhiAccrual new_detector() const {
    return PhiAccrual(config_.max_samples, config_.min_std);
  }
};

// Reactive membership: drives a `MembershipCore` and projects the alive set
// onto a `Cell` so the `PeerSet` invalidates only on a set change.
template <typename P>
class MembershipCell {
 public:
  using Event = PeerChangeEvent<P>;

  MembershipCell(Context& ctx, MembershipConfig config)
      : core_(config), peer_set_(ctx.cell(std::set<P>{})) {}

  std::vector<Event> join(Context& ctx, P peer, uint64_t now) {
    auto events = core_.join(std::move(peer), now);
    refresh(ctx);
    return events;
  }

  std::vector<Event> heartbeat(Context& ctx, P peer, uint64_t now) {
    auto events = core_.heartbeat(std::move(peer), now);
    refresh(ctx);
    return events;
  }

  std::vector<Event> leave(Context& ctx, P peer, uint64_t now) {
    auto events = core_.leave(std::move(peer), now);
    refresh(ctx);
    return events;
  }

  std::vector<Event> tick(Context& ctx, uint64_t now) {
    auto events = core_.tick(now);
    refresh(ctx);
    return events;
  }

  // The reactive alive peer set (`PeerSet`).
  std::set<P> peer_set(Context& ctx) { return ctx.get_cell(peer_set_); }

  // The backing `PeerSet` cell, for direct subscription.
  CellHandle<std::set<P>> peer_set_cell() const { return peer_set_; }

  std::optional<PeerState> state(const P& peer) const { return core_.state(peer); }

 private:
  MembershipCore<P> core_;
  CellHandle<std::set<P>> peer_set_;

  void refresh(Context& ctx) {
    // set_cell dedups (std::set has operator!=), so the PeerSet reader only
    // invalidates when the alive set actually changes.
    ctx.set_cell(peer_set_, core_.alive_set());
  }
};

// The derived reactive alive-peer set — a `Cell<std::set<P>>` handle exposed by
// `MembershipCell::peer_set_cell`.
template <typename P>
using PeerSet = CellHandle<std::set<P>>;

}  // namespace lazily

#endif  // LAZILY_MEMBERSHIP_HPP
