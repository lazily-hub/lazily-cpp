#ifndef LAZILY_HLC_HPP
#define LAZILY_HLC_HPP

#include <lazily/types.hpp>

#include <algorithm>

namespace lazily {

struct HlcStamp {
  int64_t wall_time = 0;
  int64_t logical = 0;
  PeerId peer = 0;

  int compare(const HlcStamp& o) const {
    if (wall_time != o.wall_time) return wall_time < o.wall_time ? -1 : 1;
    if (logical != o.logical) return logical < o.logical ? -1 : 1;
    if (peer != o.peer) return peer < o.peer ? -1 : 1;
    return 0;
  }
  bool operator<(const HlcStamp& o) const { return compare(o) < 0; }
  bool operator<=(const HlcStamp& o) const { return compare(o) <= 0; }
  bool operator>(const HlcStamp& o) const { return compare(o) > 0; }
  bool operator>=(const HlcStamp& o) const { return compare(o) >= 0; }
  bool operator==(const HlcStamp& o) const { return compare(o) == 0; }
};

}  // namespace lazily

namespace std {
template <>
struct hash<lazily::HlcStamp> {
  size_t operator()(const lazily::HlcStamp& s) const noexcept {
    return hash<int64_t>{}(s.wall_time) ^
           (hash<int64_t>{}(s.logical) << 1) ^
           (hash<int64_t>{}(s.peer) << 2);
  }
};
}  // namespace std

namespace lazily {

inline HlcStamp min_stamp(const HlcStamp& a, const HlcStamp& b) {
  return a < b ? a : b;
}
inline HlcStamp max_stamp(const HlcStamp& a, const HlcStamp& b) {
  return a > b ? a : b;
}

class Hlc {
 public:
  explicit Hlc(PeerId peer) : peer_(peer) {}
  PeerId peer() const { return peer_; }

  HlcStamp tick(int64_t now_micros) {
    if (now_micros > last_wall_) {
      last_wall_ = now_micros;
      last_logical_ = 0;
    } else {
      last_logical_++;
    }
    return {last_wall_, last_logical_, peer_};
  }

  HlcStamp observe(const HlcStamp& remote, int64_t now_micros) {
    int64_t wall = std::max({last_wall_, remote.wall_time, now_micros});
    if (wall == last_wall_ && wall == remote.wall_time) {
      last_logical_ = std::max(last_logical_, remote.logical) + 1;
    } else if (wall == last_wall_) {
      last_logical_++;
    } else if (wall == remote.wall_time) {
      last_logical_ = remote.logical + 1;
    } else {
      last_logical_ = 0;
    }
    last_wall_ = wall;
    return {last_wall_, last_logical_, peer_};
  }

 private:
  PeerId peer_;
  int64_t last_wall_ = 0;
  int64_t last_logical_ = 0;
};

// Wire stamp (codec-stable wire mirror of HlcStamp)
struct WireStamp {
  int64_t wall_time;
  int64_t logical;
  PeerId peer;
};

inline WireStamp to_wire(const HlcStamp& s) {
  return {s.wall_time, s.logical, s.peer};
}
inline HlcStamp from_wire(const WireStamp& s) {
  return {s.wall_time, s.logical, s.peer};
}

// Stamp frontier (per-peer max)
class StampFrontier {
 public:
  void observe(PeerId peer, const HlcStamp& stamp) {
    auto it = stamps_.find(peer);
    if (it == stamps_.end() || stamp > it->second) {
      stamps_[peer] = stamp;
    }
  }

  bool knows(PeerId peer) const { return stamps_.count(peer) > 0; }

  std::optional<HlcStamp> get(PeerId peer) const {
    auto it = stamps_.find(peer);
    if (it == stamps_.end()) return std::nullopt;
    return it->second;
  }

  std::vector<PeerId> peers() const {
    std::vector<PeerId> result;
    for (auto& [p, _] : stamps_) result.push_back(p);
    std::sort(result.begin(), result.end());
    return result;
  }

  std::optional<HlcStamp> watermark(const std::vector<PeerId>& membership) const {
    HlcStamp min;
    bool first = true;
    for (auto p : membership) {
      auto it = stamps_.find(p);
      if (it == stamps_.end()) return std::nullopt;
      if (first || it->second < min) { min = it->second; first = false; }
    }
    return first ? std::nullopt : std::optional<HlcStamp>(min);
  }

 private:
  std::unordered_map<PeerId, HlcStamp> stamps_;
};

}  // namespace lazily

#endif  // LAZILY_HLC_HPP
