#ifndef LAZILY_COORDINATION_HPP
#define LAZILY_COORDINATION_HPP

// Distributed coordination primitives (`#lzcoord`).
//
// A C++17 port of `lazily-rs/src/coordination.rs`. See
// `lazily-spec/docs/coordination.md` and the formal model
// `lazily-formal/LazilyFormal/Coordination.lean`. Lease / leader / lock /
// semaphore / barrier + quorum primitives, each a pure **compute core** (a state
// machine over integers / peer ids — bytes-payload, C++-eligible) split from a
// reactive **cell** projecting the salient reader onto a `Source`. Time is a
// logical clock — a monotone `now` (uint64_t) tick the runtime drives; `expiry`
// is a tick value. A reader invalidates only when its projection changes (the
// backend-portability rule).

#include <cstdint>
#include <optional>
#include <set>
#include <utility>

#include <lazily/context.hpp>
#include <lazily/cell.hpp>

namespace lazily {

// ===========================================================================
// Lease + fencing token
// ===========================================================================

/// Single-writer lease authority with a monotone fencing token.
template <typename P>
class LeaseCore {
 public:
  LeaseCore() : holder_(std::nullopt), expiry_(0), fence_(0) {}

  /// Whether the lease is currently held (and not expired at `now`).
  bool is_held(uint64_t now) const {
    return holder_.has_value() && !is_expired(now);
  }

  /// The live holder at `now`.
  std::optional<P> holder(uint64_t now) const {
    if (is_held(now)) return holder_;
    return std::nullopt;
  }

  uint64_t fence() const { return fence_; }

  /// Grant if free/expired (new grant increments `fence`); renew by the holder
  /// keeps the same fence; held by another -> `std::nullopt`.
  std::optional<uint64_t> acquire(P peer, uint64_t now, uint64_t ttl) {
    bool free = !holder_.has_value() || is_expired(now);
    if (free) {
      fence_ += 1;
      holder_ = std::move(peer);
      expiry_ = now + ttl;
      return fence_;
    }
    if (holder_ == peer) {
      expiry_ = now + ttl;  // renew keeps fence
      return fence_;
    }
    return std::nullopt;
  }

  /// Extend the expiry if `peer` is the live holder.
  bool renew(const P& peer, uint64_t now, uint64_t ttl) {
    if (is_held(now) && holder_ == peer) {
      expiry_ = now + ttl;
      return true;
    }
    return false;
  }

  /// Drop the grant if `peer` holds it.
  void release(const P& peer) {
    if (holder_ == peer) holder_ = std::nullopt;
  }

  /// Expire the grant when `now >= expiry`; returns the expiry edge.
  bool tick(uint64_t now) {
    if (is_expired(now)) {
      holder_ = std::nullopt;
      return true;
    }
    return false;
  }

 private:
  bool is_expired(uint64_t now) const {
    return holder_.has_value() && now >= expiry_;
  }

  std::optional<P> holder_;
  uint64_t expiry_;
  uint64_t fence_;
};

/// Reactive lease: projects the holder onto a cell (invalidates on holder
/// change).
template <typename P>
class LeaseCell {
 public:
  explicit LeaseCell(Context& ctx)
      : core_(), holder_(ctx.source(std::optional<P>(std::nullopt))) {}

  std::optional<uint64_t> acquire(Context& ctx, P peer, uint64_t now,
                                  uint64_t ttl) {
    auto r = core_.acquire(std::move(peer), now, ttl);
    refresh(ctx, now);
    return r;
  }

  bool renew(Context& ctx, const P& peer, uint64_t now, uint64_t ttl) {
    bool r = core_.renew(peer, now, ttl);
    refresh(ctx, now);
    return r;
  }

  void release(Context& ctx, const P& peer, uint64_t now) {
    core_.release(peer);
    refresh(ctx, now);
  }

  bool tick(Context& ctx, uint64_t now) {
    bool r = core_.tick(now);
    refresh(ctx, now);
    return r;
  }

  std::optional<P> holder(uint64_t now) const { return core_.holder(now); }
  bool is_held(uint64_t now) const { return core_.is_held(now); }
  uint64_t fence() const { return core_.fence(); }
  Source<std::optional<P>> holder_cell() const { return holder_; }

 private:
  void refresh(Context& ctx, uint64_t now) {
    ctx.set(holder_, core_.holder(now));
  }

  LeaseCore<P> core_;
  Source<std::optional<P>> holder_;
};

// ===========================================================================
// Leader / follower / candidate
// ===========================================================================

/// The local node's role, derived from lease ownership.
enum class LeaderRole {
  Leader,
  Follower,
  Candidate,
};

/// Reactive leadership over a lease from node `me`'s perspective.
template <typename P>
class LeaderCell {
 public:
  LeaderCell(Context& ctx, P me)
      : core_(),
        me_(std::move(me)),
        current_leader_(ctx.source(std::optional<P>(std::nullopt))) {}

  /// Try to acquire leadership for `me`.
  LeaderRole campaign(Context& ctx, uint64_t now, uint64_t ttl) {
    core_.acquire(me_, now, ttl);
    refresh(ctx, now);
    return role(now);
  }

  /// Simulate another peer contending (mostly for tests / co-hosted nodes).
  LeaderRole contend(Context& ctx, P peer, uint64_t now, uint64_t ttl) {
    core_.acquire(std::move(peer), now, ttl);
    refresh(ctx, now);
    return role(now);
  }

  LeaderRole tick(Context& ctx, uint64_t now) {
    core_.tick(now);
    refresh(ctx, now);
    return role(now);
  }

  std::optional<P> current_leader(uint64_t now) const {
    return core_.holder(now);
  }

  LeaderRole role(uint64_t now) const {
    auto h = core_.holder(now);
    if (!h.has_value()) return LeaderRole::Candidate;
    return (*h == me_) ? LeaderRole::Leader : LeaderRole::Follower;
  }

  Source<std::optional<P>> current_leader_cell() const {
    return current_leader_;
  }

 private:
  void refresh(Context& ctx, uint64_t now) {
    ctx.set(current_leader_, core_.holder(now));
  }

  LeaseCore<P> core_;
  P me_;
  Source<std::optional<P>> current_leader_;
};

// ===========================================================================
// Distributed lock + fencing
// ===========================================================================

/// Reactive distributed mutex over a lease + fencing token.
template <typename P>
class LockCell {
 public:
  explicit LockCell(Context& ctx) : core_(), is_locked_(ctx.source(false)) {}

  /// Acquire the lock, returning a fencing token, or `std::nullopt` if held.
  std::optional<uint64_t> acquire(Context& ctx, P peer, uint64_t now,
                                  uint64_t ttl) {
    auto r = core_.acquire(std::move(peer), now, ttl);
    refresh(ctx, now);
    return r;
  }

  void release(Context& ctx, const P& peer, uint64_t now) {
    core_.release(peer);
    refresh(ctx, now);
  }

  bool tick(Context& ctx, uint64_t now) {
    bool r = core_.tick(now);
    refresh(ctx, now);
    return r;
  }

  /// Whether `fence` is the current (non-stale) fencing token.
  bool validate(uint64_t fence) const { return core_.fence() == fence; }

  bool is_locked(uint64_t now) const { return core_.is_held(now); }
  uint64_t fence() const { return core_.fence(); }
  Source<bool> is_locked_cell() const { return is_locked_; }

 private:
  void refresh(Context& ctx, uint64_t now) {
    ctx.set(is_locked_, core_.is_held(now));
  }

  LeaseCore<P> core_;
  Source<bool> is_locked_;
};

// ===========================================================================
// Semaphore
// ===========================================================================

/// Bounded permit pool compute core.
class SemaphoreCore {
 public:
  explicit SemaphoreCore(uint64_t capacity)
      : capacity_(capacity), acquired_(0) {}

  uint64_t available() const { return capacity_ - acquired_; }

  bool acquire() {
    if (acquired_ < capacity_) {
      acquired_ += 1;
      return true;
    }
    return false;
  }

  void release() {
    if (acquired_ > 0) acquired_ -= 1;
  }

 private:
  uint64_t capacity_;
  uint64_t acquired_;
};

/// Reactive semaphore: projects `permits_available` onto a cell.
class SemaphoreCell {
 public:
  SemaphoreCell(Context& ctx, uint64_t capacity)
      : core_(capacity), available_(ctx.source<uint64_t>(capacity)) {}

  bool acquire(Context& ctx) {
    bool r = core_.acquire();
    refresh(ctx);
    return r;
  }

  void release(Context& ctx) {
    core_.release();
    refresh(ctx);
  }

  uint64_t permits_available(Context& ctx) const { return available_.get(ctx); }
  Source<uint64_t> permits_available_cell() const { return available_; }

 private:
  void refresh(Context& ctx) { ctx.set(available_, core_.available()); }

  SemaphoreCore core_;
  Source<uint64_t> available_;
};

// ===========================================================================
// Barrier / quorum
// ===========================================================================

/// Wait-for-N gate compute core over distinct arriving peers.
template <typename P>
class BarrierCore {
 public:
  explicit BarrierCore(uint64_t required) : required_(required) {}

  /// Register a distinct arrival; returns whether the gate is open afterward.
  bool arrive(P peer) {
    arrived_.insert(std::move(peer));
    return is_open();
  }

  uint64_t count() const { return static_cast<uint64_t>(arrived_.size()); }

  bool is_open() const { return count() >= required_; }

 private:
  uint64_t required_;
  std::set<P> arrived_;
};

/// Reactive wait-for-N gate. `quorum` is a barrier with
/// `required = total / 2 + 1`.
template <typename P>
class BarrierCell {
 public:
  BarrierCell(Context& ctx, uint64_t required)
      : core_(required), is_open_(ctx.source(core_.is_open())) {}

  /// A quorum gate: opens at strict majority of `total`.
  static BarrierCell<P> quorum(Context& ctx, uint64_t total) {
    return BarrierCell<P>(ctx, total / 2 + 1);
  }

  /// Register an arrival / vote; returns whether the gate is open afterward.
  bool arrive(Context& ctx, P peer) {
    bool r = core_.arrive(std::move(peer));
    refresh(ctx);
    return r;
  }

  uint64_t count() const { return core_.count(); }
  bool is_open(Context& ctx) const { return is_open_.get(ctx); }
  Source<bool> is_open_cell() const { return is_open_; }

 private:
  void refresh(Context& ctx) { ctx.set(is_open_, core_.is_open()); }

  BarrierCore<P> core_;
  Source<bool> is_open_;
};

}  // namespace lazily

#endif  // LAZILY_COORDINATION_HPP
