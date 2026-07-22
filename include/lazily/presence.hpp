#ifndef LAZILY_PRESENCE_HPP
#define LAZILY_PRESENCE_HPP

// Presence + ephemeral plane (`#lzpresence`).
//
// Port of `lazily-rs/src/presence.rs` (see `lazily-spec/docs/presence.md` and
// the formal model `lazily-formal/LazilyFormal/Presence.lean`). The CRDT plane
// is durable; collaborative apps also need an **ephemeral** plane that does not
// persist (live cursors, typing indicators, presence). Each primitive is a pure
// compute **core** (a keyed map / single value + TTL over a monotone logical
// clock) split from a reactive **cell** projecting the live view onto a
// `Source` (invalidates only on a live-view change).
//
// The ephemeral plane is distinct from the durable plane: the `Ephemeral`
// marker tags values that MUST NOT be persisted, and a durable sink is generic
// over `Durable`, so handing it an ephemeral value fails to compile
// (`durable_persist` below static_asserts `!is_ephemeral_v`, mirroring the Rust
// `compile_fail` doctest).
//
// Conformance fixtures: `lazily-spec/conformance/presence/*.json`
// (awareness.json / ephemeral.json / presence.json).

#include <cstdint>
#include <map>
#include <optional>
#include <type_traits>
#include <utility>

#include <lazily/context.hpp>
#include <lazily/cell.hpp>

namespace lazily {

// ===========================================================================
// Plane markers
// ===========================================================================

/// Marker tag: a value on the **ephemeral** plane. MUST NOT be persisted.
struct Ephemeral {};

/// Marker tag: a value that may be written to the durable outbox.
struct Durable {};

/// Trait: does `T` witness the `Ephemeral` marker? Types opt in by declaring
/// `using plane_marker = Ephemeral;`.
template <typename T, typename = void>
struct is_ephemeral : std::false_type {};

template <typename T>
struct is_ephemeral<T, std::void_t<typename T::plane_marker>>
    : std::is_same<typename T::plane_marker, Ephemeral> {};

template <typename T>
inline constexpr bool is_ephemeral_v = is_ephemeral<T>::value;

/// A newtype witnessing the `Ephemeral` marker (used by the compile-time
/// rejection demo and by ephemeral payloads).
template <typename T>
struct EphemeralValue {
  using plane_marker = Ephemeral;
  T value;
};

/// A durable sink: statically rejects ephemeral values. Mirrors the Rust
/// `fn persist<T: Durable>(_v: T)` — handing it an ephemeral value fails to
/// compile.
template <typename T>
void durable_persist(const T&) {
  static_assert(!is_ephemeral_v<T>,
                "lazily: cannot persist a value on the ephemeral plane");
}

static_assert(is_ephemeral_v<EphemeralValue<int>>,
              "EphemeralValue witnesses the Ephemeral marker");
static_assert(!is_ephemeral_v<int>, "plain values are not ephemeral");

// ===========================================================================
// Ephemeral single value
// ===========================================================================

/// Single-value auto-expiry compute core — "the last value seen in window N".
template <typename T>
class EphemeralCore {
 public:
  using plane_marker = Ephemeral;

  EphemeralCore() = default;

  /// Set the value, expiring at `now + ttl`.
  void set(T value, uint64_t now, uint64_t ttl) {
    value_ = std::move(value);
    expiry_ = now + ttl;
  }

  /// Clear the value once `now >= expiry`.
  void tick(uint64_t now) {
    if (value_ && now >= expiry_) value_.reset();
  }

  const std::optional<T>& value() const { return value_; }

 private:
  std::optional<T> value_;
  uint64_t expiry_ = 0;
};

/// Reactive single-value ephemeral cell.
template <typename T>
class EphemeralCell {
 public:
  using plane_marker = Ephemeral;

  explicit EphemeralCell(Context& ctx)
      : value_(ctx.source(std::optional<T>{})) {}

  void set(Context& ctx, T value, uint64_t now, uint64_t ttl) {
    core_.set(std::move(value), now, ttl);
    refresh(ctx);
  }

  void tick(Context& ctx, uint64_t now) {
    core_.tick(now);
    refresh(ctx);
  }

  std::optional<T> value(Context& ctx) { return value_.get(ctx); }

  Source<std::optional<T>> value_cell() const { return value_; }

 private:
  void refresh(Context& ctx) { value_.set(ctx, core_.value()); }

  EphemeralCore<T> core_;
  Source<std::optional<T>> value_;
};

// ===========================================================================
// Keyed per-peer ephemeral map (shared by presence + awareness)
// ===========================================================================

/// Per-key ephemeral map with TTL eviction — the shared core behind presence
/// and awareness. Each entry carries an expiry; `tick` evicts lapsed entries.
/// `std::map` (ordered) mirrors the Rust `BTreeMap`.
template <typename K, typename V>
class EphemeralMapCore {
 public:
  using plane_marker = Ephemeral;

  EphemeralMapCore() = default;

  /// Set/refresh `key`'s value (last-writer wins), expiring at `now + ttl`.
  void set(K key, V value, uint64_t now, uint64_t ttl) {
    entries_[std::move(key)] = std::make_pair(std::move(value), now + ttl);
  }

  /// Drop `key` immediately (membership Dead/Left).
  void evict(const K& key) { entries_.erase(key); }

  /// Evict entries whose TTL has lapsed (`now >= expiry`).
  void tick(uint64_t now) {
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (now >= it->second.second)
        it = entries_.erase(it);
      else
        ++it;
    }
  }

  /// The live value for `key` (respecting `now`).
  std::optional<V> get(const K& key, uint64_t now) const {
    auto it = entries_.find(key);
    if (it == entries_.end() || now >= it->second.second) return std::nullopt;
    return it->second.first;
  }

  /// The live key -> value map at `now`.
  std::map<K, V> present(uint64_t now) const {
    std::map<K, V> out;
    for (const auto& kv : entries_) {
      if (now < kv.second.second) out.emplace(kv.first, kv.second.first);
    }
    return out;
  }

 private:
  std::map<K, std::pair<V, uint64_t>> entries_;
};

/// Reactive per-peer presence: heartbeat-kept, membership- and TTL-evicted.
template <typename K, typename V>
class PresenceCell {
 public:
  using plane_marker = Ephemeral;

  PresenceCell(Context& ctx, uint64_t ttl)
      : present_(ctx.source(std::map<K, V>{})), ttl_(ttl) {}

  /// Heartbeat a peer's presence (expiring at `now + ttl`).
  void heartbeat(Context& ctx, K peer, V value, uint64_t now) {
    core_.set(std::move(peer), std::move(value), now, ttl_);
    refresh(ctx, now);
  }

  /// Evict a peer on membership loss.
  void evict(Context& ctx, const K& peer, uint64_t now) {
    core_.evict(peer);
    refresh(ctx, now);
  }

  void tick(Context& ctx, uint64_t now) {
    core_.tick(now);
    refresh(ctx, now);
  }

  std::map<K, V> present(Context& ctx) { return present_.get(ctx); }

  Source<std::map<K, V>> present_cell() const { return present_; }

 private:
  void refresh(Context& ctx, uint64_t now) {
    present_.set(ctx, core_.present(now));
  }

  EphemeralMapCore<K, V> core_;
  Source<std::map<K, V>> present_;
  uint64_t ttl_;
};

/// Reactive typed ephemeral broadcast (cursors / selections): last-writer-per-
/// peer with a TTL.
template <typename K, typename V>
class AwarenessCell {
 public:
  using plane_marker = Ephemeral;

  AwarenessCell(Context& ctx, uint64_t ttl)
      : present_(ctx.source(std::map<K, V>{})), ttl_(ttl) {}

  /// Set a peer's awareness value (last-writer wins, no merge).
  void set(Context& ctx, K peer, V value, uint64_t now) {
    core_.set(std::move(peer), std::move(value), now, ttl_);
    refresh(ctx, now);
  }

  void tick(Context& ctx, uint64_t now) {
    core_.tick(now);
    refresh(ctx, now);
  }

  std::optional<V> get(const K& peer, uint64_t now) const {
    return core_.get(peer, now);
  }

  std::map<K, V> present(Context& ctx) { return present_.get(ctx); }

  Source<std::map<K, V>> present_cell() const { return present_; }

 private:
  void refresh(Context& ctx, uint64_t now) {
    present_.set(ctx, core_.present(now));
  }

  EphemeralMapCore<K, V> core_;
  Source<std::map<K, V>> present_;
  uint64_t ttl_;
};

}  // namespace lazily

#endif  // LAZILY_PRESENCE_HPP
