#ifndef LAZILY_COMMAND_HPP
#define LAZILY_COMMAND_HPP

#include <lazily/ipc.hpp>
#include <lazily/receipt.hpp>
#include <lazily/types.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace lazily {

// -- Command plane (command-plane-v1) --

enum class DedupePolicy { None, SameIdempotencyKey, SameCommandId };

struct CommandPolicy {
  DedupePolicy dedupe = DedupePolicy::None;
  bool supersede = false;
  bool cancel_on_preempt = false;
};

enum class CommandStatus {
  Submitted, Accepted, Running,
  Applied, Rejected, Cancelled, Superseded, TimedOut
};

inline bool is_terminal_status(CommandStatus s) {
  return s == CommandStatus::Applied || s == CommandStatus::Rejected ||
         s == CommandStatus::Cancelled || s == CommandStatus::Superseded ||
         s == CommandStatus::TimedOut;
}

inline int phase_rank(CommandStatus s) {
  switch (s) {
    case CommandStatus::Submitted: return 0;
    case CommandStatus::Accepted: return 1;
    case CommandStatus::Running: return 2;
    default: return 3;  // terminal
  }
}

struct CommandSubmit {
  std::string command_id;
  std::string causation_id;
  std::string source;
  std::string target;
  std::string ns;
  std::string name;
  int64_t authority_generation;
  std::string idempotency_key;
  int64_t deadline_ms;
  CommandPolicy policy;
  std::string payload_type;
  std::string payload_hash;
  IpcValue payload;
  std::vector<std::string> required_features;
};

struct CommandCancel {
  std::string command_id;
  std::string causation_id;
  std::string source;
  int64_t authority_generation;
  std::optional<std::string> reason;
};

enum class CommandEventKind {
  Observed, Accepted, Started, Progress, Cancelled, Superseded, TimedOut
};

struct CommandEvent {
  std::string event_id;
  std::string command_id;
  CommandEventKind kind;
  int64_t generation;
  std::optional<std::string> detail;
};

struct CommandEvents {
  std::vector<CommandEvent> events;
};

struct CommandProjectionEntry {
  std::string command_id;
  CommandStatus status;
  bool terminal;
  int64_t generation;
  std::optional<std::string> reason;
  std::optional<std::string> terminal_receipt_id;
  std::optional<std::string> last_event_id;
};

struct CommandProjectionImage {
  int64_t generation;
  std::vector<CommandProjectionEntry> commands;
};

struct CommandMessageSubmit { CommandSubmit value; };
struct CommandMessageCancel { CommandCancel value; };
struct CommandMessageEvents { CommandEvents value; };
struct CommandMessageProjection { CommandProjectionImage value; };

using CommandMessage = std::variant<CommandMessageSubmit, CommandMessageCancel,
                                      CommandMessageEvents, CommandMessageProjection>;

struct CommandStatusRecorded {};
struct CommandStatusDuplicate {};
struct CommandStatusUnknown {};
struct CommandStatusStaleGeneration { int64_t expected; int64_t actual; };
struct CommandStatusTerminalConflict {
  std::string command_id;
  CommandStatus existing;
  CommandStatus incoming;
};

using CommandApplyStatus = std::variant<CommandStatusRecorded, CommandStatusDuplicate,
                                          CommandStatusUnknown, CommandStatusStaleGeneration,
                                          CommandStatusTerminalConflict>;

class CommandProjection {
 public:
  int64_t generation() const { return generation_; }

  CommandApplyStatus apply_message(const CommandMessage& msg) {
    if (std::holds_alternative<CommandMessageSubmit>(msg))
      return submit(std::get<CommandMessageSubmit>(msg).value);
    if (std::holds_alternative<CommandMessageCancel>(msg))
      return cancel(std::get<CommandMessageCancel>(msg).value);
    if (std::holds_alternative<CommandMessageEvents>(msg))
      return apply_events(std::get<CommandMessageEvents>(msg).value);
    if (std::holds_alternative<CommandMessageProjection>(msg))
      return apply_projection(std::get<CommandMessageProjection>(msg).value);
    return CommandStatusUnknown{};
  }

  CommandApplyStatus submit(const CommandSubmit& cmd) {
    if (entries_.count(cmd.command_id)) return CommandStatusDuplicate{};
    entries_[cmd.command_id] = {
      cmd.command_id, CommandStatus::Submitted, false, cmd.authority_generation,
      std::nullopt, std::nullopt, std::nullopt
    };
    return CommandStatusRecorded{};
  }

  CommandApplyStatus cancel(const CommandCancel& cmd) {
    auto it = entries_.find(cmd.command_id);
    if (it == entries_.end()) return CommandStatusUnknown{};
    if (is_terminal_status(it->second.status)) return CommandStatusRecorded{};
    if (cmd.authority_generation < it->second.generation)
      return CommandStatusStaleGeneration{it->second.generation, cmd.authority_generation};
    return CommandStatusRecorded{};
  }

  CommandApplyStatus apply_events(const CommandEvents& events) {
    for (auto& evt : events.events) {
      auto it = entries_.find(evt.command_id);
      if (it == entries_.end()) continue;
      if (seen_event_ids_.count(evt.event_id)) continue;
      seen_event_ids_.insert(evt.event_id);

      auto new_status = progress_status_of(evt.kind);
      if (new_status == CommandStatus::Submitted) continue;  // no-op
      if (phase_rank(new_status) >= phase_rank(it->second.status)) {
        it->second.status = new_status;
        it->second.last_event_id = evt.event_id;
      }
    }
    return CommandStatusRecorded{};
  }

  void observe_receipt(const CausalReceipt& receipt) {
    auto it = entries_.find(receipt.causation_id);
    if (it == entries_.end()) return;
    if (seen_receipt_ids_.count(receipt.receipt_id)) return;
    seen_receipt_ids_.insert(receipt.receipt_id);

    if (is_terminal(receipt.outcome)) {
      if (is_terminal_status(it->second.status) &&
          it->second.status != terminal_status_of(receipt.outcome, receipt.reason)) {
        conflicts_.insert(receipt.causation_id);
        return;
      }
      it->second.status = terminal_status_of(receipt.outcome, receipt.reason);
      it->second.terminal = true;
      it->second.terminal_receipt_id = receipt.receipt_id;
      it->second.reason = receipt.reason;
    }
  }

  CommandApplyStatus apply_projection(const CommandProjectionImage& image) {
    generation_ = image.generation;
    for (auto& entry : image.commands) {
      entries_[entry.command_id] = entry;
    }
    return CommandStatusRecorded{};
  }

  std::optional<CommandProjectionEntry> entry(const std::string& command_id) const {
    auto it = entries_.find(command_id);
    if (it == entries_.end()) return std::nullopt;
    return it->second;
  }

  std::optional<CommandProjectionEntry> terminal_for(const std::string& command_id) const {
    auto e = entry(command_id);
    if (e && e->terminal) return e;
    return std::nullopt;
  }

  bool has_conflict(const std::string& command_id) const {
    return conflicts_.count(command_id) > 0;
  }

  CommandProjectionImage to_image() const {
    std::vector<CommandProjectionEntry> cmds;
    for (auto& [_, e] : entries_) cmds.push_back(e);
    std::sort(cmds.begin(), cmds.end(),
              [](const auto& a, const auto& b) { return a.command_id < b.command_id; });
    return {generation_, std::move(cmds)};
  }

 private:
  int64_t generation_ = 0;
  std::unordered_map<std::string, CommandProjectionEntry> entries_;
  std::unordered_set<std::string> seen_event_ids_;
  std::unordered_set<std::string> seen_receipt_ids_;
  std::unordered_set<std::string> conflicts_;

  static CommandStatus progress_status_of(CommandEventKind kind) {
    switch (kind) {
      case CommandEventKind::Observed:
      case CommandEventKind::Accepted: return CommandStatus::Accepted;
      case CommandEventKind::Started:
      case CommandEventKind::Progress: return CommandStatus::Running;
      default: return CommandStatus::Submitted;  // UX-only kinds
    }
  }

  static CommandStatus terminal_status_of(ReceiptOutcome outcome,
                                            std::optional<std::string> reason) {
    if (outcome == ReceiptOutcome::Applied) return CommandStatus::Applied;
    if (outcome == ReceiptOutcome::Rejected) {
      if (reason) {
        if (*reason == "cancelled") return CommandStatus::Cancelled;
        if (*reason == "superseded") return CommandStatus::Superseded;
        if (*reason == "timed_out") return CommandStatus::TimedOut;
      }
      return CommandStatus::Rejected;
    }
    return CommandStatus::Rejected;
  }
};

// -- Distributed CRDT plane runtime --

class CrdtPlaneRuntime {
 public:
  explicit CrdtPlaneRuntime(PeerId peer) : peer_(peer) {}

  PeerId peer() const { return peer_; }

  int ingest_ops(const std::vector<CrdtOp>& ops) {
    int applied = 0;
    for (auto& op : ops) {
      // Dedup by (node, stamp)
      auto dedup_key = std::to_string(op.node) + ":" +
                        std::to_string(op.stamp.wall_time) + ":" +
                        std::to_string(op.stamp.logical) + ":" +
                        std::to_string(op.stamp.peer);
      if (log_.count(dedup_key)) continue;
      log_.insert(dedup_key);
      ops_log_.push_back(op);

      // Observe stamp in frontier
      HlcStamp stamp = from_wire(op.stamp);
      frontier_.observe(op.stamp.peer, stamp);
      membership_.insert(op.stamp.peer);

      // Resolve key↔node mapping
      if (op.key) {
        key_to_node_[op.key->to_wire()] = op.node;
        node_to_key_[op.node] = op.key->to_wire();
      }

      // Update winning op if stamp is greater
      auto it = winning_.find(op.node);
      if (it == winning_.end() || stamp > from_wire(it->second.stamp)) {
        winning_[op.node] = op;
        applied++;
      }
    }
    return applied;
  }

  int ingest(const CrdtSync& sync) {
    // Observe frontier stamps first
    for (auto& [p, s] : sync.frontier) {
      frontier_.observe(p, from_wire(s));
      membership_.insert(p);
    }
    return ingest_ops(sync.ops);
  }

  struct ConvergedEntry {
    NodeId node;
    std::optional<std::string> key;
    IpcValue state;
  };

  std::vector<ConvergedEntry> converged() const {
    std::vector<ConvergedEntry> result;
    for (auto& [node, op] : winning_) {
      std::optional<std::string> key;
      auto kit = node_to_key_.find(node);
      if (kit != node_to_key_.end()) key = kit->second;
      result.push_back({node, key, op.state});
    }
    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) { return a.node < b.node; });
    return result;
  }

  std::optional<CrdtOp> winning_op(NodeId node) const {
    auto it = winning_.find(node);
    if (it == winning_.end()) return std::nullopt;
    return it->second;
  }

  std::optional<IpcValue> value(NodeId node) const {
    auto it = winning_.find(node);
    if (it == winning_.end()) return std::nullopt;
    return it->second.state;
  }

  std::vector<NodeId> nodes() const {
    std::vector<NodeId> result;
    for (auto& [n, _] : winning_) result.push_back(n);
    std::sort(result.begin(), result.end());
    return result;
  }

  size_t size() const { return winning_.size(); }
  bool is_empty() const { return winning_.empty(); }

  std::vector<PeerId> membership() const {
    std::vector<PeerId> result(membership_.begin(), membership_.end());
    std::sort(result.begin(), result.end());
    return result;
  }

  std::vector<StampFrontierEntry> frontier_entries() const {
    std::vector<StampFrontierEntry> result;
    for (auto p : membership()) {
      auto s = frontier_.get(p);
      if (s) result.push_back({p, to_wire(*s)});
    }
    return result;
  }

  std::vector<CrdtOp> ops() const { return ops_log_; }

 private:
  PeerId peer_;
  StampFrontier frontier_;
  std::unordered_set<PeerId> membership_;
  std::unordered_map<NodeId, CrdtOp> winning_;
  std::unordered_set<std::string> log_;
  std::vector<CrdtOp> ops_log_;
  std::unordered_map<std::string, NodeId> key_to_node_;
  std::unordered_map<NodeId, std::string> node_to_key_;
};

// -- Instrumentation --

struct BenchmarkResult {
  std::string name;
  int iterations;
  int64_t total_micros;

  double avg_micros() const { return static_cast<double>(total_micros) / iterations; }
  double ops_per_second() const {
    return static_cast<double>(iterations) / (static_cast<double>(total_micros) / 1e6);
  }
};

inline BenchmarkResult benchmark(const std::string& name, std::function<void()> body, int iterations) {
  auto start = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < iterations; ++i) body();
  auto end = std::chrono::high_resolution_clock::now();
  auto micros = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
  return {name, iterations, static_cast<int64_t>(micros)};
}

inline constexpr int kDefaultBenchmarkIterations = 10000;

inline std::vector<BenchmarkResult> run_benchmark_suite(int iterations) {
  std::vector<BenchmarkResult> results;
  results.push_back(benchmark("Cell read/write", [&]() {
    Context ctx;
    auto c = ctx.cell(42);
    ctx.set_cell(c, 100);
    (void)ctx.get_cell(c);
  }, iterations));
  results.push_back(benchmark("Slot recompute", [&]() {
    Context ctx;
    auto a = ctx.cell(1);
    auto b = ctx.cell(2);
    auto s = ctx.computed<int>([&](Context& c) {
      return c.get_cell(a) + c.get_cell(b);
    });
    (void)ctx.get(s);
  }, iterations));
  results.push_back(benchmark("batch coalesce (10 cells)", [&]() {
    Context ctx;
    std::vector<CellHandle<int>> cells;
    for (int i = 0; i < 10; ++i) cells.push_back(ctx.cell(i));
    ctx.batch([&](Context& c) {
      for (int i = 0; i < 10; ++i) c.set_cell(cells[i], i + 100);
    });
  }, iterations));
  return results;
}

}  // namespace lazily

#endif  // LAZILY_COMMAND_HPP
