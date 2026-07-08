#ifndef LAZILY_LOSSLESS_TREE_CRDT_HPP
#define LAZILY_LOSSLESS_TREE_CRDT_HPP

#include <lazily/crdt.hpp>
#include <lazily/types.hpp>

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace lazily {

enum class LeafKind { Token, Trivia, Raw, Error };

struct TreeSortKey {
  std::vector<int> frac;
  PeerId peer;

  int compare(const TreeSortKey& o) const {
    size_t min_len = std::min(frac.size(), o.frac.size());
    for (size_t i = 0; i < min_len; ++i) {
      if (frac[i] != o.frac[i]) return frac[i] < o.frac[i] ? -1 : 1;
    }
    if (frac.size() != o.frac.size()) return frac.size() < o.frac.size() ? -1 : 1;
    if (peer != o.peer) return peer < o.peer ? -1 : 1;
    return 0;
  }
  bool operator<(const TreeSortKey& o) const { return compare(o) < 0; }
  bool operator==(const TreeSortKey& o) const { return compare(o) == 0; }
};

struct TreeNodeSeedElement { std::string kind; };
struct TreeNodeSeedLeaf { LeafKind kind; std::string text; };
using TreeNodeSeed = std::variant<TreeNodeSeedElement, TreeNodeSeedLeaf>;

struct TreeOpCreateNode { OpId id; OpId parent; TreeSortKey sort; TreeNodeSeed seed; };
struct TreeOpTombstone { OpId node; };
struct TreeOpReorder { OpId node; TreeSortKey sort; };
struct TreeOpLeafEdit { OpId node; OpId prev; std::vector<TextOp> ops; };
struct TreeOpSplitLeaf { OpId node; OpId new_id; TreeSortKey sort; int at_char; OpId prev; };
struct TreeOpMergeLeaves { OpId left; OpId right; OpId prev_left; OpId prev_right; };

using TreeOpKind = std::variant<TreeOpCreateNode, TreeOpTombstone, TreeOpReorder,
                                  TreeOpLeafEdit, TreeOpSplitLeaf, TreeOpMergeLeaves>;

struct TreeOp {
  OpId id;
  TreeOpKind kind;
};

struct TreeUpdate {
  std::vector<TreeOp> ops;
};

struct TreeDotRange {
  int64_t contiguous = 0;
  std::unordered_set<int64_t> sparse;

  bool contains(int64_t counter) const {
    if (counter <= contiguous) return true;
    return sparse.count(counter) > 0;
  }

  void observe(int64_t counter) {
    if (counter <= contiguous) return;
    sparse.insert(counter);
    while (sparse.count(contiguous + 1)) {
      sparse.erase(contiguous + 1);
      contiguous++;
    }
  }
};

struct TreeVersionFrontier {
  std::unordered_map<PeerId, TreeDotRange> dots;

  bool contains(const OpId& id) const {
    auto it = dots.find(id.peer);
    if (it == dots.end()) return false;
    return it->second.contains(id.counter);
  }

  void observe(const OpId& id) {
    dots[id.peer].observe(id.counter);
  }
};

inline const OpId kTreeRoot = {0, 0};

inline std::vector<int> tree_key_between(const std::vector<int>* lo,
                                            const std::vector<int>* hi) {
  if (!lo && !hi) return {128};
  if (!lo) {
    if (hi->front() > 1) return {hi->front() / 2};
    std::vector<int> result = *hi;
    result.push_back(128);
    return result;
  }
  if (!hi) {
    std::vector<int> result = *lo;
    result.push_back(128);
    return result;
  }
  size_t min_len = std::min(lo->size(), hi->size());
  for (size_t i = 0; i < min_len; ++i) {
    if (lo->at(i) < hi->at(i) && hi->at(i) - lo->at(i) > 1) {
      std::vector<int> result(lo->begin(), lo->begin() + i + 1);
      result.back() = (lo->at(i) + hi->at(i)) / 2;
      return result;
    }
  }
  std::vector<int> result = *lo;
  result.push_back(128);
  return result;
}

struct TreeNode {
  OpId id;
  std::optional<OpId> parent;
  TreeSortKey sort;
  OpId sort_stamp;
  bool is_leaf = false;
  LeafKind leaf_kind = LeafKind::Token;
  std::string element_kind;
  std::shared_ptr<TextCrdt> text;
  std::optional<OpId> tomb;
  OpId text_head;
};

class LosslessTreeCrdt {
 public:
  explicit LosslessTreeCrdt(PeerId peer)
      : peer_(peer), counter_(0) {
    TreeNode root;
    root.id = kTreeRoot;
    root.sort = {{128}, peer};
    root.sort_stamp = {0, peer};
    root.is_leaf = false;
    root.element_kind = "root";
    nodes_[kTreeRoot] = root;
  }

  OpId create_node(const OpId& parent, const OpId* after, TreeNodeSeed seed) {
    auto id = next_id();
    auto parent_it = nodes_.find(parent);
    if (parent_it == nodes_.end()) return id;

    auto left = after ? &nodes_[*after].sort : nullptr;
    auto right = find_right_sibling(parent, after);
    auto key = TreeSortKey{tree_key_between(
        left ? &left->frac : nullptr,
        right ? &right->frac : nullptr), peer_};

    TreeNode node;
    node.id = id;
    node.parent = parent;
    node.sort = key;
    node.sort_stamp = id;

    if (std::holds_alternative<TreeNodeSeedElement>(seed)) {
      node.is_leaf = false;
      node.element_kind = std::get<TreeNodeSeedElement>(seed).kind;
    } else {
      auto& leaf = std::get<TreeNodeSeedLeaf>(seed);
      node.is_leaf = true;
      node.leaf_kind = leaf.kind;
      node.text = std::make_shared<TextCrdt>(peer_);
      node.text->insert_str(0, leaf.text);
      node.text_head = id;
    }

    nodes_[id] = node;
    children_[parent].push_back(id);
    log_.push_back(TreeOp{id, TreeOpCreateNode{id, parent, key, seed}});
    return id;
  }

  void tombstone_node(const OpId& node_id) {
    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) return;
    if (it->second.tomb) {
      if (it->second.tomb < node_id) return;
    }
    it->second.tomb = next_id();
    log_.push_back(TreeOp{it->second.tomb.value(), TreeOpTombstone{node_id}});
  }

  void reorder_child(const OpId& node_id, const OpId* after) {
    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) return;
    auto parent = it->second.parent;
    if (!parent) return;

    auto left = after ? &nodes_[*after].sort : nullptr;
    auto right = find_right_sibling(*parent, after);
    auto key = TreeSortKey{tree_key_between(
        left ? &left->frac : nullptr,
        right ? &right->frac : nullptr), peer_};

    auto stamp = next_id();
    it->second.sort = key;
    it->second.sort_stamp = stamp;
    log_.push_back(TreeOp{stamp, TreeOpReorder{node_id, key}});
  }

  void edit_leaf(const OpId& node_id, size_t at_byte, size_t delete_bytes,
                  const std::string& insert) {
    auto it = nodes_.find(node_id);
    if (it == nodes_.end() || !it->second.is_leaf || !it->second.text) return;

    // Re-own the leaf's TextCrdt under local peer
    it->second.text = std::make_shared<TextCrdt>(it->second.text->fork(peer_));

    size_t char_pos = byte_to_char(it->second.text->text(), at_byte);
    for (size_t i = 0; i < delete_bytes && char_pos < it->second.text->visible_len();) {
      it->second.text->del(char_pos);
      ++i;
    }
    if (!insert.empty()) {
      it->second.text->insert_str(char_pos, insert);
    }

    auto prev = it->second.text_head;
    auto stamp = next_id();
    it->second.text_head = stamp;
    auto vv = it->second.text->version_vector();
    auto delta = it->second.text->delta_since({});
    log_.push_back(TreeOp{stamp, TreeOpLeafEdit{node_id, prev, delta}});
  }

  OpId split_leaf(const OpId& node_id, size_t at_byte) {
    auto it = nodes_.find(node_id);
    if (it == nodes_.end() || !it->second.is_leaf || !it->second.text) return {};

    auto text = it->second.text->text();
    size_t char_pos = byte_to_char(text, at_byte);
    std::string head = text.substr(0, at_byte);
    std::string tail = text.substr(at_byte);

    auto new_id = next_id();
    auto parent = it->second.parent.value_or(kTreeRoot);

    // Re-seed head
    it->second.text = std::make_shared<TextCrdt>(peer_);
    it->second.text->insert_str(0, head);
    auto prev = it->second.text_head;
    auto stamp = next_id();
    it->second.text_head = stamp;

    // Create tail node
    TreeNode tail_node;
    tail_node.id = new_id;
    tail_node.parent = parent;
    tail_node.sort = TreeSortKey{tree_key_between(&it->second.sort.frac, nullptr), peer_};
    tail_node.sort_stamp = new_id;
    tail_node.is_leaf = true;
    tail_node.leaf_kind = it->second.leaf_kind;
    tail_node.text = std::make_shared<TextCrdt>(new_id.peer);
    tail_node.text->insert_str(0, tail);
    tail_node.text_head = new_id;
    nodes_[new_id] = tail_node;
    children_[parent].push_back(new_id);

    log_.push_back(TreeOp{stamp, TreeOpSplitLeaf{node_id, new_id, tail_node.sort,
                                                     static_cast<int>(char_pos), prev}});
    return new_id;
  }

  void merge_adjacent_leaves(const OpId& left_id, const OpId& right_id) {
    auto lit = nodes_.find(left_id);
    auto rit = nodes_.find(right_id);
    if (lit == nodes_.end() || rit == nodes_.end()) return;
    if (!lit->second.is_leaf || !rit->second.is_leaf) return;

    std::string merged = lit->second.text->text() + rit->second.text->text();
    lit->second.text = std::make_shared<TextCrdt>(peer_);
    lit->second.text->insert_str(0, merged);

    auto prev_left = lit->second.text_head;
    auto prev_right = rit->second.text_head;
    auto stamp = next_id();
    lit->second.text_head = stamp;

    // Tombstone right node
    rit->second.tomb = next_id();

    log_.push_back(TreeOp{stamp, TreeOpMergeLeaves{left_id, right_id, prev_left, prev_right}});
  }

  std::string render() const {
    std::string result;
    render_node(kTreeRoot, result);
    return result;
  }

  size_t live_node_count() const {
    size_t count = 0;
    for (auto& [_, node] : nodes_) {
      if (!node.tomb) ++count;
    }
    return count;
  }

  const TreeVersionFrontier& frontier() const { return frontier_; }

  std::optional<std::string> element_kind(const OpId& node) const {
    auto it = nodes_.find(node);
    if (it == nodes_.end() || it->second.is_leaf) return std::nullopt;
    return it->second.element_kind;
  }

  std::optional<LeafKind> leaf_kind(const OpId& node) const {
    auto it = nodes_.find(node);
    if (it == nodes_.end() || !it->second.is_leaf) return std::nullopt;
    return it->second.leaf_kind;
  }

  std::vector<OpId> children(const OpId& parent) const {
    std::vector<OpId> result;
    auto it = children_.find(parent);
    if (it != children_.end()) {
      for (auto& c : it->second) {
        if (nodes_.at(c).tomb) continue;
        result.push_back(c);
      }
    }
    return result;
  }

  std::optional<std::string> leaf_text(const OpId& node) const {
    auto it = nodes_.find(node);
    if (it == nodes_.end() || !it->second.is_leaf || !it->second.text) return std::nullopt;
    return it->second.text->text();
  }

  TreeUpdate diff(const TreeVersionFrontier& their) const {
    TreeUpdate update;
    for (auto& op : log_) {
      if (!their.contains(op.id)) {
        update.ops.push_back(op);
      }
    }
    return update;
  }

  void apply_update(const TreeUpdate& update) {
    for (auto& op : update.ops) {
      frontier_.observe(op.id);
      apply_op(op);
    }
  }

  LosslessTreeCrdt fork(PeerId new_peer) const {
    LosslessTreeCrdt clone(new_peer);
    clone.counter_ = counter_;
    clone.nodes_ = nodes_;
    clone.children_ = children_;
    clone.log_ = log_;
    clone.frontier_ = frontier_;
    return clone;
  }

 private:
  PeerId peer_;
  int64_t counter_;
  std::map<OpId, TreeNode> nodes_;
  std::map<OpId, std::vector<OpId>> children_;
  std::vector<TreeOp> log_;
  TreeVersionFrontier frontier_;
  std::vector<TreeOp> buffered_;

  OpId next_id() { return {++counter_, peer_}; }

  const TreeSortKey* find_right_sibling(const OpId& parent, const OpId* after) const {
    auto it = children_.find(parent);
    if (it == children_.end()) return nullptr;
    bool found_after = (after == nullptr);
    for (auto& c : it->second) {
      auto nit = nodes_.find(c);
      if (nit == nodes_.end() || nit->second.tomb) continue;
      if (found_after) return &nit->second.sort;
      if (after && c == *after) found_after = true;
    }
    return nullptr;
  }

  static size_t byte_to_char(const std::string& s, size_t byte_offset) {
    size_t char_count = 0;
    for (size_t i = 0; i < byte_offset && i < s.size(); ++i) {
      if ((s[i] & 0xC0) != 0x80) ++char_count;
    }
    return char_count;
  }

  void render_node(const OpId& id, std::string& out) const {
    auto it = nodes_.find(id);
    if (it == nodes_.end() || it->second.tomb) return;
    if (it->second.is_leaf && it->second.text) {
      out += it->second.text->text();
      return;
    }
    auto cit = children_.find(id);
    if (cit != children_.end()) {
      std::vector<std::pair<TreeSortKey, OpId>> sorted_children;
      for (auto& c : cit->second) {
        auto nit = nodes_.find(c);
        if (nit == nodes_.end() || nit->second.tomb) continue;
        sorted_children.push_back({nit->second.sort, c});
      }
      std::sort(sorted_children.begin(), sorted_children.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
      for (auto& [_, c] : sorted_children) {
        render_node(c, out);
      }
    }
  }

  void apply_op(const TreeOp& op) {
    if (std::holds_alternative<TreeOpCreateNode>(op.kind)) {
      auto& k = std::get<TreeOpCreateNode>(op.kind);
      if (nodes_.count(k.id)) return;
      TreeNode node;
      node.id = k.id;
      node.parent = k.parent;
      node.sort = k.sort;
      node.sort_stamp = op.id;
      if (std::holds_alternative<TreeNodeSeedElement>(k.seed)) {
        node.is_leaf = false;
        node.element_kind = std::get<TreeNodeSeedElement>(k.seed).kind;
      } else {
        auto& leaf = std::get<TreeNodeSeedLeaf>(k.seed);
        node.is_leaf = true;
        node.leaf_kind = leaf.kind;
        node.text = std::make_shared<TextCrdt>(k.id.peer);
        node.text->insert_str(0, leaf.text);
        node.text_head = k.id;
      }
      nodes_[k.id] = node;
      children_[k.parent].push_back(k.id);
    } else if (std::holds_alternative<TreeOpTombstone>(op.kind)) {
      auto& k = std::get<TreeOpTombstone>(op.kind);
      auto it = nodes_.find(k.node);
      if (it != nodes_.end()) {
        if (!it->second.tomb || op.id < *it->second.tomb) {
          it->second.tomb = op.id;
        }
      }
    } else if (std::holds_alternative<TreeOpReorder>(op.kind)) {
      auto& k = std::get<TreeOpReorder>(op.kind);
      auto it = nodes_.find(k.node);
      if (it != nodes_.end() && op.id > it->second.sort_stamp) {
        it->second.sort = k.sort;
        it->second.sort_stamp = op.id;
      }
    } else if (std::holds_alternative<TreeOpLeafEdit>(op.kind)) {
      auto& k = std::get<TreeOpLeafEdit>(op.kind);
      auto it = nodes_.find(k.node);
      if (it != nodes_.end() && it->second.is_leaf && it->second.text) {
        it->second.text->apply_delta(k.ops);
        it->second.text_head = op.id;
      }
    } else if (std::holds_alternative<TreeOpSplitLeaf>(op.kind)) {
      auto& k = std::get<TreeOpSplitLeaf>(op.kind);
      auto it = nodes_.find(k.node);
      if (it != nodes_.end() && it->second.is_leaf && it->second.text) {
        std::string text = it->second.text->text();
        size_t byte_pos = char_to_byte(text, k.at_char);
        std::string head = text.substr(0, byte_pos);
        std::string tail = text.substr(byte_pos);
        it->second.text = std::make_shared<TextCrdt>(k.node.peer);
        it->second.text->insert_str(0, head);
        it->second.text_head = op.id;
        TreeNode tail_node;
        tail_node.id = k.new_id;
        tail_node.parent = it->second.parent;
        tail_node.sort = k.sort;
        tail_node.sort_stamp = op.id;
        tail_node.is_leaf = true;
        tail_node.leaf_kind = it->second.leaf_kind;
        tail_node.text = std::make_shared<TextCrdt>(k.new_id.peer);
        tail_node.text->insert_str(0, tail);
        tail_node.text_head = op.id;
        nodes_[k.new_id] = tail_node;
        if (it->second.parent) {
          children_[*it->second.parent].push_back(k.new_id);
        }
      }
    } else if (std::holds_alternative<TreeOpMergeLeaves>(op.kind)) {
      auto& k = std::get<TreeOpMergeLeaves>(op.kind);
      auto lit = nodes_.find(k.left);
      auto rit = nodes_.find(k.right);
      if (lit != nodes_.end() && rit != nodes_.end() &&
          lit->second.is_leaf && rit->second.is_leaf) {
        std::string merged = lit->second.text->text() + rit->second.text->text();
        lit->second.text = std::make_shared<TextCrdt>(peer_);
        lit->second.text->insert_str(0, merged);
        lit->second.text_head = op.id;
        rit->second.tomb = op.id;
      }
    }
  }

  static size_t char_to_byte(const std::string& s, size_t char_offset) {
    size_t byte_pos = 0;
    size_t char_count = 0;
    while (byte_pos < s.size() && char_count < char_offset) {
      if ((s[byte_pos] & 0xC0) != 0x80) ++char_count;
      ++byte_pos;
    }
    return byte_pos;
  }
};

}  // namespace lazily

#endif  // LAZILY_LOSSLESS_TREE_CRDT_HPP
