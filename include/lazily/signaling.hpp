#ifndef LAZILY_SIGNALING_HPP
#define LAZILY_SIGNALING_HPP

#include <lazily/types.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace lazily {

enum class SignalingMode { Open, Allowlist };

// Client → Server messages (internally tagged by "type")
struct ClientJoin { PeerId peer; std::vector<std::string> capabilities; };
struct ClientOffer { PeerId to; std::string sdp; };
struct ClientAnswer { PeerId to; std::string sdp; };
struct ClientIce { PeerId to; std::string candidate; };
struct ClientRelay { PeerId to; std::string payload; };
struct ClientLeave {};

using ClientMessage = std::variant<ClientJoin, ClientOffer, ClientAnswer,
                                      ClientIce, ClientRelay, ClientLeave>;

// Server → Client messages
struct ServerWelcome { PeerId peer; std::vector<PeerId> peers; };
struct ServerPeerJoined { PeerId peer; };
struct ServerPeerLeft { PeerId peer; };
struct ServerOffer { PeerId from; std::string sdp; };
struct ServerAnswer { PeerId from; std::string sdp; };
struct ServerIce { PeerId from; std::string candidate; };
struct ServerRelay { PeerId from; std::string payload; };
struct ServerError { std::string code; std::string message; };

using ServerMessage = std::variant<ServerWelcome, ServerPeerJoined, ServerPeerLeft,
                                      ServerOffer, ServerAnswer, ServerIce,
                                      ServerRelay, ServerError>;

class SignalingRoom {
 public:
  using ConnID = uint64_t;

  explicit SignalingRoom(SignalingMode mode = SignalingMode::Open)
      : mode_(mode), next_conn_id_(1) {}

  ConnID connect() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto id = next_conn_id_++;
    conns_[id] = ConnState{};
    return id;
  }

  void disconnect(ConnID conn_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = conns_.find(conn_id);
    if (it == conns_.end()) return;
    if (it->second.peer) {
      peer_to_conn_.erase(*it->second.peer);
      // Notify other peers
      ServerPeerLeft msg{*it->second.peer};
      for (auto& [_, conn] : conns_) {
        if (conn.peer && *conn.peer != *it->second.peer) {
          conn.outbound.push_back(msg);
        }
      }
    }
    conns_.erase(conn_id);
  }

  void allow_join(PeerId peer) {
    std::lock_guard<std::mutex> lock(mutex_);
    join_allowed_.insert(peer);
  }

  void allow_signal(PeerId from, PeerId target) {
    std::lock_guard<std::mutex> lock(mutex_);
    signal_allowed_[from].insert(target);
  }

  std::optional<PeerId> get_peer(ConnID conn_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = conns_.find(conn_id);
    if (it == conns_.end() || !it->second.peer) return std::nullopt;
    return it->second.peer;
  }

  std::vector<PeerId> roster() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PeerId> result;
    for (auto& [_, conn] : conns_) {
      if (conn.peer) result.push_back(*conn.peer);
    }
    std::sort(result.begin(), result.end());
    return result;
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (auto& [_, conn] : conns_) {
      if (conn.peer) ++count;
    }
    return count;
  }

  // Process a client message from a connection
  std::vector<ServerMessage> process(ConnID conn_id, const ClientMessage& msg) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ServerMessage> results;

    if (std::holds_alternative<ClientJoin>(msg)) {
      auto& join = std::get<ClientJoin>(msg);
      auto it = conns_.find(conn_id);
      if (it == conns_.end()) {
        results.push_back(ServerError{"bad_message", "unknown connection"});
        return results;
      }
      if (it->second.peer) {
        results.push_back(ServerError{"already_joined", "connection already has a peer"});
        return results;
      }
      if (mode_ == SignalingMode::Allowlist && !join_allowed_.count(join.peer)) {
        results.push_back(ServerError{"permission_denied", "peer not allowed to join"});
        return results;
      }
      // Check for duplicate
      if (peer_to_conn_.count(join.peer)) {
        results.push_back(ServerError{"duplicate_peer", "peer already in room"});
        return results;
      }
      it->second.peer = join.peer;
      peer_to_conn_[join.peer] = conn_id;

      // Welcome with current roster
      std::vector<PeerId> peers;
      for (auto& [_, conn] : conns_) {
        if (conn.peer && *conn.peer != join.peer) peers.push_back(*conn.peer);
      }
      results.push_back(ServerWelcome{join.peer, peers});

      // Notify others
      ServerPeerJoined joined{join.peer};
      for (auto& [_, conn] : conns_) {
        if (conn.peer && *conn.peer != join.peer) {
          conn.outbound.push_back(joined);
        }
      }
    } else if (std::holds_alternative<ClientLeave>(msg)) {
      // Handled by disconnect
    } else {
      // Forward offer/answer/ice/relay to target
      auto it = conns_.find(conn_id);
      if (it == conns_.end() || !it->second.peer) {
        results.push_back(ServerError{"not_joined", "connection has not joined"});
        return results;
      }
      PeerId from = *it->second.peer;
      PeerId target = 0;

      if (std::holds_alternative<ClientOffer>(msg)) {
        auto& m = std::get<ClientOffer>(msg);
        target = m.to;
        forward(target, ServerOffer{from, m.sdp});
      } else if (std::holds_alternative<ClientAnswer>(msg)) {
        auto& m = std::get<ClientAnswer>(msg);
        target = m.to;
        forward(target, ServerAnswer{from, m.sdp});
      } else if (std::holds_alternative<ClientIce>(msg)) {
        auto& m = std::get<ClientIce>(msg);
        target = m.to;
        forward(target, ServerIce{from, m.candidate});
      } else if (std::holds_alternative<ClientRelay>(msg)) {
        auto& m = std::get<ClientRelay>(msg);
        target = m.to;
        forward(target, ServerRelay{from, m.payload});
      }
    }
    return results;
  }

  // Drain outbound messages for a connection
  std::vector<ServerMessage> drain(ConnID conn_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = conns_.find(conn_id);
    if (it == conns_.end()) return {};
    auto result = std::move(it->second.outbound);
    it->second.outbound.clear();
    return result;
  }

 private:
  struct ConnState {
    std::optional<PeerId> peer;
    std::vector<ServerMessage> outbound;
  };

  void forward(PeerId target, ServerMessage msg) {
    auto it = peer_to_conn_.find(target);
    if (it == peer_to_conn_.end()) return;
    conns_[it->second].outbound.push_back(std::move(msg));
  }

  mutable std::mutex mutex_;
  SignalingMode mode_;
  ConnID next_conn_id_;
  std::unordered_map<ConnID, ConnState> conns_;
  std::unordered_map<PeerId, ConnID> peer_to_conn_;
  std::unordered_set<PeerId> join_allowed_;
  std::unordered_map<PeerId, std::unordered_set<PeerId>> signal_allowed_;
};

}  // namespace lazily

#endif  // LAZILY_SIGNALING_HPP
