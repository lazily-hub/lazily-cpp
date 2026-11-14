#ifndef LAZILY_EGRESS_HPP
#define LAZILY_EGRESS_HPP

// Reactive egress shells (`#lzegress`).
//
// Reference implementation: lazily-rs `src/{egress,thread_safe_egress,
// async_egress}.rs`.
//
// ## The core / shell split
//
//   EgressCore<T>               delivery authority (egress_core.hpp). No graph,
//                               no I/O, no interior mutability.
//     BasicEgressCell<Cx, T>    the flavor-neutral reactive shell
//       EgressCell              single-threaded (Context)
//       ThreadSafeEgressCell    Send + Sync (ThreadSafeContext)
//       AsyncEgressCell         async (AsyncContext)
//
// The four delivery projections -- `pending`, `inflight`, `acked_through`,
// `retry` -- are memoized `Computed`s over the core. Every op takes the core
// mutex for the transition, RELEASES it, and only then clears exactly the reader
// kinds the core reported dirty, inside one batch. A reader's compute takes the
// graph first and the core mutex second, so an op that invalidated while holding
// the core mutex would invert the order.
//
// ## Transport attachment
//
// Transport I/O lives in exactly ONE Effect per attachment. The attachment
// captures the producer generation at attach time; a `reconnect` does not touch
// it, so the old attachment becomes inert at the core's generation fence and the
// caller attaches one replacement for the new incarnation. `inflight` is a
// dependency of the attachment even though the core remains the authority: an
// acknowledgement reopens the send window and must reactively re-run the same
// attachment, with no request/ack handshake.
//
// Single-threaded and thread-safe attachments claim and send inside the Effect.
// The async attachment keeps the DEPENDENCY TRACKING synchronous (a graph Effect
// over the same two projections) and runs only the claim/send loop on the
// attachment's own worker thread -- the delivery-state readers stay ordinary
// synchronous Computeds, exactly like the other two flavors, and only the
// transport side is async-coloured.
//
// ## NO observers -- only derives
//
// There is no listener list or subscription set anywhere below. The attachment
// is a graph Effect; everything else is a Computed.
//
// Wasm tier: threaded (pulls `thread_safe.hpp` and `async_context.hpp`). The
// core itself is `egress_core.hpp`, which is core-tier.

#include <lazily/async_context.hpp>
#include <lazily/cell.hpp>
#include <lazily/context.hpp>
#include <lazily/egress_core.hpp>
#include <lazily/thread_safe.hpp>

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace lazily {

/// The only transport seam egress owns. Durability and framing remain separate
/// adapters around the envelope.
///
/// `send` returns `true` when the transport ACCEPTED the attempt. Domain
/// delivery is proven later, by `ack`.
template <typename T> struct EgressTransport {
  virtual ~EgressTransport() = default;
  virtual bool send(const EgressEnvelope<T>& envelope) = 0;
};

namespace egress_detail {

/// Per-flavor graph access. The single-threaded and thread-safe contexts already
/// serialize their own graph; the async context's graph is a plain `Context`
/// that the async attachment's worker also writes, so every async access goes
/// through `AsyncContext::batch`, which holds the context's mutex.
template <typename Cx> struct Graph;

template <> struct Graph<Context> {
  template <typename V, typename F> static Computed<V> mint(Context& ctx, F&& compute) {
    return ctx.template computed<V>(std::forward<F>(compute));
  }
  template <typename V> static V read(Context& ctx, const Computed<V>& handle) {
    return ctx.get(handle);
  }
  template <typename V> static bool is_valid(Context& ctx, const Computed<V>& handle) {
    return ctx.is_set(handle);
  }
  static void clear(Context& ctx, const std::vector<SlotId>& roots) {
    ctx.batch([&](Context& g) {
      for (const SlotId id : roots)
        g.clear_slot(id);
    });
  }
};

template <> struct Graph<ThreadSafeContext> {
  template <typename V, typename F> static Computed<V> mint(ThreadSafeContext& ctx, F&& compute) {
    return ctx.template computed<V>(std::forward<F>(compute));
  }
  template <typename V> static V read(ThreadSafeContext& ctx, const Computed<V>& handle) {
    return ctx.get(handle);
  }
  template <typename V> static bool is_valid(ThreadSafeContext& ctx, const Computed<V>& handle) {
    return ctx.is_set(handle);
  }
  static void clear(ThreadSafeContext& ctx, const std::vector<SlotId>& roots) {
    ctx.batch([&](Context& g) {
      for (const SlotId id : roots)
        g.clear_slot(id);
    });
  }
};

template <> struct Graph<AsyncContext> {
  template <typename V, typename F> static Computed<V> mint(AsyncContext& ctx, F&& compute) {
    std::optional<Computed<V>> out;
    ctx.batch([&] { out = ctx.context().template computed<V>(std::forward<F>(compute)); });
    return *out;
  }
  template <typename V> static V read(AsyncContext& ctx, const Computed<V>& handle) {
    std::optional<V> out;
    ctx.batch([&] { out = ctx.context().get(handle); });
    return std::move(*out);
  }
  template <typename V> static bool is_valid(AsyncContext& ctx, const Computed<V>& handle) {
    bool out = false;
    ctx.batch([&] { out = ctx.context().is_set(handle); });
    return out;
  }
  static void clear(AsyncContext& ctx, const std::vector<SlotId>& roots) {
    ctx.batch([&] {
      for (const SlotId id : roots)
        ctx.context().clear_slot(id);
    });
  }
};

/// `false` when the reader's cache is invalid -- the probe the conformance corpus
/// asserts `invalidates` through, in both directions.
template <typename Cx, typename V> bool is_valid(Cx& ctx, const Computed<V>& handle) {
  return Graph<Cx>::is_valid(ctx, handle);
}

template <typename T> struct Readers {
  Computed<std::vector<EgressEnvelope<T>>> pending;
  Computed<std::vector<EgressEnvelope<T>>> inflight;
  Computed<std::optional<std::uint64_t>> acked_through;
  Computed<std::optional<EgressRetry>> retry;
};

template <typename T> struct EgressCellInner {
  /// Guards the core. NEVER held across a graph write.
  mutable std::mutex core_mutex;
  EgressCore<T> core;
  Readers<T> readers;

  EgressCellInner(std::uint64_t generation, EgressPolicy policy) : core(generation, policy) {}
};

} // namespace egress_detail

/// The flavor-neutral reactive egress shell.
template <typename OwnerContext, typename T> class BasicEgressCell {
  using Graph = egress_detail::Graph<OwnerContext>;
  using Inner = egress_detail::EgressCellInner<T>;

public:
  using value_type = T;
  using context_type = OwnerContext;

  /// Throws `EgressConfigException` for a policy that bounds nothing.
  BasicEgressCell(OwnerContext& ctx, std::uint64_t generation, EgressPolicy policy)
      : inner_(std::make_shared<Inner>(generation, policy)) {
    const std::shared_ptr<Inner> inner = inner_;
    inner_->readers.pending =
        Graph::template mint<std::vector<EgressEnvelope<T>>>(ctx, [inner](Compute&) {
          std::lock_guard<std::mutex> lock(inner->core_mutex);
          return inner->core.pending();
        });
    inner_->readers.inflight =
        Graph::template mint<std::vector<EgressEnvelope<T>>>(ctx, [inner](Compute&) {
          std::lock_guard<std::mutex> lock(inner->core_mutex);
          return inner->core.inflight();
        });
    inner_->readers.acked_through =
        Graph::template mint<std::optional<std::uint64_t>>(ctx, [inner](Compute&) {
          std::lock_guard<std::mutex> lock(inner->core_mutex);
          return inner->core.acked_through();
        });
    inner_->readers.retry =
        Graph::template mint<std::optional<EgressRetry>>(ctx, [inner](Compute&) {
          std::lock_guard<std::mutex> lock(inner->core_mutex);
          return inner->core.retry();
        });
  }

  std::uint64_t generation() const {
    std::lock_guard<std::mutex> lock(inner_->core_mutex);
    return inner_->core.generation();
  }

  std::uint64_t next_sequence() const {
    std::lock_guard<std::mutex> lock(inner_->core_mutex);
    return inner_->core.next_sequence();
  }

  // -- Mutating ops: transition under the core mutex, then invalidate. --

  std::uint64_t enqueue(OwnerContext& ctx, T payload) {
    return transition(ctx, [&](EgressCore<T>& core) { return core.enqueue(std::move(payload)); });
  }

  EgressClaim<T> claim(OwnerContext& ctx, std::uint64_t generation) {
    return transition(ctx, [&](EgressCore<T>& core) { return core.claim(generation); });
  }

  EgressAck ack(OwnerContext& ctx, std::uint64_t generation, std::uint64_t through) {
    return transition(ctx, [&](EgressCore<T>& core) { return core.ack(generation, through); });
  }

  EgressFailure fail(OwnerContext& ctx, std::uint64_t generation, std::uint64_t sequence) {
    return transition(ctx, [&](EgressCore<T>& core) { return core.fail(generation, sequence); });
  }

  EgressRetryAction retry_now(OwnerContext& ctx, std::uint64_t generation, std::uint64_t sequence) {
    return transition(ctx,
                      [&](EgressCore<T>& core) { return core.retry_now(generation, sequence); });
  }

  EgressReconnect reconnect(OwnerContext& ctx, std::uint64_t generation) {
    return transition(ctx, [&](EgressCore<T>& core) { return core.reconnect(generation); });
  }

  // -- Reactive reads. --

  std::vector<EgressEnvelope<T>> pending(OwnerContext& ctx) const {
    return Graph::read(ctx, inner_->readers.pending);
  }
  std::vector<EgressEnvelope<T>> inflight(OwnerContext& ctx) const {
    return Graph::read(ctx, inner_->readers.inflight);
  }
  std::optional<std::uint64_t> acked_through(OwnerContext& ctx) const {
    return Graph::read(ctx, inner_->readers.acked_through);
  }
  std::optional<EgressRetry> retry(OwnerContext& ctx) const {
    return Graph::read(ctx, inner_->readers.retry);
  }

  Computed<std::vector<EgressEnvelope<T>>> pending_handle() const {
    return inner_->readers.pending;
  }
  Computed<std::vector<EgressEnvelope<T>>> inflight_handle() const {
    return inner_->readers.inflight;
  }
  Computed<std::optional<std::uint64_t>> acked_through_handle() const {
    return inner_->readers.acked_through;
  }
  Computed<std::optional<EgressRetry>> retry_handle() const { return inner_->readers.retry; }

protected:
  /// Claim and send until the window, the queue, or the transport says stop.
  /// Shared by every flavor's attachment.
  template <typename Tr>
  static void deliver(BasicEgressCell cell, OwnerContext& ctx, std::uint64_t generation,
                      Tr& transport) {
    for (;;) {
      EgressClaim<T> claimed = cell.claim(ctx, generation);
      if (claimed.kind != EgressClaimKind::Claimed) break;
      if (!transport.send(*claimed.envelope)) {
        cell.fail(ctx, generation, claimed.envelope->sequence);
        break;
      }
    }
  }

  /// Apply one core-reported invalidation set inside ONE batch, so no reader
  /// observes a partial fan-out.
  void apply(OwnerContext& ctx, const EgressChange& change) const {
    if (change.empty()) return;
    std::vector<SlotId> roots;
    if (change.pending) roots.push_back(inner_->readers.pending.id());
    if (change.inflight) roots.push_back(inner_->readers.inflight.id());
    if (change.acked_through) roots.push_back(inner_->readers.acked_through.id());
    if (change.retry) roots.push_back(inner_->readers.retry.id());
    Graph::clear(ctx, roots);
  }

private:
  template <typename Op> auto transition(OwnerContext& ctx, Op&& op) {
    std::optional<decltype(op(inner_->core))> result;
    {
      std::lock_guard<std::mutex> lock(inner_->core_mutex);
      result.emplace(op(inner_->core));
    }
    apply(ctx, result->first);
    return std::move(result->second);
  }

  std::shared_ptr<Inner> inner_;
};

/// The single-threaded flavor.
template <typename T> class EgressCell : public BasicEgressCell<Context, T> {
  using Base = BasicEgressCell<Context, T>;

public:
  EgressCell(Context& ctx, std::uint64_t generation, EgressPolicy policy = EgressPolicy{})
      : Base(ctx, generation, policy) {}

  /// Attach ONE transport Effect at the current producer generation. `ctx` must
  /// outlive the returned Effect (it owns it); dispose it with
  /// `ctx.dispose_effect`.
  template <typename Tr> Effect attach_transport(Context& ctx, std::shared_ptr<Tr> transport) {
    const std::uint64_t generation = this->generation();
    const auto pending = this->pending_handle();
    const auto inflight = this->inflight_handle();
    const Base cell = *this;
    Context* owner = &ctx;
    return ctx.effect_void([cell, owner, transport, generation, pending, inflight](Compute& c) {
      (void)c.get(inflight);
      if (c.get(pending).empty()) return;
      Base::deliver(cell, *owner, generation, *transport);
    });
  }
};

/// The `Send + Sync` flavor. Delivery authority stays in the shared core; the
/// transport is serialized by its own mutex.
template <typename T> class ThreadSafeEgressCell : public BasicEgressCell<ThreadSafeContext, T> {
  using Base = BasicEgressCell<ThreadSafeContext, T>;

public:
  ThreadSafeEgressCell(ThreadSafeContext& ctx, std::uint64_t generation,
                       EgressPolicy policy = EgressPolicy{})
      : Base(ctx, generation, policy) {}

  template <typename Tr>
  Effect attach_transport(ThreadSafeContext& ctx, std::shared_ptr<Tr> transport) {
    const std::uint64_t generation = this->generation();
    const auto pending = this->pending_handle();
    const auto inflight = this->inflight_handle();
    const Base cell = *this;
    ThreadSafeContext* owner = &ctx;
    const auto transport_mutex = std::make_shared<std::mutex>();
    return ctx.effect_void(
        [cell, owner, transport, transport_mutex, generation, pending, inflight](Compute& c) {
          (void)c.get(inflight);
          if (c.get(pending).empty()) return;
          // Serialize sends per attachment; the graph lock is re-entrant for
          // this thread, so the claim inside `deliver` does not deadlock.
          std::lock_guard<std::mutex> lock(*transport_mutex);
          Base::deliver(cell, *owner, generation, *transport);
        });
  }
};

/// One async transport attachment: a synchronous dependency-tracking graph
/// Effect plus the worker that runs the claim/send loop. Move-only; destroying
/// it disposes the tracker and joins the worker. It must be destroyed BEFORE
/// the `AsyncContext` it was attached to, and not from inside one of that
/// context's batches.
class AsyncEgressAttachment {
public:
  AsyncEgressAttachment() = default;
  AsyncEgressAttachment(AsyncEgressAttachment&&) = default;
  AsyncEgressAttachment& operator=(AsyncEgressAttachment&& other) noexcept {
    if (this != &other) {
      reset();
      state_ = std::move(other.state_);
    }
    return *this;
  }
  AsyncEgressAttachment(const AsyncEgressAttachment&) = delete;
  AsyncEgressAttachment& operator=(const AsyncEgressAttachment&) = delete;
  ~AsyncEgressAttachment() { reset(); }

  /// The tracker Effect on the async context's graph.
  Effect tracker() const { return state_ ? state_->tracker : Effect{}; }

  /// How many delivery passes the worker has completed. Diagnostic only.
  std::uint64_t deliveries() const {
    if (!state_) return 0;
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->completed;
  }

  /// Dispose the tracker and join the worker. Idempotent.
  void reset() {
    if (!state_) return;
    const std::shared_ptr<State> state = std::move(state_);
    state->ctx->batch([&] { state->ctx->context().dispose_effect(state->tracker); });
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->stop = true;
    }
    state->wake.notify_all();
    if (state->worker.joinable()) state->worker.join();
  }

private:
  template <typename> friend class AsyncEgressCell;

  struct State {
    AsyncContext* ctx = nullptr;
    Effect tracker;
    mutable std::mutex mutex;
    std::condition_variable wake;
    bool requested = false;
    bool stop = false;
    std::uint64_t completed = 0;
    std::thread worker;

    void request() {
      {
        std::lock_guard<std::mutex> lock(mutex);
        requested = true;
      }
      wake.notify_all();
    }
  };

  std::shared_ptr<State> state_;
};

/// The async flavor. Delivery-state readers are synchronous Computeds on the
/// `AsyncContext`'s graph; only the transport attachment is async-coloured.
template <typename T> class AsyncEgressCell : public BasicEgressCell<AsyncContext, T> {
  using Base = BasicEgressCell<AsyncContext, T>;

public:
  AsyncEgressCell(AsyncContext& ctx, std::uint64_t generation, EgressPolicy policy = EgressPolicy{})
      : Base(ctx, generation, policy) {}

  template <typename Tr>
  AsyncEgressAttachment attach_transport(AsyncContext& ctx, std::shared_ptr<Tr> transport) {
    using State = AsyncEgressAttachment::State;
    const std::uint64_t generation = this->generation();
    const auto pending = this->pending_handle();
    const auto inflight = this->inflight_handle();
    const Base cell = *this;

    auto state = std::make_shared<State>();
    state->ctx = &ctx;
    // The worker holds the state weakly-by-raw-pointer: the attachment joins it
    // before the state can go away.
    State* raw = state.get();
    state->worker = std::thread([raw, cell, transport, generation]() {
      std::unique_lock<std::mutex> lock(raw->mutex);
      for (;;) {
        raw->wake.wait(lock, [raw] { return raw->requested || raw->stop; });
        if (raw->stop) return;
        raw->requested = false;
        lock.unlock();
        Base::deliver(cell, *raw->ctx, generation, *transport);
        lock.lock();
        ++raw->completed;
      }
    });

    // Dependency tracking stays synchronous: the tracker reads both projections
    // through its compute view and only wakes the worker.
    ctx.batch([&] {
      state->tracker = ctx.context().effect_void([raw, pending, inflight](Compute& c) {
        (void)c.get(inflight);
        if (!c.get(pending).empty()) raw->request();
      });
    });

    AsyncEgressAttachment attachment;
    attachment.state_ = std::move(state);
    return attachment;
  }
};

} // namespace lazily

#endif // LAZILY_EGRESS_HPP
