#ifndef LAZILY_CRDT_HPP
#define LAZILY_CRDT_HPP

#include <lazily/hlc.hpp>
#include <lazily/types.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lazily {

// -- OpId (unique element identifier for TextCrdt) --

struct OpId {
  int64_t counter = 0;
  PeerId peer = 0;

  bool operator==(const OpId& o) const {
    return counter == o.counter && peer == o.peer;
  }
  int compare(const OpId& o) const {
    if (counter != o.counter) return counter < o.counter ? -1 : 1;
    if (peer != o.peer) return peer < o.peer ? -1 : 1;
    return 0;
  }
  bool operator<(const OpId& o) const { return compare(o) < 0; }
  bool operator>(const OpId& o) const { return compare(o) > 0; }
};

}  // namespace lazily

namespace std {
template <>
struct hash<lazily::OpId> {
  size_t operator()(const lazily::OpId& id) const noexcept {
    return hash<int64_t>{}(id.counter) ^ (hash<int64_t>{}(id.peer) << 1);
  }
};
}  // namespace std

namespace lazily {

// -- TextCrdt (Fugue/RGA character CRDT + delta sync) --

struct TextOp {
  OpId id;
  std::string ch;
  std::optional<OpId> origin;
  std::optional<OpId> deleted;
};

struct TextElem {
  std::string ch;
  std::optional<OpId> origin;
  std::optional<OpId> deleted;
};

inline const OpId kTextRootKey = {0, 0};

class TextCrdt {
 public:
  explicit TextCrdt(PeerId peer) : peer_(peer), counter_(0) {}

  static TextCrdt from_str(PeerId peer, const std::string& s) {
    TextCrdt crdt(peer);
    for (size_t i = 0; i < s.size();) {
      int64_t next = static_cast<int64_t>(i);
      char c = s[i];
      std::string ch(1, c);
      crdt.insert_str(crdt.visible_len(), ch);
      ++i;
    }
    return crdt;
  }

  PeerId peer() const { return peer_; }
  OpId clock() const { return {counter_, peer_}; }

  void insert(size_t index, const std::string& ch) {
    if (ch.empty()) return;
    auto origin = find_origin(index);
    OpId id = next_id();
    TextElem elem{ch, origin, std::nullopt};
    elems_[id] = elem;
    by_origin_[origin.value_or(kTextRootKey)].push_back(id);
  }

  void insert_str(size_t index, const std::string& s) {
    for (size_t i = 0; i < s.size(); ++i) {
      insert(index + i, std::string(1, s[i]));
    }
  }

  void del(size_t index) {
    auto visible = visible_ids();
    if (index >= visible.size()) return;
    OpId target = visible[index];
    OpId del_id = next_id();
    auto& elem = elems_[target];
    if (!elem.deleted) {
      elem.deleted = del_id;
    } else {
      // Sticky tombstone: keep smaller delete id
      if (del_id < *elem.deleted) {
        elem.deleted = del_id;
      }
    }
  }

  std::string text() const {
    std::string result;
    for (auto& id : visible_ids()) {
      result += elems_.at(id).ch;
    }
    return result;
  }

  size_t visible_len() const { return visible_ids().size(); }
  bool is_empty() const { return visible_len() == 0; }
  size_t tombstone_count() const {
    size_t count = 0;
    for (auto& [_, elem] : elems_) {
      if (elem.deleted) ++count;
    }
    return count;
  }

  TextCrdt fork(PeerId new_peer) const {
    TextCrdt clone(new_peer);
    clone.counter_ = counter_;
    clone.elems_ = elems_;
    clone.by_origin_ = by_origin_;
    return clone;
  }

  TextCrdt clone() const { return fork(peer_); }

  bool merge(const TextCrdt& other) {
    bool changed = false;
    for (auto& [id, elem] : other.elems_) {
      auto it = elems_.find(id);
      if (it == elems_.end()) {
        elems_[id] = elem;
        by_origin_[elem.origin.value_or(kTextRootKey)].push_back(id);
        if (!elem.deleted) changed = true;
        if (id.counter > counter_) counter_ = id.counter;
      } else {
        // Merge tombstones (sticky: keep smaller)
        if (elem.deleted && !it->second.deleted) {
          it->second.deleted = elem.deleted;
          changed = true;
        } else if (elem.deleted && it->second.deleted) {
          if (*elem.deleted < *it->second.deleted) {
            it->second.deleted = elem.deleted;
          }
        }
        if (elem.deleted && elem.deleted->counter > counter_) counter_ = elem.deleted->counter;
      }
    }
    return changed;
  }

  // -- Delta sync (#lztextsync) --

  using VersionVector = std::unordered_map<PeerId, int64_t>;

  VersionVector version_vector() const {
    VersionVector vv;
    for (auto& [id, elem] : elems_) {
      vv[id.peer] = std::max(vv[id.peer], id.counter);
      if (elem.deleted) {
        vv[elem.deleted->peer] = std::max(vv[elem.deleted->peer], elem.deleted->counter);
      }
    }
    return vv;
  }

  std::vector<TextOp> delta_since(const VersionVector& their_vv) const {
    std::vector<TextOp> ops;
    for (auto& [id, elem] : elems_) {
      int64_t their = 0;
      auto it = their_vv.find(id.peer);
      if (it != their_vv.end()) their = it->second;
      if (id.counter > their) {
        ops.push_back({id, elem.ch, elem.origin, elem.deleted});
        continue;
      }
      // Check for fresh deletion of an already-shared element
      if (elem.deleted) {
        int64_t their_del = 0;
        auto dit = their_vv.find(elem.deleted->peer);
        if (dit != their_vv.end()) their_del = dit->second;
        if (elem.deleted->counter > their_del) {
          ops.push_back({id, elem.ch, elem.origin, elem.deleted});
        }
      }
    }
    return ops;
  }

  bool apply_delta(const std::vector<TextOp>& ops) {
    bool changed = false;
    for (auto& op : ops) {
      auto it = elems_.find(op.id);
      if (it == elems_.end()) {
        elems_[op.id] = {op.ch, op.origin, op.deleted};
        by_origin_[op.origin.value_or(kTextRootKey)].push_back(op.id);
        if (!op.deleted) changed = true;
        if (op.id.counter > counter_) counter_ = op.id.counter;
      } else {
        if (op.deleted && !it->second.deleted) {
          it->second.deleted = op.deleted;
          changed = true;
        } else if (op.deleted && it->second.deleted) {
          if (*op.deleted < *it->second.deleted) {
            it->second.deleted = op.deleted;
          }
        }
        if (op.deleted && op.deleted->counter > counter_) counter_ = op.deleted->counter;
      }
    }
    return changed;
  }

  size_t gc_with(std::function<bool(OpId)> is_stable) {
    size_t collected = 0;
    // Collect tombstones that are stable AND not referenced as an origin
    std::unordered_set<OpId> referenced;
    for (auto& [_, elem] : elems_) {
      if (elem.origin) referenced.insert(*elem.origin);
    }
    for (auto it = elems_.begin(); it != elems_.end();) {
      auto& [id, elem] = *it;
      if (elem.deleted && is_stable(*elem.deleted) && !referenced.count(id)) {
        // Remove from by_origin
        auto origin_key = elem.origin.value_or(kTextRootKey);
        auto& bucket = by_origin_[origin_key];
        bucket.erase(std::remove(bucket.begin(), bucket.end(), id), bucket.end());
        it = elems_.erase(it);
        ++collected;
      } else {
        ++it;
      }
    }
    return collected;
  }

 private:
  PeerId peer_;
  int64_t counter_;
  std::map<OpId, TextElem> elems_;
  std::map<OpId, std::vector<OpId>> by_origin_;

  OpId next_id() { return {++counter_, peer_}; }

  std::optional<OpId> find_origin(size_t visible_index) const {
    auto visible = visible_ids();
    if (visible_index == 0) return std::nullopt;
    if (visible_index - 1 < visible.size()) {
      return visible[visible_index - 1];
    }
    if (!visible.empty()) return visible.back();
    return std::nullopt;
  }

  std::vector<OpId> visible_ids() const {
    std::vector<OpId> result;
    traverse(kTextRootKey, result);
    return result;
  }

  void traverse(const OpId& origin, std::vector<OpId>& out) const {
    auto it = by_origin_.find(origin);
    if (it == by_origin_.end()) return;
    // Sort children descending by OpId (most recent first)
    auto children = it->second;
    std::sort(children.begin(), children.end(), [](const OpId& a, const OpId& b) {
      return b < a;
    });
    for (auto& child : children) {
      auto eit = elems_.find(child);
      if (eit == elems_.end()) continue;
      if (!eit->second.deleted) out.push_back(child);
      traverse(child, out);
    }
  }
};

// -- LWW Register --

template <typename V>
class LwwRegister {
 public:
  LwwRegister(V value, HlcStamp stamp) : value_(std::move(value)), stamp_(stamp) {}

  bool set(V new_value, HlcStamp new_stamp) {
    if (new_stamp > stamp_) {
      value_ = std::move(new_value);
      stamp_ = new_stamp;
      return true;
    }
    return false;
  }

  bool merge_from(const LwwRegister<V>& other) {
    return set(other.value_, other.stamp_);
  }

  const V& value() const { return value_; }
  const HlcStamp& stamp() const { return stamp_; }
  LwwRegister<V> copy() const { return *this; }

 private:
  V value_;
  HlcStamp stamp_;
};

// -- MV Register (multi-value) --

template <typename V>
class MvRegister {
 public:
  void write(V value, HlcStamp stamp, const std::unordered_set<HlcStamp>& observed) {
    bool collapses = true;
    for (auto& s : stamps_) {
      if (!observed.count(s)) { collapses = false; break; }
    }
    if (collapses) {
      values_.clear();
      stamps_.clear();
    }
    values_.push_back(std::move(value));
    stamps_.push_back(stamp);
  }

  void merge(const MvRegister<V>& other) {
    for (size_t i = 0; i < other.values_.size(); ++i) {
      bool found = false;
      for (auto& s : stamps_) {
        if (s == other.stamps_[i]) { found = true; break; }
      }
      if (!found) {
        values_.push_back(other.values_[i]);
        stamps_.push_back(other.stamps_[i]);
      }
    }
  }

  std::vector<V> values() const { return values_; }

 private:
  std::vector<V> values_;
  std::vector<HlcStamp> stamps_;
};

// -- PN Counter --

class PnCounter {
 public:
  explicit PnCounter(PeerId peer) : peer_(peer) {}

  void increment() { positive_[peer_]++; }
  void decrement() { negative_[peer_]++; }
  void increment_by(int64_t n) { positive_[peer_] += n; }
  void decrement_by(int64_t n) { negative_[peer_] += n; }

  int64_t value() const {
    int64_t sum = 0;
    for (auto& [_, v] : positive_) sum += v;
    for (auto& [_, v] : negative_) sum -= v;
    return sum;
  }

  void merge(const PnCounter& other) {
    for (auto& [k, v] : other.positive_) positive_[k] = std::max(positive_[k], v);
    for (auto& [k, v] : other.negative_) negative_[k] = std::max(negative_[k], v);
  }

  PnCounter copy() const { return *this; }
  PeerId peer() const { return peer_; }

 private:
  PeerId peer_;
  std::unordered_map<PeerId, int64_t> positive_;
  std::unordered_map<PeerId, int64_t> negative_;
};

// -- SeqCrdt (move-aware sequence CRDT) --

struct Position {
  std::vector<uint8_t> frac;
  PeerId peer = 0;

  int compare(const Position& o) const {
    size_t min_len = std::min(frac.size(), o.frac.size());
    for (size_t i = 0; i < min_len; ++i) {
      if (frac[i] != o.frac[i]) return frac[i] < o.frac[i] ? -1 : 1;
    }
    if (frac.size() != o.frac.size()) return frac.size() < o.frac.size() ? -1 : 1;
    if (peer != o.peer) return peer < o.peer ? -1 : 1;
    return 0;
  }
  bool operator<(const Position& o) const { return compare(o) < 0; }
  bool operator==(const Position& o) const { return compare(o) == 0; }
};

inline std::vector<uint8_t> key_between(const std::vector<uint8_t>* lo,
                                          const std::vector<uint8_t>* hi) {
  // Simple midpoint fractional index
  if (!lo && !hi) return {128};
  if (!lo) {
    std::vector<uint8_t> result;
    if (hi->front() > 1) {
      result.push_back(hi->front() / 2);
    } else {
      result = *hi;
      result.push_back(128);
    }
    return result;
  }
  if (!hi) {
    std::vector<uint8_t> result = *lo;
    result.push_back(128);
    return result;
  }
  // Find first differing byte
  size_t min_len = std::min(lo->size(), hi->size());
  for (size_t i = 0; i < min_len; ++i) {
    if (lo->at(i) < hi->at(i) && hi->at(i) - lo->at(i) > 1) {
      std::vector<uint8_t> result(lo->begin(), lo->begin() + i + 1);
      result.back() = (lo->at(i) + hi->at(i)) / 2;
      return result;
    }
  }
  // lo is a prefix of hi (or equal)
  std::vector<uint8_t> result = *lo;
  result.push_back(128);
  return result;
}

template <typename Id, typename V>
struct SeqEntry {
  LwwRegister<V> value;
  LwwRegister<Position> position;
  LwwRegister<bool> deleted;

  SeqEntry(V v, Position pos, HlcStamp stamp)
      : value(std::move(v), stamp),
        position(std::move(pos), stamp),
        deleted(false, stamp) {}

  HlcStamp max_stamp() const {
    HlcStamp m = value.stamp();
    if (position.stamp() > m) m = position.stamp();
    if (deleted.stamp() > m) m = deleted.stamp();
    return m;
  }
};

template <typename Id, typename V>
class SeqCrdt {
 public:
  explicit SeqCrdt(PeerId peer) : peer_(peer), hlc_(peer) {}

  PeerId peer() const { return peer_; }

  void insert_between(const Id& id, V value,
                       const Id* left, const Id* right, int64_t now_micros) {
    auto stamp = hlc_.tick(now_micros);
    auto left_pos = left ? &entries_.at(*left).position.value() : nullptr;
    auto right_pos = right ? &entries_.at(*right).position.value() : nullptr;
    auto pos = Position{key_between(
        left_pos ? &left_pos->frac : nullptr,
        right_pos ? &right_pos->frac : nullptr), peer_};
    entries_.emplace(id, SeqEntry<Id, V>(std::move(value), pos, stamp));
  }

  void insert_back(const Id& id, V value, int64_t now_micros) {
    auto ord = order();
    const Id* left = ord.empty() ? nullptr : &ord.back();
    insert_between(id, std::move(value), left, nullptr, now_micros);
  }

  void insert_front(const Id& id, V value, int64_t now_micros) {
    auto ord = order();
    const Id* right = ord.empty() ? nullptr : &ord.front();
    insert_between(id, std::move(value), nullptr, right, now_micros);
  }

  bool set_value(const Id& id, V value, int64_t now_micros) {
    auto it = entries_.find(id);
    if (it == entries_.end()) return false;
    auto stamp = hlc_.tick(now_micros);
    return it->second.value.set(std::move(value), stamp);
  }

  bool move_between(const Id& id, const Id* left, const Id* right, int64_t now_micros) {
    auto it = entries_.find(id);
    if (it == entries_.end()) return false;
    auto stamp = hlc_.tick(now_micros);
    auto left_pos = left ? &entries_.at(*left).position.value() : nullptr;
    auto right_pos = right ? &entries_.at(*right).position.value() : nullptr;
    auto pos = Position{key_between(
        left_pos ? &left_pos->frac : nullptr,
        right_pos ? &right_pos->frac : nullptr), peer_};
    return it->second.position.set(std::move(pos), stamp);
  }

  bool remove(const Id& id, int64_t now_micros) {
    auto it = entries_.find(id);
    if (it == entries_.end()) return false;
    auto stamp = hlc_.tick(now_micros);
    return it->second.deleted.set(true, stamp);
  }

  bool contains(const Id& id) const { return entries_.count(id) > 0; }

  std::optional<V> get(const Id& id) const {
    auto it = entries_.find(id);
    if (it == entries_.end() || it->second.deleted.value()) return std::nullopt;
    return it->second.value.value();
  }

  std::vector<Id> order() const {
    std::vector<std::pair<Position, Id>> live;
    for (auto& [id, entry] : entries_) {
      if (!entry.deleted.value())
        live.push_back({entry.position.value(), id});
    }
    std::sort(live.begin(), live.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<Id> result;
    for (auto& [_, id] : live) result.push_back(id);
    return result;
  }

  size_t len() const {
    size_t count = 0;
    for (auto& [_, e] : entries_) if (!e.deleted.value()) ++count;
    return count;
  }

  size_t tombstone_count() const {
    size_t count = 0;
    for (auto& [_, e] : entries_) if (e.deleted.value()) ++count;
    return count;
  }

  bool merge(const SeqCrdt<Id, V>& other, int64_t now_micros) {
    // Advance HLC past every remote stamp
    for (auto& [_, entry] : other.entries_) {
      hlc_.observe(entry.max_stamp(), now_micros);
    }
    bool changed = false;
    for (auto& [id, entry] : other.entries_) {
      auto it = entries_.find(id);
      if (it == entries_.end()) {
        entries_.emplace(id, entry);
        changed = true;
      } else {
        changed |= it->second.value.merge_from(entry.value);
        changed |= it->second.position.merge_from(entry.position);
        changed |= it->second.deleted.merge_from(entry.deleted);
      }
    }
    return changed;
  }

  SeqCrdt<Id, V> fork(PeerId new_peer) const {
    SeqCrdt<Id, V> clone(new_peer);
    clone.entries_ = entries_;
    return clone;
  }

 private:
  PeerId peer_;
  Hlc hlc_;
  std::unordered_map<Id, SeqEntry<Id, V>> entries_;
};

}  // namespace lazily

#endif  // LAZILY_CRDT_HPP
