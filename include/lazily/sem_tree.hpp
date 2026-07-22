#ifndef LAZILY_SEM_TREE_HPP
#define LAZILY_SEM_TREE_HPP

#include <lazily/collections.hpp>
#include <lazily/context.hpp>
#include <lazily/cell.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace lazily {

template <typename V, typename D>
struct SemNode {
  std::string id;
  Source<V> value_cell;
  Source<int> child_keys_cell;
  std::vector<std::string> child_order;
  std::unordered_map<std::string, std::shared_ptr<SemNode<V, D>>> children;
  Computed<D> slot;
  int child_version;
};

template <typename V, typename D>
class SemTree {
 public:
  using FoldFn = std::function<D(const V&, const std::vector<D>&)>;

  SemTree(Context& ctx, const std::string& root_id, V root_value, FoldFn fold)
      : ctx_(ctx), fold_(std::move(fold)) {
    root_ = std::make_shared<SemNode<V, D>>();
    root_->id = root_id;
    root_->value_cell = ctx_.source(std::move(root_value));
    root_->child_version = 0;
    root_->child_keys_cell = ctx_.source(0);
    root_->slot = build_fold_computed(root_);
  }

  void add_child(const std::string& parent_id, const std::string& child_id, V value) {
    auto parent = find_node(root_, parent_id);
    if (!parent) return;
    auto child = std::make_shared<SemNode<V, D>>();
    child->id = child_id;
    child->value_cell = ctx_.source(std::move(value));
    child->child_version = 0;
    child->child_keys_cell = ctx_.source(0);
    child->slot = build_fold_computed(child);
    parent->child_order.push_back(child_id);
    parent->children[child_id] = child;
    parent->child_version++;
    ctx_.set(parent->child_keys_cell, parent->child_version);
  }

  void set_value(const std::string& id, V value) {
    auto node = find_node(root_, id);
    if (!node) return;
    ctx_.set(node->value_cell, std::move(value));
  }

  void remove_child(const std::string& parent_id, const std::string& child_id) {
    auto parent = find_node(root_, parent_id);
    if (!parent) return;
    parent->child_order.erase(
        std::remove(parent->child_order.begin(), parent->child_order.end(), child_id),
        parent->child_order.end());
    parent->children.erase(child_id);
    parent->child_version++;
    ctx_.set(parent->child_keys_cell, parent->child_version);
  }

  D value() { return ctx_.get(root_->slot); }

  std::optional<D> node_value(const std::string& id) {
    auto node = find_node(root_, id);
    if (!node) return std::nullopt;
    return ctx_.get(node->slot);
  }

  bool is_cached(const std::string& id) {
    auto node = find_node(root_, id);
    if (!node) return false;
    return ctx_.is_set(node->slot);
  }

  Computed<D> root_handle() { return root_->slot; }

  std::optional<Computed<D>> node_handle(const std::string& id) {
    auto node = find_node(root_, id);
    if (!node) return std::nullopt;
    return node->slot;
  }

 private:
  Context& ctx_;
  FoldFn fold_;
  std::shared_ptr<SemNode<V, D>> root_;

  Computed<D> build_fold_computed(std::shared_ptr<SemNode<V, D>> node) {
    auto value_cell = node->value_cell;
    auto child_keys_cell = node->child_keys_cell;
    auto fold = fold_;

    return ctx_.computed<D>([=](Context& ctx) -> D {
      (void)ctx.get(child_keys_cell);
      V val = ctx.get(value_cell);
      std::vector<D> child_vals;
      // Capture children at compute time
      auto n = node;
      for (auto& child_id : n->child_order) {
        auto it = n->children.find(child_id);
        if (it != n->children.end()) {
          child_vals.push_back(ctx.get(it->second->slot));
        }
      }
      return fold(val, child_vals);
    });
  }

  std::shared_ptr<SemNode<V, D>> find_node(
      std::shared_ptr<SemNode<V, D>> node, const std::string& id) {
    if (node->id == id) return node;
    for (auto& [_, child] : node->children) {
      auto found = find_node(child, id);
      if (found) return found;
    }
    return nullptr;
  }
};

}  // namespace lazily

#endif  // LAZILY_SEM_TREE_HPP
