// RelayCell backpressure plan (#relaycell), Phases 2–6 — the header-only C++ port.
//
// See lazily-spec/docs/relaycell.md and relaycell-backpressure-analysis.md. A
// RelayCell is an *algebra-typed conflating relay*: it accumulates a fast ingress
// into a hot head (a merge-policy fold), bounds it with a reactive
// BackpressurePolicy, and lets a slow egress drain the coalesced window. The
// converged egress state is independent of the drain schedule whenever the merge
// ⊕ is associative (the relay_converges invariant, pinned in LazilyFormal.Relay).
//
// Phase 2 RelayCell + BackpressurePolicy · Phase 3 SpillStore · Phase 4 Transport
// · Phase 5 Outbox/Inbox roles · Phase 6 Rate/Window/Expiry/Priority/Keyed
// policies. Time is a logical clock (a monotone tick) so behaviour is
// deterministic and portable.
//
// Binding idiom: as in `merge.hpp`, the merge policy is a *compile-time type*
// (`RelayCell<T, Policy>`) carrying a static `merge` + `static constexpr`
// algebra flags. The Conflate/RawFifo validation is inherently runtime (the
// overflow is a reactive cell), so it is a runtime throw mirroring the Rust
// `Result` — a `static_assert` would be too strong for a policy whose overflow
// cell can be retuned live.

#ifndef LAZILY_RELAY_HPP
#define LAZILY_RELAY_HPP

#include <lazily/context.hpp>
#include <lazily/cell.hpp>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lazily {

// -- Phase 2: RelayCell + BackpressurePolicy ---------------------------------

/// What a bound measures (analysis §4.4). The Phase-2 core meters `Count`.
enum class BoundDim { Count, Bytes, Keys, Age };

/// The action taken when the hot head crosses `high_water` (analysis §4.4).
enum class Overflow {
  /// Refuse ingress; the producer backpressures (observes `is_full`). Lossless.
  Block,
  /// Discard the incoming op. Lossy.
  DropNewest,
  /// Reset the window to the incoming op, discarding what accumulated. Lossy.
  DropOldest,
  /// Keep merging — the coalescence *is* the bound. Requires `Policy::conflates`.
  Conflate,
  /// Page the accumulated window to a durable tail (Phase 3 `SpillStore`).
  Spill,
};

/// Why a construction/merge-swap was rejected (analysis §4.3 flag validation).
enum class RelayConfigError {
  /// `Conflate` chosen for a non-conflating policy (`RawFifo`).
  ConflateNotBounding,
};

/// Thrown by `RelayCell`'s constructor when the overflow choice is illegal for
/// the policy's algebra (mirrors the Rust `Err(RelayConfigError)`).
struct RelayConfigException : std::invalid_argument {
  RelayConfigError error;
  explicit RelayConfigException(RelayConfigError e)
      : std::invalid_argument("ConflateNotBounding"), error(e) {}
};

/// The outcome of a single `ingress` op.
enum class IngressOutcome {
  /// Merged into an empty window (window depth was 0).
  Accepted,
  /// Merged into a non-empty window (coalesced with prior ops).
  Conflated,
  /// Dropped by `DropNewest`/`DropOldest` overflow.
  Dropped,
  /// Refused by `Block` overflow; the producer must retry after a drain.
  Blocked,
};

/// Reactive backpressure limits (analysis §4.4). Every field is a cell, so an
/// operator or an adaptive controller retunes it live and dependent relays react.
/// Hysteresis (`high_water` ≠ `low_water`) prevents flapping.
struct BackpressurePolicy {
  Source<BoundDim> dimension;
  Source<std::uint64_t> high_water;
  Source<std::uint64_t> low_water;
  Source<Overflow> overflow;

  BackpressurePolicy(Context& ctx, BoundDim dimension_, std::uint64_t high_water_,
                     std::uint64_t low_water_, Overflow overflow_)
      : dimension(ctx.source(dimension_)),
        high_water(ctx.source(high_water_)),
        low_water(ctx.source(low_water_)),
        overflow(ctx.source(overflow_)) {}
};

/// The algebra-typed conflating relay (Phase 2, in-proc core). The hot head is a
/// cell; `depth`/`is_full`/`is_empty` are demand-driven slots, so an unobserved
/// relay costs `N·⊕` and no more (the merge cost law). `Policy` is a compile-time
/// type (a static `merge` + `static constexpr conflates`).
template <typename T, typename Policy>
class RelayCell {
 public:
  /// Build a relay over `policy`, validating the initial overflow against the
  /// policy's algebra flags (analysis §4.3): `Conflate` requires
  /// `Policy::conflates`. Throws `RelayConfigException` otherwise.
  RelayCell(Context& ctx, BackpressurePolicy policy)
      : ctx_(&ctx), policy_(policy) {
    if (ctx.get(policy_.overflow) == Overflow::Conflate && !Policy::conflates) {
      throw RelayConfigException(RelayConfigError::ConflateNotBounding);
    }
    head_ = ctx.source(std::optional<T>{});
    pending_ = ctx.source<std::uint64_t>(0);
    auto pending = pending_;
    auto head = head_;
    auto high_water = policy_.high_water;
    depth_ = ctx.computed<std::uint64_t>(
        [pending](Context& c) { return c.get(pending); });
    auto depth = depth_;
    is_full_ = ctx.computed<bool>([depth, high_water](Context& c) {
      return c.get(depth) >= c.get(high_water);
    });
    is_empty_ = ctx.computed<bool>(
        [head](Context& c) { return !c.get(head).has_value(); });
  }

  /// Whether the current overflow choice is legal for `Policy` — a runtime guard
  /// mirroring the constructor's check (the overflow cell is reactive).
  bool overflow_is_legal() const {
    return ctx_->get(policy_.overflow) != Overflow::Conflate || Policy::conflates;
  }

  /// Demand-driven reader handle: current window depth (`Count`).
  Computed<std::uint64_t> depth() const { return depth_; }
  /// Demand-driven reader handle: window is at/over `high_water`.
  Computed<bool> is_full() const { return is_full_; }
  /// Demand-driven reader handle: window is empty (nothing to drain).
  Computed<bool> is_empty() const { return is_empty_; }

  /// Convenience: read the current depth value.
  std::uint64_t depth_value() const { return ctx_->get(depth_); }
  /// Convenience: read the current is_full value.
  bool is_full_value() const { return ctx_->get(is_full_); }
  /// Convenience: read the current is_empty value.
  bool is_empty_value() const { return ctx_->get(is_empty_); }

  /// Ingest one op. Applies the reactive overflow policy when the window is at
  /// `high_water`; otherwise merges the op into the hot head under `Policy`.
  IngressOutcome ingress(T op) {
    bool was_empty = ctx_->get(pending_) == 0;

    if (read_full()) {
      switch (ctx_->get(policy_.overflow)) {
        case Overflow::Block:
          return IngressOutcome::Blocked;
        case Overflow::DropNewest:
          return IngressOutcome::Dropped;
        case Overflow::DropOldest:
          // Discard the accumulated window, restart from this op.
          ctx_->set(head_, std::optional<T>(std::move(op)));
          ctx_->set<std::uint64_t>(pending_, 1);
          return IngressOutcome::Dropped;
        // Conflate keeps merging (the coalescence is the bound); Spill is Phase 3
        // and, until wired, degrades to Conflate for a bounding policy. Both fall
        // through to the merge below.
        case Overflow::Conflate:
        case Overflow::Spill:
          break;
      }
    }

    merge_into_head(std::move(op));
    ctx_->set<std::uint64_t>(pending_, ctx_->get(pending_) + 1);
    return was_empty ? IngressOutcome::Accepted : IngressOutcome::Conflated;
  }

  /// Drain the coalesced window: take the hot head's value and reset the window.
  /// Returns `std::nullopt` for an empty window. `relay_converges` guarantees the
  /// egress fold equals the flat fold of every ingested op, for any drain schedule.
  std::optional<T> drain() {
    auto cur = ctx_->get(head_);
    if (cur.has_value()) {
      ctx_->set(head_, std::optional<T>{});
      ctx_->set<std::uint64_t>(pending_, 0);
    }
    return cur;
  }

  /// Peek the current coalesced window without draining.
  std::optional<T> peek() const { return ctx_->get(head_); }

 private:
  bool read_full() const {
    return ctx_->get(pending_) >= ctx_->get(policy_.high_water);
  }

  void merge_into_head(T op) {
    auto cur = ctx_->get(head_);
    T next = cur.has_value()
                 ? Policy::template merge<T>(cur.value(), std::move(op))
                 : std::move(op);
    ctx_->set(head_, std::optional<T>(std::move(next)));
  }

  Context* ctx_;
  BackpressurePolicy policy_;
  Source<std::optional<T>> head_;
  Source<std::uint64_t> pending_;
  Computed<std::uint64_t> depth_;
  Computed<bool> is_full_;
  Computed<bool> is_empty_;
};

// -- Phase 3: SpillStore -----------------------------------------------------

/// How spilled windows are laid out on the durable tail (analysis §6).
enum class SpillMode {
  /// Merge each spilled window into the open page until it fills — minimizes disk
  /// (keep-latest / semilattice). One page holds a coalesced run.
  CompactOnWrite,
  /// Append each spilled window as its own page — preserves increments for an
  /// accumulating (non-idempotent) policy that must not double-count.
  AppendCompact,
};

/// One immutable cold page: a coalesced window summary plus its manifest entry.
template <typename T>
struct SpillPage {
  std::uint64_t id;
  T summary;
  std::uint64_t bytes;
};

/// A paged durable tail for a `RelayCell` (Phase 3, in-memory reference backend).
/// Holds cold pages plus a bounded manifest, an egress cursor, and
/// ack-before-reclaim. Memory is `O(hot) + O(manifest)`.
template <typename T, typename Policy>
class SpillStore {
 public:
  SpillStore(SpillMode mode, std::uint64_t page_size)
      : mode_(mode), page_size_(page_size < 1 ? 1 : page_size) {}

  /// Spill one coalesced window summary to the durable tail. `AppendCompact`
  /// always opens a new page; `CompactOnWrite` merges into the open page until it
  /// reaches `page_size`, then seals it.
  void spill(T window, std::uint64_t bytes) {
    if (mode_ == SpillMode::AppendCompact) {
      push_page(std::move(window), bytes);
    } else {
      if (open_fill_ >= page_size_ || pages_.empty()) {
        push_page(std::move(window), bytes);
        open_fill_ = 1;
      } else {
        SpillPage<T>& last = pages_.back();
        last.summary = Policy::template merge<T>(last.summary, std::move(window));
        last.bytes += bytes;
        open_fill_ += 1;
      }
    }
  }

  /// The manifest: `(page_id, bytes)` for every live page (bounded metadata).
  std::vector<std::pair<std::uint64_t, std::uint64_t>> manifest() const {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
    out.reserve(pages_.size());
    for (const auto& p : pages_) out.emplace_back(p.id, p.bytes);
    return out;
  }

  /// Pages the egress has not yet acked (at/after the ack cursor).
  std::vector<SpillPage<T>> pending_pages() const {
    return std::vector<SpillPage<T>>(pages_.begin() + acked_, pages_.end());
  }

  std::size_t page_count() const { return pages_.size(); }

  /// Ack every page through `id` (inclusive), advancing the reclaim cursor.
  void ack_through(std::uint64_t id) {
    while (acked_ < pages_.size() && pages_[acked_].id <= id) ++acked_;
  }

  /// Drop acked pages (durable reclaim). Manifest/cursor stay consistent.
  void reclaim() {
    if (acked_ > 0) {
      pages_.erase(pages_.begin(), pages_.begin() + acked_);
      acked_ = 0;
    }
  }

  /// Fold every live cold page (oldest first) into `s0`.
  T fold_pages(T s0) const {
    T acc = std::move(s0);
    for (const auto& p : pages_) acc = Policy::template merge<T>(acc, p.summary);
    return acc;
  }

  /// **Reconstruction (spill_lossless).** Fold the cold tail then the hot head —
  /// reproduces the flat fold of every op the relay ever ingested.
  T reconstruct(T s0, std::optional<T> hot) const {
    T cold = fold_pages(std::move(s0));
    if (hot.has_value()) return Policy::template merge<T>(cold, std::move(hot).value());
    return cold;
  }

  /// **Crash replay.** Re-deliver every unacked page from the ack cursor into
  /// `downstream`. For an idempotent policy re-applying an already-delivered page
  /// is a no-op (`spill_replay_idempotent`), so at-least-once replay converges.
  T replay_unacked(T downstream) const {
    T acc = std::move(downstream);
    for (std::size_t i = acked_; i < pages_.size(); ++i)
      acc = Policy::template merge<T>(acc, pages_[i].summary);
    return acc;
  }

 private:
  void push_page(T summary, std::uint64_t bytes) {
    pages_.push_back(SpillPage<T>{next_id_, std::move(summary), bytes});
    ++next_id_;
  }

  std::vector<SpillPage<T>> pages_;
  SpillMode mode_;
  std::uint64_t page_size_;
  std::uint64_t open_fill_ = 0;
  std::uint64_t next_id_ = 0;
  std::size_t acked_ = 0;
};

// -- Phase 4: Transport ------------------------------------------------------
//
// Transport abstracts ingress/egress delivery so the mechanism is pluggable. A
// RelayCell is written once against Transport; the merge algebra — not the
// transport — guarantees converged state (transport_independent), so transports
// may differ across bindings and still converge. Expressed here as a small
// abstract base with two concrete ends of the spectrum.

/// A pluggable delivery mechanism for relay ops. `deliver` enqueues; `poll` pulls
/// the next transport-defined frame; `has_pending` reports buffered ops.
template <typename T>
class Transport {
 public:
  virtual ~Transport() = default;
  virtual void deliver(T op) = 0;
  virtual std::vector<T> poll() = 0;
  virtual bool has_pending() const = 0;
};

/// `InProc` — direct delivery: every buffered op is handed over in one frame.
template <typename T>
class InProcTransport : public Transport<T> {
 public:
  void deliver(T op) override { buf_.push_back(std::move(op)); }
  std::vector<T> poll() override {
    std::vector<T> out = std::move(buf_);
    buf_.clear();
    return out;
  }
  bool has_pending() const override { return !buf_.empty(); }

 private:
  std::vector<T> buf_;
};

/// A *framed* transport — models `CrossThread`/`Ipc`/`Ws`: ops are delivered in
/// bounded frames of at most `frame_size` (an MTU / batch boundary). Different
/// `frame_size`s are different framings of the same op stream.
template <typename T>
class FramedTransport : public Transport<T> {
 public:
  explicit FramedTransport(std::size_t frame_size)
      : frame_size_(frame_size < 1 ? 1 : frame_size) {}
  void deliver(T op) override { buf_.push_back(std::move(op)); }
  std::vector<T> poll() override {
    std::size_t n = frame_size_ < buf_.size() ? frame_size_ : buf_.size();
    std::vector<T> out(std::make_move_iterator(buf_.begin()),
                       std::make_move_iterator(buf_.begin() + n));
    buf_.erase(buf_.begin(), buf_.begin() + n);
    return out;
  }
  bool has_pending() const override { return !buf_.empty(); }

 private:
  std::vector<T> buf_;
  std::size_t frame_size_;
};

// -- Phase 5: Outbox / Inbox roles -------------------------------------------
//
// RelayCell is direction-neutral; Outbox and Inbox are role facades (typed
// constructors with direction-appropriate defaults), not reimplementations. They
// differ in the backpressure-propagation contract. A network link is
// Outbox → Transport → Inbox.

/// The app → transport send side (analysis §4.7). Backpressures the local
/// producer directly via `is_full`. Default overflow `Conflate` (state broadcast).
template <typename T, typename Policy>
class Outbox {
 public:
  Outbox(Context& ctx, std::uint64_t high_water, BoundDim dimension = BoundDim::Count,
         Overflow overflow = Overflow::Conflate)
      : relay_(ctx, BackpressurePolicy(ctx, dimension, high_water, high_water / 2, overflow)) {}

  /// The local producer sends an op. A `Blocked` outcome is the producer's
  /// backpressure signal — it should await a drain before retrying.
  IngressOutcome send(T op) { return relay_.ingress(std::move(op)); }

  /// The transport drains the coalesced window for egress.
  std::optional<T> drain() { return relay_.drain(); }

  /// The producer-facing backpressure signal (window at/over the watermark).
  Computed<bool> is_full() const { return relay_.is_full(); }

  /// Access the underlying relay (for wiring extra egress stages).
  RelayCell<T, Policy>& relay() { return relay_; }

 private:
  RelayCell<T, Policy> relay_;
};

/// The transport → app receive side (analysis §4.7). Cannot block the remote
/// directly; backpressure is a credit meter the app replenishes.
template <typename T, typename Policy>
class Inbox {
 public:
  Inbox(Context& ctx, std::uint64_t high_water, std::uint64_t max_credits,
        Overflow overflow = Overflow::Conflate)
      : relay_(ctx, BackpressurePolicy(ctx, BoundDim::Count, high_water, high_water / 2, overflow)),
        credits_(max_credits),
        max_credits_(max_credits) {}

  /// Whether the transport may deliver another message (a credit is available).
  /// When `false`, the transport must stop reading → the remote throttles.
  bool ready() const { return credits_ > 0; }

  /// Credits currently available to the remote.
  std::uint64_t credits() const { return credits_; }

  /// The transport delivers a received op. Consumes a credit; the caller MUST have
  /// checked `ready()` (a delivery without credit still applies but drives
  /// `credits` to zero, signalling the remote to stop).
  IngressOutcome receive(T op) {
    if (credits_ > 0) --credits_;
    return relay_.ingress(std::move(op));
  }

  /// The app consumes the coalesced window and replenishes `replenish` credits (up
  /// to the budget), re-opening the remote's flow.
  std::optional<T> consume(std::uint64_t replenish) {
    auto out = relay_.drain();
    credits_ = std::min(credits_ + replenish, max_credits_);
    return out;
  }

  RelayCell<T, Policy>& relay() { return relay_; }

 private:
  RelayCell<T, Policy> relay_;
  std::uint64_t credits_;
  std::uint64_t max_credits_;
};

// -- Phase 6: extra reactive policies ----------------------------------------
//
// Each policy is an optional reactive stage composed onto a relay egress; they
// only change where/when a relay flushes or which ops survive. Time is a logical
// clock (a monotone tick) — a binding drives tick/advance from its own runtime.

/// Case 9 — rate-limited egress (token bucket). A drain is permitted only when a
/// token is available. Refilled `refill_per_tick` tokens per logical tick, capped
/// at `capacity`.
class RatePolicy {
 public:
  RatePolicy(std::uint64_t capacity, std::uint64_t refill_per_tick)
      : capacity_(capacity), tokens_(capacity), refill_per_tick_(refill_per_tick) {}

  std::uint64_t tokens() const { return tokens_; }

  /// Try to consume one token for an egress; returns `true` if paced through.
  bool try_egress() {
    if (tokens_ > 0) {
      --tokens_;
      return true;
    }
    return false;
  }

  /// Advance the logical clock, refilling the bucket (saturating at capacity).
  void tick() { tokens_ = std::min(tokens_ + refill_per_tick_, capacity_); }

 private:
  std::uint64_t capacity_;
  std::uint64_t tokens_;
  std::uint64_t refill_per_tick_;
};

/// Case 8 — time-windowed coalescence (debounce/throttle). Flushes when it reaches
/// `window_ops` ops or on an explicit `tick`. A window is just a flush group, so
/// associativity keeps the converged state unchanged (flushGroupingIrrelevant).
class WindowPolicy {
 public:
  explicit WindowPolicy(std::uint64_t window_ops)
      : window_ops_(window_ops < 1 ? 1 : window_ops) {}

  /// Record one ingress; returns `true` when the window is full and should flush.
  bool on_ingress() {
    ++pending_;
    if (pending_ >= window_ops_) {
      pending_ = 0;
      return true;
    }
    return false;
  }

  /// The debounce/throttle interval elapsed: flush whatever is pending.
  bool tick() {
    if (pending_ > 0) {
      pending_ = 0;
      return true;
    }
    return false;
  }

 private:
  std::uint64_t window_ops_;
  std::uint64_t pending_ = 0;
};

/// Case 10 — TTL / deadline expiry. Drops elements whose age exceeds `ttl` against
/// a logical clock. Lossy-by-age (explicit); used to shed cold data.
class ExpiryPolicy {
 public:
  explicit ExpiryPolicy(std::uint64_t ttl) : ttl_(ttl) {}

  void advance(std::uint64_t by) { now_ += by; }
  std::uint64_t now() const { return now_; }

  /// Whether an element stamped at `stamped_at` is still live (not expired).
  bool is_live(std::uint64_t stamped_at) const {
    std::uint64_t age = now_ >= stamped_at ? now_ - stamped_at : 0;
    return age <= ttl_;
  }

  /// Retain only the live elements of a timestamped batch (drop the aged tail).
  template <typename T>
  std::vector<T> retain_live(const std::vector<std::pair<std::uint64_t, T>>& batch) const {
    std::vector<T> out;
    for (const auto& e : batch)
      if (is_live(e.first)) out.push_back(e.second);
    return out;
  }

 private:
  std::uint64_t ttl_;
  std::uint64_t now_ = 0;
};

/// Case 11 — priority egress. Ingress carries a priority; egress pops the highest
/// priority first (not FIFO), FIFO within equal priority. Reordering, so sound for
/// a commutative merge downstream (reorder_adjacent).
template <typename T>
class PriorityStorage {
 public:
  void push(std::uint64_t priority, T value) {
    items_.push_back(Item{priority, seq_, std::move(value)});
    ++seq_;
  }

  /// Pop the highest-priority element (FIFO within equal priority).
  std::optional<T> pop() {
    if (items_.empty()) return std::nullopt;
    std::size_t best = 0;
    for (std::size_t i = 1; i < items_.size(); ++i) {
      const Item& a = items_[i];
      const Item& b = items_[best];
      if (a.priority > b.priority ||
          (a.priority == b.priority && a.seq < b.seq)) {
        best = i;
      }
    }
    T out = std::move(items_[best].value);
    items_.erase(items_.begin() + best);
    return out;
  }

  std::size_t size() const { return items_.size(); }
  bool is_empty() const { return items_.empty(); }

 private:
  struct Item {
    std::uint64_t priority;
    std::uint64_t seq;
    T value;
  };
  std::vector<Item> items_;
  std::uint64_t seq_ = 0;
};

/// Case 18 — keyed sharding. N independent relays keyed by `K`; an op routes to
/// its key's shard. Merging across shards requires a commutative merge. The
/// converged per-key state equals a single relay per key.
template <typename K, typename T, typename Policy>
class KeyedRelay {
 public:
  KeyedRelay(Context& ctx, std::uint64_t high_water, Overflow overflow)
      : ctx_(&ctx), high_water_(high_water), overflow_(overflow) {}

  /// Route `op` to `key`'s shard, creating the shard on first use.
  IngressOutcome ingress(const K& key, T op) {
    auto it = shards_.find(key);
    if (it == shards_.end()) {
      it = shards_
               .emplace(std::piecewise_construct, std::forward_as_tuple(key),
                        std::forward_as_tuple(
                            *ctx_, BackpressurePolicy(*ctx_, BoundDim::Count, high_water_,
                                                      high_water_ / 2, overflow_)))
               .first;
    }
    return it->second.ingress(std::move(op));
  }

  /// Drain a key's coalesced window.
  std::optional<T> drain(const K& key) {
    auto it = shards_.find(key);
    if (it == shards_.end()) return std::nullopt;
    return it->second.drain();
  }

  std::vector<K> keys() const {
    std::vector<K> out;
    out.reserve(shards_.size());
    for (const auto& kv : shards_) out.push_back(kv.first);
    return out;
  }

 private:
  Context* ctx_;
  std::uint64_t high_water_;
  Overflow overflow_;
  std::unordered_map<K, RelayCell<T, Policy>> shards_;
};

}  // namespace lazily

#endif  // LAZILY_RELAY_HPP
