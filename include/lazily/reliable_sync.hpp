#ifndef LAZILY_RELIABLE_SYNC_HPP
#define LAZILY_RELIABLE_SYNC_HPP

// Reliable sync protocol (`#lzsync`).
//
// Delivery-reliability over the `Snapshot`/`Delta`/`CrdtSync` planes (`lazily-spec`
// § Reliable Sync): gap recovery, at-least-once outbox, and OR-set / LWW liveness
// cells. Correctness backstop: `lazily-formal` `ReliableSync.lean`; cross-language
// pins: `lazily-spec/conformance/reliable-sync/`.
//
// Three pure-protocol pieces (identical logic in every binding, no I/O / clock /
// storage engine): `ResyncCoordinator`, `DurableOutbox` (+ `InMemoryOutbox`), and
// the `OrSet` / `WireLwwRegister` liveness cells. The reverse-channel control
// frames are `IpcMessageResyncRequest` / `IpcMessageOutboxAck` — variants on the
// same bidirectional plane as the state frames (see `ipc.hpp`). The full-duplex
// loop shape is `SyncDriver`, wired to a caller-supplied transport seam.

#include <lazily/hlc.hpp>
#include <lazily/ipc.hpp>
#include <lazily/types.hpp>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lazily {

// -- ResyncAction: receiver decision for an inbound frame (spec § ResyncCoordinator) --

struct ResyncAction {
  enum class Kind {
    Apply,            // Apply the frame and advance the receiver epoch.
    RequestSnapshot,  // A gap was detected; request a fresh Snapshot covering from_epoch.
    Ignore,           // Drop the frame (re-delivery, empty, suppressed duplicate, or control frame).
  };
  Kind kind;
  Epoch from_epoch = 0;  // valid only when kind == RequestSnapshot

  static ResyncAction apply() { return {Kind::Apply, 0}; }
  static ResyncAction request_snapshot(Epoch from_epoch) { return {Kind::RequestSnapshot, from_epoch}; }
  static ResyncAction ignore() { return {Kind::Ignore, 0}; }

  bool is_apply() const { return kind == Kind::Apply; }
  bool is_request_snapshot() const { return kind == Kind::RequestSnapshot; }
  bool is_ignore() const { return kind == Kind::Ignore; }
};

// -- ResyncCoordinator: receiver-side reliable-sync coordinator --
//
// Holds `last_epoch` (highest epoch fully applied) and a `resyncing` flag (a
// RequestSnapshot is outstanding until a covering Snapshot lands). `ingest`
// advances `last_epoch` on Apply; the caller MUST fold the frame's ops on Apply.
// Mirrors `ReliableSync.step`.
class ResyncCoordinator {
 public:
  explicit ResyncCoordinator(Epoch last_epoch = 0) : last_epoch_(last_epoch) {}

  Epoch last_epoch() const { return last_epoch_; }
  bool is_resyncing() const { return resyncing_; }

  // Classify + fold an inbound Delta; advances to `delta.epoch` on Apply (multi-epoch aware).
  ResyncAction ingest_delta(const Delta& delta) {
    if (delta.base_epoch == last_epoch_) {
      if (delta.epoch >= delta.base_epoch + 1) {
        last_epoch_ = delta.epoch;
        resyncing_ = false;
        return ResyncAction::apply();
      }
      return ResyncAction::ignore();  // empty/backward epoch
    }
    if (delta.base_epoch < last_epoch_) {
      return ResyncAction::ignore();  // already applied — re-delivery
    }
    // gap: base_epoch > last_epoch
    if (resyncing_) {
      return ResyncAction::ignore();  // suppress duplicate request
    }
    resyncing_ = true;
    return ResyncAction::request_snapshot(last_epoch_);
  }

  // Adopt a Snapshot — a full-state frame always applies.
  ResyncAction ingest_snapshot(Epoch snapshot_epoch) {
    last_epoch_ = snapshot_epoch;
    resyncing_ = false;
    return ResyncAction::apply();
  }

  // Classify an inbound IpcMessage. CrdtSync rides the CRDT plane and the
  // reverse-channel control frames are for the sender's driver, so both are
  // ignored by this data receiver.
  ResyncAction ingest(const IpcMessage& msg) {
    if (std::holds_alternative<IpcMessageSnapshot>(msg))
      return ingest_snapshot(std::get<IpcMessageSnapshot>(msg).value.epoch);
    if (std::holds_alternative<IpcMessageDelta>(msg))
      return ingest_delta(std::get<IpcMessageDelta>(msg).value);
    return ResyncAction::ignore();  // CrdtSync / ResyncRequest / OutboxAck
  }

  // The OutboxAck message advertising this receiver's resume cursor.
  IpcMessage ack() const { return ipc_outbox_ack(last_epoch_); }

 private:
  Epoch last_epoch_;
  bool resyncing_ = false;
};

// -- DurableOutbox: sender-side at-least-once outbox contract (spec § DurableOutbox) --
//
// Every frame is durably `append`ed before it is sent, retained until the peer
// proves receipt (`ack_through`), and `replay_from` a reconnect cursor re-sends
// everything unacked. With the receiver's idempotent Ignore of already-applied
// deltas this is at-least-once delivery with exactly-once effect.
class DurableOutbox {
 public:
  virtual ~DurableOutbox() = default;

  // Persist `msg` at `epoch` before the send is attempted.
  virtual void append(Epoch epoch, const IpcMessage& msg) = 0;

  // The peer proved receipt through `epoch`; retained frames <= epoch MAY be pruned.
  virtual void ack_through(Epoch epoch) = 0;

  // Retained frames with epoch > cursor, ascending.
  virtual std::vector<std::pair<Epoch, IpcMessage>> replay_from(Epoch cursor) const = 0;

  // Epochs still retained (not yet acked), ascending — diagnostics/tests.
  virtual std::vector<Epoch> retained_epochs() const = 0;
};

// In-memory DurableOutbox — correct within a process lifetime; the default.
class InMemoryOutbox : public DurableOutbox {
 public:
  void append(Epoch epoch, const IpcMessage& msg) override {
    entries_.emplace_back(epoch, msg);
  }

  void ack_through(Epoch epoch) override {
    if (epoch > acked_through_) acked_through_ = epoch;
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [this](const std::pair<Epoch, IpcMessage>& e) {
                                    return e.first <= acked_through_;
                                  }),
                   entries_.end());
  }

  std::vector<std::pair<Epoch, IpcMessage>> replay_from(Epoch cursor) const override {
    std::vector<std::pair<Epoch, IpcMessage>> out;
    for (const auto& e : entries_) {
      if (e.first > cursor) out.push_back(e);
    }
    std::sort(out.begin(), out.end(),
              [](const std::pair<Epoch, IpcMessage>& a, const std::pair<Epoch, IpcMessage>& b) {
                return a.first < b.first;
              });
    return out;
  }

  std::vector<Epoch> retained_epochs() const override {
    std::vector<Epoch> out;
    out.reserve(entries_.size());
    for (const auto& e : entries_) out.push_back(e.first);
    std::sort(out.begin(), out.end());
    return out;
  }

  Epoch acked_through() const { return acked_through_; }

 private:
  std::vector<std::pair<Epoch, IpcMessage>> entries_;
  Epoch acked_through_ = 0;
};

// -- OrSet: observed-remove set liveness cell (spec § lzsync-liveness) --
//
// A tag is present iff some add-tag is not shadowed by a remove that observed it
// (add-wins over a stale remove). Join is the union of both tag sets — a
// semilattice, so out-of-order and duplicate delivery converge.
class OrSet {
 public:
  void add(const std::string& tag) { adds_.insert(tag); }

  void remove_observed(const std::vector<std::string>& tags) {
    for (const auto& t : tags) removes_.insert(t);
  }

  bool present() const {
    for (const auto& a : adds_) {
      if (removes_.find(a) == removes_.end()) return true;
    }
    return false;
  }

  void join(const OrSet& other) {
    adds_.insert(other.adds_.begin(), other.adds_.end());
    removes_.insert(other.removes_.begin(), other.removes_.end());
  }

 private:
  std::set<std::string> adds_;
  std::set<std::string> removes_;
};

// -- WireLwwRegister: last-writer-wins register liveness cell --
//
// Keyed by WireStamp (wall_time, logical, peer) total order: the highest stamp
// wins. Join is the stamp-max, a semilattice.

inline bool wire_stamp_greater(const WireStamp& a, const WireStamp& b) {
  if (a.wall_time != b.wall_time) return a.wall_time > b.wall_time;
  if (a.logical != b.logical) return a.logical > b.logical;
  return a.peer > b.peer;
}

template <class V>
class WireLwwRegister {
 public:
  WireLwwRegister(WireStamp stamp, V value) : stamp_(stamp), value_(std::move(value)) {}

  const WireStamp& stamp() const { return stamp_; }
  const V& value() const { return value_; }

  // Write `value` at `stamp` iff it dominates the current stamp.
  void set(const WireStamp& stamp, const V& value) {
    if (wire_stamp_greater(stamp, stamp_)) {
      stamp_ = stamp;
      value_ = value;
    }
  }

  // Join another replica's register (keep the higher stamp).
  void join(const WireLwwRegister<V>& other) { set(other.stamp_, other.value_); }

 private:
  WireStamp stamp_;
  V value_;
};

// ---------------------------------------------------------------------------
// SyncDriver + transport seam (`#sync-driver`, `#lzsync-transport-seam`).
//
// The loop shape that wires an outbound producer to a transport through the
// outbox and drives resync on reconnect. It owns no clock and no runtime: the
// host calls `SyncDriver::tick` from its own scheduler, so the driver stays a
// pure state machine over injected seams. Semantics are normative and identical
// across bindings (spec § SyncDriver); the seams carry no wire form of their own
// — what crosses the wire is the codec-encoded IpcMessage frame.
// ---------------------------------------------------------------------------

// Outbound transport seam. Deliver exactly one already-encoded protocol frame:
// returns `true` if handed to the peer, `false` on transport failure. At-least-once
// is a SyncDriver property, not a sink property — on a failed send the driver
// keeps the frame in the DurableOutbox and replays it after the next reconnect.
using IpcSink = std::function<bool(const IpcMessage&)>;

// Inbound transport seam. Poll for the next frame without blocking: an empty
// optional means the source is currently exhausted or closed (no inbound progress
// this tick). A thrown exception is the reconnect signal: it surfaces from
// `SyncDriver::tick` as `SyncDriverSourceException`; the host re-establishes the
// byte carrier and calls `SyncDriver::on_reconnect`.
using IpcSource = std::function<std::optional<IpcMessage>()>;

// Monotonic clock seam — policy injected, no runtime in core. Milliseconds from
// an arbitrary fixed origin; monotonic, non-decreasing.
using Clock = std::function<int64_t()>;

// Sender-side answer to a peer's ResyncRequest. Produces a full-state Snapshot
// message covering `from_epoch` (its epoch MUST be >= from_epoch).
using SnapshotProvider = std::function<IpcMessage(Epoch)>;

// Surfaced by `SyncDriver::tick` when the inbound IpcSource read fails. A sink
// failure, by contrast, is retain-and-stall (reported through Progress/stall
// signals), NOT an exception.
class SyncDriverSourceException : public std::runtime_error {
 public:
  SyncDriverSourceException()
      : std::runtime_error(
            "inbound IpcSource read failed — reconnect and call on_reconnect()") {}
};

// What one `SyncDriver::tick` accomplished (spec § SyncDriver).
struct Progress {
  int sent = 0;                       // data frames pushed to the sink this tick
  std::vector<IpcMessage> applied;    // inbound frames the host must fold into its projection
  bool resync_requested = false;      // a gap was detected inbound and a ResyncRequest emitted
  int snapshots_served = 0;           // inbound ResyncRequests answered with a provider snapshot
  Epoch peer_acked_through = 0;       // the peer's ack cursor after this tick
  int retained = 0;                   // outbox frames still unacked
};

// Full-duplex reliable-sync loop driver (spec § SyncDriver). One driver drives
// one peer connection over a caller-supplied IpcSink/IpcSource pair. It composes
// the three pure-protocol pieces into the loop shape the spec pins:
//
//   1. resync-on-reconnect — `on_reconnect` arms a replay of the unacked outbox
//      suffix from the peer's ack cursor;
//   2. drain — pop host-enqueued outbound data frames, append each BEFORE sending
//      (at-least-once durability), send via the sink; a send error leaves the frame
//      in the outbox (unacked) and stalls the drain;
//   3. receive — read inbound frames, route control frames (OutboxAck -> advance
//      retention; ResyncRequest -> answer with a provider snapshot) and feed data
//      frames through the ResyncCoordinator;
//   4. advertise — if anything was applied, send an OutboxAck carrying the new
//      receiver cursor (retried until it lands).
//
// The driver owns no threads, no clock source, and no storage engine — the host
// injects all three and decides the tick cadence.
class SyncDriver {
 public:
  SyncDriver(IpcSink sink, IpcSource source, std::shared_ptr<DurableOutbox> outbox,
             Clock clock, SnapshotProvider provider, Epoch last_epoch = 0)
      : sink_(std::move(sink)),
        source_(std::move(source)),
        outbox_(std::move(outbox)),
        clock_(std::move(clock)),
        provider_(std::move(provider)),
        coordinator_(last_epoch) {}

  // Borrowable outbox (diagnostics / durable-store flush).
  DurableOutbox& outbox() { return *outbox_; }

  // Stage an outbound data frame at `epoch` for the next tick's drain. `epoch` is
  // the frame's accepted-event count; it becomes the outbox retention key.
  void enqueue(Epoch epoch, const IpcMessage& msg) {
    pending_.emplace_back(epoch, msg);
  }

  // Signal that the transport was re-established; the next tick replays the
  // unacked outbox suffix and re-advertises our receiver cursor.
  void on_reconnect() {
    replay_pending_ = true;
    ack_owed_ = true;
    stalled_since_.reset();
  }

  Epoch last_epoch() const { return coordinator_.last_epoch(); }
  bool is_stalled() const { return stalled_since_.has_value(); }

  // Millis the sink has been stalled as of `now`, or 0 when healthy.
  int64_t stalled_for(int64_t now) const {
    if (!stalled_since_) return 0;
    int64_t d = now - *stalled_since_;
    return d < 0 ? 0 : d;
  }

  // Run one loop pass (drain -> retain -> receive -> resync). Sink failures
  // retain-and-stall (not an exception); only an inbound source read failure
  // throws SyncDriverSourceException.
  Progress tick() {
    const int64_t now = clock_();
    Progress prog;

    // 1. resync-on-reconnect: replay the unacked outbox suffix, oldest first.
    if (replay_pending_) {
      replay_pending_ = false;
      for (const auto& e : outbox_->replay_from(peer_acked_through_)) {
        if (sink_(e.second)) {
          prog.sent += 1;
        } else {
          stalled_since_ = now;
          replay_pending_ = true;  // finish the replay after the next reconnect
          break;
        }
      }
    }

    // 2. drain fresh enqueues: append-before-send, retain-and-stop on failure.
    //    A pre-existing stall skips the drain entirely.
    while (!stalled_since_ && !pending_.empty()) {
      auto entry = pending_.front();
      outbox_->append(entry.first, entry.second);
      pending_.pop_front();
      if (sink_(entry.second)) {
        prog.sent += 1;
        stalled_since_.reset();
      } else {
        // Retained in the outbox (unacked) -> replayed on reconnect.
        stalled_since_ = now;
        break;
      }
    }

    // 3. receive: route control frames + feed data frames through the coordinator.
    while (true) {
      std::optional<IpcMessage> maybe;
      try {
        maybe = source_();
      } catch (const SyncDriverSourceException&) {
        throw;
      } catch (...) {
        throw SyncDriverSourceException();
      }
      if (!maybe) break;
      const IpcMessage& msg = *maybe;

      if (std::holds_alternative<IpcMessageOutboxAck>(msg)) {
        Epoch through = std::get<IpcMessageOutboxAck>(msg).value.through_epoch;
        if (through > peer_acked_through_) peer_acked_through_ = through;
        outbox_->ack_through(through);
      } else if (std::holds_alternative<IpcMessageResyncRequest>(msg)) {
        Epoch from = std::get<IpcMessageResyncRequest>(msg).value.from_epoch;
        IpcMessage snap = provider_(from);
        if (sink_(snap)) prog.snapshots_served += 1; else stalled_since_ = now;
      } else if (std::holds_alternative<IpcMessageCrdtSync>(msg)) {
        // Idempotent anti-entropy plane — the host folds it directly.
        prog.applied.push_back(msg);
      } else {  // Snapshot / Delta
        ResyncAction action = coordinator_.ingest(msg);
        switch (action.kind) {
          case ResyncAction::Kind::Apply:
            ack_owed_ = true;
            prog.applied.push_back(msg);
            break;
          case ResyncAction::Kind::RequestSnapshot: {
            IpcMessage req = ipc_resync_request(action.from_epoch);
            if (sink_(req)) prog.resync_requested = true; else stalled_since_ = now;
            break;
          }
          case ResyncAction::Kind::Ignore:
            break;
        }
      }
    }

    // 4. advertise our receiver cursor if we applied anything (retry until sent).
    if (ack_owed_ && sink_(coordinator_.ack())) {
      ack_owed_ = false;
    }

    prog.peer_acked_through = peer_acked_through_;
    prog.retained = static_cast<int>(outbox_->retained_epochs().size());
    return prog;
  }

 private:
  IpcSink sink_;
  IpcSource source_;
  std::shared_ptr<DurableOutbox> outbox_;
  Clock clock_;
  SnapshotProvider provider_;
  ResyncCoordinator coordinator_;

  std::deque<std::pair<Epoch, IpcMessage>> pending_;
  Epoch peer_acked_through_ = 0;
  bool ack_owed_ = false;
  bool replay_pending_ = false;
  std::optional<int64_t> stalled_since_;
};

}  // namespace lazily

#endif  // LAZILY_RELIABLE_SYNC_HPP
