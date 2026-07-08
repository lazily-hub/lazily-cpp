#ifndef LAZILY_RECEIPT_HPP
#define LAZILY_RECEIPT_HPP

#include <lazily/ipc.hpp>
#include <lazily/types.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace lazily {

// -- Causal receipts --

enum class ReceiptOutcome { Observed, Accepted, Applied, Rejected };

inline bool is_terminal(ReceiptOutcome o) {
  return o == ReceiptOutcome::Applied || o == ReceiptOutcome::Rejected;
}

struct CausalReceipt {
  std::string receipt_id;
  std::string causation_id;
  std::string observer;
  int64_t generation;
  ReceiptOutcome outcome;
  std::optional<std::string> reason;
  std::optional<std::string> payload_hash;
};

inline CausalReceipt observed_receipt(std::string receipt_id, std::string causation_id,
                                       std::string observer, int64_t generation) {
  return {std::move(receipt_id), std::move(causation_id), std::move(observer),
          generation, ReceiptOutcome::Observed, std::nullopt, std::nullopt};
}

inline CausalReceipt accepted_receipt(std::string receipt_id, std::string causation_id,
                                       std::string observer, int64_t generation) {
  return {std::move(receipt_id), std::move(causation_id), std::move(observer),
          generation, ReceiptOutcome::Accepted, std::nullopt, std::nullopt};
}

inline CausalReceipt applied_receipt(std::string receipt_id, std::string causation_id,
                                      std::string observer, int64_t generation) {
  return {std::move(receipt_id), std::move(causation_id), std::move(observer),
          generation, ReceiptOutcome::Applied, std::nullopt, std::nullopt};
}

inline CausalReceipt rejected_receipt(std::string receipt_id, std::string causation_id,
                                       std::string observer, int64_t generation) {
  return {std::move(receipt_id), std::move(causation_id), std::move(observer),
          generation, ReceiptOutcome::Rejected, std::nullopt, std::nullopt};
}

struct ReceiptRecorded {};
struct ReceiptDuplicate {};
struct ReceiptStaleGeneration { int64_t expected; int64_t actual; };
struct ReceiptTerminalConflict {
  std::string causation_id;
  ReceiptOutcome existing;
  ReceiptOutcome incoming;
};

using ReceiptApplyStatus = std::variant<ReceiptRecorded, ReceiptDuplicate,
                                          ReceiptStaleGeneration, ReceiptTerminalConflict>;

class ReceiptProjection {
 public:
  int64_t current_generation() const { return current_generation_; }
  int receipt_count() const { return static_cast<int>(receipts_by_id_.size()); }

  ReceiptApplyStatus observe(std::optional<int64_t> current_gen, const CausalReceipt& receipt) {
    if (receipts_by_id_.count(receipt.receipt_id) || stale_receipt_ids_.count(receipt.receipt_id))
      return ReceiptDuplicate{};

    if (current_gen && receipt.generation != *current_gen) {
      stale_receipt_ids_.insert(receipt.receipt_id);
      current_generation_ = std::max(current_generation_, receipt.generation);
      return ReceiptStaleGeneration{*current_gen, receipt.generation};
    }

    auto existing_term = terminal_by_causation_.find(receipt.causation_id);
    if (existing_term != terminal_by_causation_.end() && is_terminal(receipt.outcome)) {
      if (existing_term->second != receipt.outcome) {
        return ReceiptTerminalConflict{receipt.causation_id, existing_term->second, receipt.outcome};
      }
    }

    receipts_by_id_[receipt.receipt_id] = receipt;
    latest_by_causation_[receipt.causation_id] = receipt;

    if (is_terminal(receipt.outcome)) {
      if (!terminal_by_causation_.count(receipt.causation_id)) {
        terminal_by_causation_[receipt.causation_id] = receipt.outcome;
      }
    }

    if (current_gen)
      current_generation_ = std::max(current_generation_, receipt.generation);

    return ReceiptRecorded{};
  }

  std::optional<CausalReceipt> latest_for(const std::string& causation_id) const {
    auto it = latest_by_causation_.find(causation_id);
    if (it == latest_by_causation_.end()) return std::nullopt;
    return it->second;
  }

  std::optional<CausalReceipt> terminal_for(const std::string& causation_id) const {
    auto it = terminal_by_causation_.find(causation_id);
    if (it == terminal_by_causation_.end()) return std::nullopt;
    auto rit = latest_by_causation_.find(causation_id);
    if (rit == latest_by_causation_.end()) return std::nullopt;
    return rit->second;
  }

  bool contains_receipt(const std::string& receipt_id) const {
    return receipts_by_id_.count(receipt_id) > 0;
  }

  std::vector<std::string> stale_receipt_ids() const {
    return {stale_receipt_ids_.begin(), stale_receipt_ids_.end()};
  }

 private:
  std::unordered_map<std::string, CausalReceipt> receipts_by_id_;
  std::unordered_map<std::string, CausalReceipt> latest_by_causation_;
  std::unordered_map<std::string, ReceiptOutcome> terminal_by_causation_;
  std::unordered_set<std::string> stale_receipt_ids_;
  int64_t current_generation_ = 0;
};

// -- State projection mirror --

class StateProjectionMirror {
 public:
  void mark_dirty(NodeId node) { dirty_.insert(node); }

  void resolve(NodeId node, IpcValue value) {
    values_[node] = std::move(value);
    dirty_.erase(node);
  }

  bool is_dirty(NodeId node) const { return dirty_.count(node) > 0; }

  std::vector<NodeId> dirty_nodes() const {
    std::vector<NodeId> result(dirty_.begin(), dirty_.end());
    std::sort(result.begin(), result.end());
    return result;
  }

  Epoch base_epoch() const { return base_epoch_; }

  Delta flush() {
    std::vector<DeltaOp> ops;
    auto dirty = dirty_nodes();
    for (auto n : dirty) {
      ops.push_back(DeltaOpInvalidate{n});
    }
    std::vector<NodeId> resolved;
    for (auto& [n, _] : values_) {
      if (!dirty_.count(n)) resolved.push_back(n);
    }
    std::sort(resolved.begin(), resolved.end());
    for (auto n : resolved) {
      ops.push_back(DeltaOpSlotValue{n, values_[n]});
    }
    dirty_.clear();
    values_.clear();
    auto delta = delta_next(base_epoch_, std::move(ops));
    base_epoch_ = delta.epoch;
    return delta;
  }

  static uint64_t document_hash(const std::string& path) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (char c : path) {
      hash ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
      hash *= 0x100000001b3ULL;
    }
    return hash;
  }

 private:
  std::unordered_set<NodeId> dirty_;
  std::unordered_map<NodeId, IpcValue> values_;
  Epoch base_epoch_ = 0;
};

}  // namespace lazily

#endif  // LAZILY_RECEIPT_HPP
