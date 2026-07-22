#ifndef LAZILY_COLLECTIONS_HPP
#define LAZILY_COLLECTIONS_HPP

#include <lazily/context.hpp>
#include <lazily/cell.hpp>
#include <lazily/reactive_family.hpp>

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lazily {

// `CellMap` / `SlotMap` / `ReactiveMap` are defined in reactive_family.hpp.

// -- CellTree: ordered keyed reactive tree --

template <typename Id, typename V>
struct CellTreeNode {
  Id id;
  Source<V> value;
  std::optional<CellMap<Id, bool>> order;
  std::unordered_map<Id, size_t> child_index;
  std::vector<std::shared_ptr<CellTreeNode<Id, V>>> children;
};

template <typename Id, typename V>
class CellTree {
 public:
  CellTree(Context& ctx, Id id, V value)
      : inner_(std::make_shared<CellTreeNode<Id, V>>()) {
    inner_->id = std::move(id);
    inner_->value = ctx.source(std::move(value));
    inner_->order.emplace(ctx);
  }

  static CellTree leaf(Context& ctx, Id id, V value) {
    return CellTree(ctx, std::move(id), std::move(value));
  }

  const Id& id() const { return inner_->id; }
  Source<V> value_handle() const { return inner_->value; }

  V get(Context& ctx) { return ctx.get(inner_->value); }
  void set(Context& ctx, V value) { ctx.set(inner_->value, std::move(value)); }

  CellTree insert_child(Context& ctx, Id child_id, V value) {
    auto it = inner_->child_index.find(child_id);
    if (it != inner_->child_index.end())
      return CellTree(inner_->children[it->second]);

    auto child = std::make_shared<CellTreeNode<Id, V>>();
    child->id = child_id;
    child->value = ctx.source(std::move(value));
    child->order.emplace(ctx);

    size_t idx = inner_->children.size();
    inner_->child_index[child_id] = idx;
    inner_->children.push_back(child);
    inner_->order->entry(ctx, child_id, true);
    return CellTree(child);
  }

  bool remove_child(Context& ctx, const Id& child_id) {
    auto it = inner_->child_index.find(child_id);
    if (it == inner_->child_index.end()) return false;
    size_t idx = it->second;
    inner_->children.erase(inner_->children.begin() + idx);
    inner_->child_index.erase(it);
    for (auto& [k, v] : inner_->child_index) {
      if (v > idx) v--;
    }
    inner_->order->remove(ctx, child_id);
    return true;
  }

  bool move_child(Context& ctx, const Id& child_id, size_t to) {
    auto it = inner_->child_index.find(child_id);
    if (it == inner_->child_index.end()) return false;
    size_t from = it->second;
    to = std::min(to, inner_->children.size() - 1);
    if (from == to) return true;

    auto node = std::move(inner_->children[from]);
    inner_->children.erase(inner_->children.begin() + from);
    inner_->children.insert(inner_->children.begin() + to, std::move(node));

    for (auto& [k, v] : inner_->child_index) {
      if (from < to) {
        if (v == from) v = to;
        else if (v > from && v <= to) v--;
      } else {
        if (v == from) v = to;
        else if (v >= to && v < from) v++;
      }
    }
    inner_->order->move_to(ctx, child_id, to);
    return true;
  }

  std::vector<Id> child_ids(Context& ctx) {
    return inner_->order->keys(ctx);
  }

  size_t child_count(Context& ctx) {
    return inner_->order->len(ctx);
  }

  bool has_child(const Id& child_id) const {
    return inner_->child_index.count(child_id) > 0;
  }

  std::optional<CellTree> child(const Id& child_id) const {
    auto it = inner_->child_index.find(child_id);
    if (it == inner_->child_index.end()) return std::nullopt;
    return CellTree(inner_->children[it->second]);
  }

 private:
  std::shared_ptr<CellTreeNode<Id, V>> inner_;

  explicit CellTree(std::shared_ptr<CellTreeNode<Id, V>> node)
      : inner_(std::move(node)) {}
};

// -- Keyed reconciliation (LIS move-minimized) --

template <typename K, typename V>
struct DiffOp {
  enum class Kind { Insert, Remove, Move, Update };
  Kind kind;
  K key;
  std::optional<V> value;
  size_t index;

  static DiffOp insert(K key, V value, size_t index) {
    return {Kind::Insert, std::move(key), std::move(value), index};
  }
  static DiffOp remove(K key) {
    return {Kind::Remove, std::move(key), std::nullopt, 0};
  }
  static DiffOp move(K key, size_t to) {
    return {Kind::Move, std::move(key), std::nullopt, to};
  }
  static DiffOp update(K key, V value) {
    return {Kind::Update, std::move(key), std::move(value), 0};
  }
};

inline std::vector<size_t> longest_increasing_subsequence(const std::vector<size_t>& seq) {
  size_t n = seq.size();
  if (n == 0) return {};
  std::vector<size_t> tails;
  std::vector<size_t> prev(n, SIZE_MAX);
  for (size_t i = 0; i < n; ++i) {
    size_t lo = 0, hi = tails.size();
    while (lo < hi) {
      size_t mid = (lo + hi) / 2;
      if (seq[tails[mid]] < seq[i])
        lo = mid + 1;
      else
        hi = mid;
    }
    if (lo > 0) prev[i] = tails[lo - 1];
    if (lo == tails.size())
      tails.push_back(i);
    else
      tails[lo] = i;
  }
  std::vector<size_t> res;
  size_t k = tails.back();
  while (true) {
    res.push_back(k);
    if (prev[k] == SIZE_MAX) break;
    k = prev[k];
  }
  std::reverse(res.begin(), res.end());
  return res;
}

template <typename K, typename V>
std::vector<DiffOp<K, V>> reconcile(const std::vector<std::pair<K, V>>& old_seq,
                                     const std::vector<std::pair<K, V>>& new_seq) {
  std::unordered_map<K, size_t> old_pos;
  for (size_t i = 0; i < old_seq.size(); ++i)
    old_pos[old_seq[i].first] = i;

  std::unordered_map<K, const V*> old_val;
  for (auto& [k, v] : old_seq) old_val[k] = &v;

  std::unordered_set<K> new_keys;
  for (auto& [k, v] : new_seq) new_keys.insert(k);

  std::vector<DiffOp<K, V>> ops;

  // 1. Removes
  for (auto& [k, v] : old_seq) {
    if (!new_keys.count(k))
      ops.push_back(DiffOp<K, V>::remove(k));
  }

  // 2. Common keys in new order → LIS
  std::vector<size_t> common_new_idx;
  std::vector<size_t> seq;
  for (size_t i = 0; i < new_seq.size(); ++i) {
    auto it = old_pos.find(new_seq[i].first);
    if (it != old_pos.end()) {
      common_new_idx.push_back(i);
      seq.push_back(it->second);
    }
  }
  auto lis = longest_increasing_subsequence(seq);
  std::unordered_set<size_t> stable;
  for (auto j : lis) stable.insert(common_new_idx[j]);

  // 3. Inserts + Moves
  for (size_t i = 0; i < new_seq.size(); ++i) {
    const K& k = new_seq[i].first;
    if (old_pos.count(k)) {
      if (!stable.count(i))
        ops.push_back(DiffOp<K, V>::move(k, i));
    } else {
      ops.push_back(DiffOp<K, V>::insert(k, new_seq[i].second, i));
    }
  }

  // 4. Updates
  for (auto& [k, v] : new_seq) {
    auto it = old_val.find(k);
    if (it != old_val.end() && *it->second != v)
      ops.push_back(DiffOp<K, V>::update(k, v));
  }

  return ops;
}

template <typename K, typename V>
void apply_to_map(Context& ctx, CellMap<K, V>& map,
                   const std::vector<DiffOp<K, V>>& ops) {
  for (auto& op : ops) {
    switch (op.kind) {
      case DiffOp<K, V>::Kind::Remove:
        map.remove(ctx, op.key);
        break;
      case DiffOp<K, V>::Kind::Insert:
        map.entry(ctx, op.key, *op.value);
        map.move_to(ctx, op.key, op.index);
        break;
      case DiffOp<K, V>::Kind::Move:
        map.move_to(ctx, op.key, op.index);
        break;
      case DiffOp<K, V>::Kind::Update:
        map.set(ctx, op.key, *op.value);
        break;
    }
  }
}

template <typename K, typename V>
void apply_to_tree(Context& ctx, CellTree<K, V>& tree,
                   const std::vector<DiffOp<K, V>>& ops) {
  for (auto& op : ops) {
    switch (op.kind) {
      case DiffOp<K, V>::Kind::Remove:
        tree.remove_child(ctx, op.key);
        break;
      case DiffOp<K, V>::Kind::Insert:
        tree.insert_child(ctx, op.key, *op.value);
        tree.move_child(ctx, op.key, op.index);
        break;
      case DiffOp<K, V>::Kind::Move:
        tree.move_child(ctx, op.key, op.index);
        break;
      case DiffOp<K, V>::Kind::Update:
        if (auto child = tree.child(op.key))
          child->set(ctx, *op.value);
        break;
    }
  }
}

template <typename K, typename V>
void CellMap<K, V>::reconcile(Context& ctx,
                               const std::vector<std::pair<K, V>>& new_seq) {
  std::vector<std::pair<K, V>> old_seq;
  for (auto& k : this->keys(ctx)) {
    auto v = this->get(ctx, k);
    if (v) old_seq.emplace_back(k, *v);
  }
  auto ops = lazily::reconcile(old_seq, new_seq);
  apply_to_map(ctx, *this, ops);
}

}  // namespace lazily

#endif  // LAZILY_COLLECTIONS_HPP
