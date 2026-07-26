// Canonical signaling-room conformance (#lazilycppsignalingroom).
//
// lazily-cpp had NO signaling runner, and `signaling/` sat in the coverage
// allowlist noting that `SignalingRoom::forward` silently dropped frames addressed
// to an unknown peer. That drop is now fixed — the sender gets an `unknown_target`
// error frame — and this replays the canonical transcript so the fix is pinned
// rather than asserted.
//
// The load-bearing invariant in this fixture is ANTI-SPOOF: a client sends a
// directed frame carrying `to`, and the server forwards it with `from` set to the
// SENDER's server-registered peer id, never a client-supplied value. A room that
// echoed a client-supplied `from` would produce the right frame types, the right
// routing and the right roster at every step — only checking `from` against the
// registered id catches it.
//
// `frames.json` is NOT replayed here: it needs signaling wire serde, which this
// binding does not have. It stays in the coverage allowlist with that reason.

#include <lazily/signaling.hpp>

#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "test_json.hpp"
#include "test_require.hpp"
#include "test_spec_fixture.hpp"

using namespace lazily;
using lazily_test::Json;
using lazily_test::parse_json;
using lazily_test::spec_fixture_text;

namespace {

int failures = 0;

void fail(int step, const std::string& what) {
  std::cout << "FAIL [step " << step << "]: " << what << std::endl;
  ++failures;
}

// The frame type name the fixture uses for a given ServerMessage.
std::string frame_type(const ServerMessage& msg) {
  if (std::holds_alternative<ServerWelcome>(msg)) return "welcome";
  if (std::holds_alternative<ServerPeerJoined>(msg)) return "peer-joined";
  if (std::holds_alternative<ServerPeerLeft>(msg)) return "peer-left";
  if (std::holds_alternative<ServerOffer>(msg)) return "offer";
  if (std::holds_alternative<ServerAnswer>(msg)) return "answer";
  if (std::holds_alternative<ServerIce>(msg)) return "ice";
  if (std::holds_alternative<ServerRelay>(msg)) return "relay";
  if (std::holds_alternative<ServerError>(msg)) return "error";
  return "<unknown>";
}

// Compare one produced frame against the fixture's expected frame.
void check_frame(int step, const ServerMessage& got, const Json& want) {
  const std::string want_type = want.find("type")->str;
  const std::string got_type = frame_type(got);
  if (got_type != want_type) {
    fail(step, "frame type `" + got_type + "`, fixture says `" + want_type + "`");
    return;
  }

  if (const auto* w = want.find("peer")) {
    PeerId got_peer = 0;
    if (std::holds_alternative<ServerWelcome>(got)) got_peer = std::get<ServerWelcome>(got).peer;
    else if (std::holds_alternative<ServerPeerJoined>(got)) got_peer = std::get<ServerPeerJoined>(got).peer;
    else if (std::holds_alternative<ServerPeerLeft>(got)) got_peer = std::get<ServerPeerLeft>(got).peer;
    if (got_peer != static_cast<PeerId>(w->number)) {
      fail(step, want_type + ".peer = " + std::to_string(got_peer) + ", fixture says " +
                     std::to_string(static_cast<PeerId>(w->number)));
    }
  }

  // ANTI-SPOOF: `from` must be the sender's server-registered id.
  if (const auto* w = want.find("from")) {
    PeerId got_from = 0;
    if (std::holds_alternative<ServerOffer>(got)) got_from = std::get<ServerOffer>(got).from;
    else if (std::holds_alternative<ServerAnswer>(got)) got_from = std::get<ServerAnswer>(got).from;
    else if (std::holds_alternative<ServerIce>(got)) got_from = std::get<ServerIce>(got).from;
    else if (std::holds_alternative<ServerRelay>(got)) got_from = std::get<ServerRelay>(got).from;
    if (got_from != static_cast<PeerId>(w->number)) {
      fail(step, want_type + ".from = " + std::to_string(got_from) + ", fixture says " +
                     std::to_string(static_cast<PeerId>(w->number)) +
                     " — `from` must be the SENDER's registered id, never client-supplied");
    }
  }

  if (const auto* w = want.find("sdp")) {
    const std::string got_sdp = std::holds_alternative<ServerOffer>(got)
                                    ? std::get<ServerOffer>(got).sdp
                                    : std::get<ServerAnswer>(got).sdp;
    if (got_sdp != w->str) fail(step, "sdp `" + got_sdp + "`, fixture says `" + w->str + "`");
  }
  if (const auto* w = want.find("candidate")) {
    const std::string got_c = std::get<ServerIce>(got).candidate;
    if (got_c != w->str) fail(step, "candidate mismatch");
  }
  if (const auto* w = want.find("code")) {
    const std::string got_code = std::get<ServerError>(got).code;
    if (got_code != w->str) {
      fail(step, "error code `" + got_code + "`, fixture says `" + w->str + "`");
    }
  }
  if (const auto* w = want.find("message")) {
    const std::string got_msg = std::get<ServerError>(got).message;
    if (got_msg != w->str) {
      fail(step, "error message `" + got_msg + "`, fixture says `" + w->str + "`");
    }
  }

  // The roster excludes the joining peer's own id and is ascending.
  if (const auto* w = want.find("peers")) {
    const auto& roster = std::get<ServerWelcome>(got).peers;
    const PeerId self = std::get<ServerWelcome>(got).peer;
    if (roster.size() != w->array.size()) {
      fail(step, "roster size " + std::to_string(roster.size()) + ", fixture says " +
                     std::to_string(w->array.size()));
      return;
    }
    for (size_t i = 0; i < roster.size(); ++i) {
      if (roster[i] != static_cast<PeerId>(w->array[i]->number)) {
        fail(step, "roster[" + std::to_string(i) + "] mismatch");
      }
      if (roster[i] == self) fail(step, "roster must exclude the joining peer's own id");
      if (i > 0 && roster[i - 1] >= roster[i]) fail(step, "roster must be ascending");
    }
  }
}

}  // namespace

int main() {
  const std::string text = spec_fixture_text("signaling", "anti_spoof_session.json");
  const auto doc = parse_json(text);
  REQUIRE(doc && doc->type == Json::Type::Object, "fixture did not parse");
  REQUIRE(doc->find("mode")->str == "open", "this runner drives an open room");

  SignalingRoom room(SignalingMode::Open);
  std::map<std::string, SignalingRoom::ConnID> conns;  // fixture conn label -> real ConnID

  const Json* steps = doc->find("steps");
  REQUIRE(steps != nullptr && !steps->array.empty(), "a replay of zero steps is not a replay");

  int checked_frames = 0;
  for (size_t i = 0; i < steps->array.size(); ++i) {
    const Json& step = *steps->array[i];
    const Json& input = *step.find("input");
    const std::string label = input.find("conn")->str;
    if (conns.find(label) == conns.end()) conns[label] = room.connect();
    const SignalingRoom::ConnID conn = conns[label];

    const Json& recv = *input.find("recv");
    const std::string kind = recv.find("type")->str;

    // Frames the server hands straight back to the sender.
    std::vector<ServerMessage> direct;
    if (kind == "join") {
      direct = room.process(conn, ClientJoin{static_cast<PeerId>(recv.find("peer")->number), {}});
    } else if (kind == "offer") {
      direct = room.process(conn, ClientOffer{static_cast<PeerId>(recv.find("to")->number),
                                              recv.find("sdp")->str});
    } else if (kind == "answer") {
      direct = room.process(conn, ClientAnswer{static_cast<PeerId>(recv.find("to")->number),
                                               recv.find("sdp")->str});
    } else if (kind == "ice") {
      direct = room.process(conn, ClientIce{static_cast<PeerId>(recv.find("to")->number),
                                            recv.find("candidate")->str});
    } else if (kind == "leave") {
      room.disconnect(conn);
    } else {
      fail(static_cast<int>(i), "unhandled input kind `" + kind + "`");
      continue;
    }

    // Collect what each connection can now see: the sender's direct results plus
    // everyone's drained outbound queue.
    std::map<std::string, std::vector<ServerMessage>> delivered;
    for (auto& [lbl, id] : conns) {
      auto drained = room.drain(id);
      if (lbl == label) {
        delivered[lbl].insert(delivered[lbl].end(), direct.begin(), direct.end());
      }
      delivered[lbl].insert(delivered[lbl].end(), drained.begin(), drained.end());
    }

    for (const auto& expect : step.find("expect")->array) {
      const std::string to = expect->find("to")->str;
      const Json& want = *expect->find("frame");
      auto& queue = delivered[to];
      if (queue.empty()) {
        fail(static_cast<int>(i), "expected a `" + want.find("type")->str +
                                      "` frame to conn " + to + " but nothing was delivered");
        continue;
      }
      check_frame(static_cast<int>(i), queue.front(), want);
      queue.erase(queue.begin());
      ++checked_frames;
    }
  }

  if (failures != 0) {
    std::cout << "signaling conformance: " << failures << " failure(s)" << std::endl;
    return 1;
  }
  // Positive proof: a runner that compared nothing would print the same success.
  if (checked_frames < 7) {
    std::cout << "FAIL: only " << checked_frames << " frames compared; the replay is vacuous"
              << std::endl;
    return 1;
  }
  REQUIRE_FIXTURES_LOADED(1);
  std::cout << "signaling conformance: " << checked_frames
            << " frames replayed from the canonical transcript" << std::endl;
  return 0;
}
