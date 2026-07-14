#ifndef LAZILY_SERVICE_HPP
#define LAZILY_SERVICE_HPP

// Embedded-service plane (`#lzservice`).
//
// Port of `lazily-rs/src/service.rs` — see `lazily-spec/docs/service.md` and
// the formal model `lazily-formal/LazilyFormal/Service.lean`. The story for "an
// instance is also a host of services": `HealthCell` / `ReadinessCell` /
// `DiscoveryCell` / `ServiceRegistry`, each a pure compute **core** (an
// aggregation / keyed map / durable log) split from a reactive **cell**
// projecting the composed view onto a `CellHandle` so a reader invalidates only
// when the projection changes. Cross-language conformance fixtures live in
// `lazily-spec/conformance/service/{health,readiness,discovery,service_registry}.json`.

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <lazily/context.hpp>

namespace lazily {

// ===========================================================================
// Health
// ===========================================================================

// Composed health status (worst component dominates). Scoped enum — the
// built-in `==`/`!=` drive `set_cell` dedup on the projected `health` reader.
enum class Health {
  Healthy,
  Degraded,
  Unhealthy,
};

// Composed liveness-probe core. Each probe reports `up` and whether it is
// `critical`. Uses `std::map` so iteration order matches Rust's `BTreeMap`.
class HealthCore {
 public:
  HealthCore() = default;

  // Set/refresh a probe.
  void set(std::string name, bool up, bool critical) {
    probes_[std::move(name)] = std::make_pair(up, critical);
  }

  // The aggregate: Unhealthy if any critical probe is down, else Degraded if
  // any is down, else Healthy.
  Health health() const {
    for (const auto& kv : probes_) {
      if (kv.second.second && !kv.second.first) return Health::Unhealthy;
    }
    for (const auto& kv : probes_) {
      if (!kv.second.first) return Health::Degraded;
    }
    return Health::Healthy;
  }

 private:
  std::map<std::string, std::pair<bool, bool>> probes_;  // name -> (up, critical)
};

// Reactive health: projects the aggregate onto a `Cell` for `/health`.
class HealthCell {
 public:
  explicit HealthCell(Context& ctx) : health_(ctx.cell(Health::Healthy)) {}

  void set(Context& ctx, std::string name, bool up, bool critical) {
    core_.set(std::move(name), up, critical);
    refresh(ctx);
  }

  Health health() const { return core_.health(); }
  CellHandle<Health> health_cell() const { return health_; }

 private:
  void refresh(Context& ctx) { ctx.set_cell(health_, core_.health()); }

  HealthCore core_;
  CellHandle<Health> health_;
};

// ===========================================================================
// Readiness
// ===========================================================================

// Composed readiness-probe core: ready iff every condition holds.
class ReadinessCore {
 public:
  ReadinessCore() = default;

  void set(std::string name, bool ready) {
    conditions_[std::move(name)] = ready;
  }

  bool ready() const {
    for (const auto& kv : conditions_) {
      if (!kv.second) return false;
    }
    return true;
  }

 private:
  std::map<std::string, bool> conditions_;
};

// Reactive readiness: projects `ready` onto a `Cell` for `/ready`.
class ReadinessCell {
 public:
  explicit ReadinessCell(Context& ctx) : ready_(ctx.cell(true)) {}

  void set(Context& ctx, std::string name, bool ready) {
    core_.set(std::move(name), ready);
    refresh(ctx);
  }

  bool ready() const { return core_.ready(); }
  CellHandle<bool> ready_cell() const { return ready_; }

 private:
  void refresh(Context& ctx) { ctx.set_cell(ready_, core_.ready()); }

  ReadinessCore core_;
  CellHandle<bool> ready_;
};

// ===========================================================================
// Discovery
// ===========================================================================

// Service-discovery core: `service -> (endpoint, owner)`. A peer's departure
// (`evict`) removes its endpoints. Generic over the peer id type `P`.
template <typename P>
class DiscoveryCore {
 public:
  DiscoveryCore() = default;

  // `register` is a reserved word in C++; the op is spelled `register_`.
  void register_(std::string service, std::string endpoint, P peer) {
    entries_[std::move(service)] =
        std::make_pair(std::move(endpoint), std::move(peer));
  }

  void deregister(const std::string& service) { entries_.erase(service); }

  // Remove all endpoints owned by `peer` (membership loss).
  void evict(const P& peer) {
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (it->second.second == peer) {
        it = entries_.erase(it);
      } else {
        ++it;
      }
    }
  }

  std::optional<std::string> resolve(const std::string& service) const {
    auto it = entries_.find(service);
    if (it == entries_.end()) return std::nullopt;
    return it->second.first;
  }

  // The live `service -> endpoint` map.
  std::map<std::string, std::string> discovery() const {
    std::map<std::string, std::string> out;
    for (const auto& kv : entries_) out[kv.first] = kv.second.first;
    return out;
  }

 private:
  std::map<std::string, std::pair<std::string, P>> entries_;
};

// Reactive service discovery.
template <typename P>
class DiscoveryCell {
 public:
  explicit DiscoveryCell(Context& ctx)
      : discovery_(ctx.cell(std::map<std::string, std::string>{})) {}

  void register_(Context& ctx, std::string service, std::string endpoint,
                 P peer) {
    core_.register_(std::move(service), std::move(endpoint), std::move(peer));
    refresh(ctx);
  }

  void deregister(Context& ctx, const std::string& service) {
    core_.deregister(service);
    refresh(ctx);
  }

  void evict(Context& ctx, const P& peer) {
    core_.evict(peer);
    refresh(ctx);
  }

  std::optional<std::string> resolve(const std::string& service) const {
    return core_.resolve(service);
  }

  std::map<std::string, std::string> discovery(Context& ctx) const {
    return ctx.get_cell(discovery_);
  }

  CellHandle<std::map<std::string, std::string>> discovery_cell() const {
    return discovery_;
  }

 private:
  void refresh(Context& ctx) { ctx.set_cell(discovery_, core_.discovery()); }

  DiscoveryCore<P> core_;
  CellHandle<std::map<std::string, std::string>> discovery_;
};

// ===========================================================================
// Service registry (durable)
// ===========================================================================

// A durable registry op (the ordered log entry).
struct RegistryOp {
  enum class Kind {
    Register,
    Deregister,
  };

  Kind kind;
  std::string service;
  std::string endpoint;  // empty for Deregister

  bool operator==(const RegistryOp& o) const {
    return kind == o.kind && service == o.service && endpoint == o.endpoint;
  }
  bool operator!=(const RegistryOp& o) const { return !(*this == o); }
};

// Durable service-registry core: an ordered log (the `DurableOutbox` pattern)
// whose left-fold is the projection, so replay reconstructs it.
class ServiceRegistryCore {
 public:
  ServiceRegistryCore() = default;

  void register_(std::string service, std::string endpoint) {
    RegistryOp op{RegistryOp::Kind::Register, std::move(service),
                  std::move(endpoint)};
    apply(projection_, op);
    log_.push_back(std::move(op));
  }

  void deregister(std::string service) {
    RegistryOp op{RegistryOp::Kind::Deregister, std::move(service),
                  std::string{}};
    apply(projection_, op);
    log_.push_back(std::move(op));
  }

  // Rebuild the projection from the durable log (restart / crash-replay).
  void replay() {
    std::map<std::string, std::string> projection;
    for (const auto& op : log_) apply(projection, op);
    projection_ = std::move(projection);
  }

  std::map<std::string, std::string> projection() const { return projection_; }
  const std::vector<RegistryOp>& log() const { return log_; }

 private:
  static void apply(std::map<std::string, std::string>& projection,
                    const RegistryOp& op) {
    if (op.kind == RegistryOp::Kind::Register) {
      projection[op.service] = op.endpoint;
    } else {
      projection.erase(op.service);
    }
  }

  std::vector<RegistryOp> log_;
  std::map<std::string, std::string> projection_;
};

// Reactive durable service registry.
class ServiceRegistry {
 public:
  explicit ServiceRegistry(Context& ctx)
      : projection_(ctx.cell(std::map<std::string, std::string>{})) {}

  void register_(Context& ctx, std::string service, std::string endpoint) {
    core_.register_(std::move(service), std::move(endpoint));
    refresh(ctx);
  }

  void deregister(Context& ctx, std::string service) {
    core_.deregister(std::move(service));
    refresh(ctx);
  }

  void replay(Context& ctx) {
    core_.replay();
    refresh(ctx);
  }

  std::map<std::string, std::string> projection(Context& ctx) const {
    return ctx.get_cell(projection_);
  }

  CellHandle<std::map<std::string, std::string>> projection_cell() const {
    return projection_;
  }

 private:
  void refresh(Context& ctx) { ctx.set_cell(projection_, core_.projection()); }

  ServiceRegistryCore core_;
  CellHandle<std::map<std::string, std::string>> projection_;
};

}  // namespace lazily

#endif  // LAZILY_SERVICE_HPP
