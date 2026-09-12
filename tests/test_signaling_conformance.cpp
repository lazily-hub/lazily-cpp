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
// Every ELEMENT of an array-valued `expect` is its own assertion block
// (#lzarrayelementsites), so the 12 expected frames across these 8 steps are
// bound INDIVIDUALLY: `steps[2].expect[0]` and `steps[2].expect[2]` are separate
// sites, and a runner that stopped asserting one of them fails naming that
// element. Before that, the array contributed no site at all and only a falsified
// VALUE was ever caught.
//
// `frames.json` is NOT replayed here: it needs signaling wire serde, which this
// binding does not have. It stays in the coverage allowlist with that reason.

#include <lazily/signaling.hpp>

#include <cstddef>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "test_assertion_keys.hpp"
#include "test_json.hpp"
#include "test_require.hpp"
#include "test_spec_fixture.hpp"

using namespace lazily;
using lazily_test::Json;
using lazily_test::parse_json;
using lazily_test::spec_fixture_text;

namespace {

int failures = 0;

// Observations the fixture's `assertions` block makes claims about. Collected
// during the replay so the block can be evaluated rather than skipped: comparing
// frames against the transcript makes those claims TRUE, but it never makes them
// ASSERTED -- it only says this binding emits what was recorded.
std::vector<ServerWelcome> observed_welcomes;
std::vector<PeerId> observed_forward_from;
std::set<PeerId> registered_peers;

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

// One produced frame, flattened to the field NAMES and values it carries.
//
// `ServerMessage` is a variant, so every read of a field is a read of one
// alternative: the old code reached straight into `std::get<ServerError>` for
// `code` on the strength of the FIXTURE naming that key, which throws rather
// than fails when the room produced a different frame. Flattening first makes
// the produced key set a first-class value, which is what the key-set assertion
// below needs, and removes every ill-typed `std::get`.
struct ProducedFrame {
  std::set<std::string> keys{"type"};
  std::string type;
  PeerId peer = 0;
  PeerId from = 0;
  std::string sdp;
  std::string candidate;
  std::string payload;
  std::string code;
  std::string message;
  std::vector<PeerId> peers;
};

ProducedFrame flatten(const ServerMessage& msg) {
  ProducedFrame out;
  out.type = frame_type(msg);
  if (const auto* m = std::get_if<ServerWelcome>(&msg)) {
    out.keys.insert({"peer", "peers"});
    out.peer = m->peer;
    out.peers = m->peers;
  } else if (const auto* m = std::get_if<ServerPeerJoined>(&msg)) {
    out.keys.insert("peer");
    out.peer = m->peer;
  } else if (const auto* m = std::get_if<ServerPeerLeft>(&msg)) {
    out.keys.insert("peer");
    out.peer = m->peer;
  } else if (const auto* m = std::get_if<ServerOffer>(&msg)) {
    out.keys.insert({"from", "sdp"});
    out.from = m->from;
    out.sdp = m->sdp;
  } else if (const auto* m = std::get_if<ServerAnswer>(&msg)) {
    out.keys.insert({"from", "sdp"});
    out.from = m->from;
    out.sdp = m->sdp;
  } else if (const auto* m = std::get_if<ServerIce>(&msg)) {
    out.keys.insert({"from", "candidate"});
    out.from = m->from;
    out.candidate = m->candidate;
  } else if (const auto* m = std::get_if<ServerRelay>(&msg)) {
    out.keys.insert({"from", "payload"});
    out.from = m->from;
    out.payload = m->payload;
  } else if (const auto* m = std::get_if<ServerError>(&msg)) {
    out.keys.insert({"code", "message"});
    out.code = m->code;
    out.message = m->message;
  }
  return out;
}

std::string render_names(const std::set<std::string>& names) {
  std::string out;
  for (const auto& name : names)
    out += (out.empty() ? "" : ", ") + name;
  return out.empty() ? "(none)" : out;
}

std::string render_routing(const std::map<std::string, std::size_t>& counts) {
  std::string out;
  for (const auto& kv : counts)
    out += (out.empty() ? "" : ", ") + kv.first + "x" + std::to_string(kv.second);
  return out.empty() ? "(nothing)" : out;
}

// Compare one produced frame against the fixture's `frame` sub-block, through the
// CHILD tracker `with_sub` hands out.
//
// Two things this closes (#lzarrayelementsites). The old form took the fixture's
// `frame` object as raw JSON and asked it for the keys it happened to know
// about, so (a) a field the room really emitted that the fixture omits was
// compared by NOTHING -- splitting a whole-frame equality into per-key equalities
// is strictly weaker than the whole unless the key set is asserted beside it --
// and (b) a key the fixture names that this function does not handle was silently
// skipped. The child tracker owns every sub-key, which is (b); the key-set
// comparison below is (a), and it runs AHEAD of any value comparison.
void check_frame(int step, const ProducedFrame& got, lazily_test::AssertionKeys& frame) {
  std::set<std::string> declared;
  for (const auto& name : frame.keys())
    declared.insert(name);
  if (declared != got.keys) {
    std::string missing;
    for (const auto& name : declared)
      if (got.keys.count(name) == 0) missing += (missing.empty() ? "" : ", ") + name;
    std::string extra;
    for (const auto& name : got.keys)
      if (declared.count(name) == 0) extra += (extra.empty() ? "" : ", ") + name;
    fail(step, "frame key set: the room produced {" + render_names(got.keys) +
                   "}, the fixture declares {" + render_names(declared) + "}" +
                   (missing.empty() ? "" : " -- declared, never produced: " + missing) +
                   (extra.empty() ? "" : " -- produced, not declared: " + extra));
  }

  frame.assert_key("type", got.type);
  frame.assert_key_if_present("peer", got.peer);

  // ANTI-SPOOF: `from` must be the sender's server-registered id.
  frame.assert_key_with_if_present("from", [&](const Json& want) {
    const PeerId expected = static_cast<PeerId>(want.as_int());
    if (got.from == expected) return true;
    fail(step, got.type + ".from = " + std::to_string(got.from) + ", fixture says " +
                   std::to_string(expected) +
                   " — `from` must be the SENDER's registered id, never client-supplied");
    return false;
  });

  frame.assert_key_if_present("sdp", got.sdp);
  frame.assert_key_if_present("candidate", got.candidate);
  frame.assert_key_if_present("payload", got.payload);
  frame.assert_key_if_present("code", got.code);
  frame.assert_key_if_present("message", got.message);

  // The roster excludes the joining peer's own id and is ascending.
  frame.assert_key_with_if_present("peers", [&](const Json& want) {
    if (got.peers.size() != want.array.size()) {
      fail(step, "roster size " + std::to_string(got.peers.size()) + ", fixture says " +
                     std::to_string(want.array.size()));
      return false;
    }
    bool ok = true;
    for (std::size_t i = 0; i < got.peers.size(); ++i) {
      if (got.peers[i] != static_cast<PeerId>(want.array[i]->as_int())) {
        fail(step, "roster[" + std::to_string(i) + "] mismatch");
        ok = false;
      }
      if (got.peers[i] == got.peer) {
        fail(step, "roster must exclude the joining peer's own id");
        ok = false;
      }
      if (i > 0 && got.peers[i - 1] >= got.peers[i]) {
        fail(step, "roster must be ascending");
        ok = false;
      }
    }
    return ok;
  });
}

} // namespace

int main() {
  const std::string text = spec_fixture_text("signaling", "anti_spoof_session.json");
  const auto doc = parse_json(text);
  REQUIRE(doc && doc->type == Json::Type::Object, "fixture did not parse");
  REQUIRE(doc->find("mode")->str == "open", "this runner drives an open room");

  SignalingRoom room(SignalingMode::Open);
  std::map<std::string, SignalingRoom::ConnID> conns; // fixture conn label -> real ConnID

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
      registered_peers.insert(static_cast<PeerId>(recv.find("peer")->number));
      direct = room.process(conn, ClientJoin{static_cast<PeerId>(recv.find("peer")->number), {}});
    } else if (kind == "offer") {
      direct = room.process(
          conn, ClientOffer{static_cast<PeerId>(recv.find("to")->number), recv.find("sdp")->str});
    } else if (kind == "answer") {
      direct = room.process(
          conn, ClientAnswer{static_cast<PeerId>(recv.find("to")->number), recv.find("sdp")->str});
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

    const Json& expect = *step.find("expect");

    // ROUTING, as a multiset the run produced against one the fixture declares,
    // BOTH directions and independent of the order the fixture lists its frames
    // in (this transcript deliberately imposes no order on peer-joined
    // broadcasts). The per-element loop below indexes `delivered` BY the
    // fixture's own `to`, so on its own it can only ever see the frames the
    // fixture asked about: a frame the room delivered to a conn the fixture
    // never routes to, or a second frame to one it routes once, was consumed by
    // nothing and reported by nothing.
    std::map<std::string, std::size_t> produced_routing;
    for (const auto& kv : delivered)
      if (!kv.second.empty()) produced_routing[kv.first] = kv.second.size();
    std::map<std::string, std::size_t> declared_routing;
    for (const auto& element : expect.array)
      ++declared_routing[element->find("to")->str];
    if (produced_routing != declared_routing) {
      fail(static_cast<int>(i), "routing: the room delivered " + render_routing(produced_routing) +
                                    ", the fixture routes " + render_routing(declared_routing));
    }

    for (std::size_t k = 0; k < expect.array.size(); ++k) {
      const Json& element = *expect.array[k];
      const std::string to = element.find("to")->str;
      auto& queue = delivered[to];
      if (queue.empty()) {
        fail(static_cast<int>(i), "expected a `" + element.find("frame")->find("type")->str +
                                      "` frame to conn " + to + " but nothing was delivered");
        continue;
      }
      const ServerMessage& got = queue.front();
      if (std::holds_alternative<ServerWelcome>(got))
        observed_welcomes.push_back(std::get<ServerWelcome>(got));
      else if (std::holds_alternative<ServerOffer>(got))
        observed_forward_from.push_back(std::get<ServerOffer>(got).from);
      else if (std::holds_alternative<ServerAnswer>(got))
        observed_forward_from.push_back(std::get<ServerAnswer>(got).from);
      else if (std::holds_alternative<ServerIce>(got))
        observed_forward_from.push_back(std::get<ServerIce>(got).from);
      else if (std::holds_alternative<ServerRelay>(got))
        observed_forward_from.push_back(std::get<ServerRelay>(got).from);

      // Rung 0 for THIS element (#lzarrayelementsites). The `where` is spelled
      // from the loader's own coordinates, so a failure names the site the walk
      // in tests/test_assertion_keys.hpp declared.
      const ProducedFrame produced = flatten(got);
      lazily_test::AssertionKeys frame_block("signaling/anti_spoof_session.json steps[" +
                                                 std::to_string(i) + "].expect[" +
                                                 std::to_string(k) + "]",
                                             element);
      // `to` is the ROUTING claim, and it is asserted against the multiset the
      // room produced rather than read to index into it.
      frame_block.assert_key_with("to", [&](const Json& want) {
        const std::string target = lazily_test::json_string(want);
        const auto produced_it = produced_routing.find(target);
        if (produced_it != produced_routing.end() &&
            produced_it->second == declared_routing[target]) {
          return true;
        }
        fail(static_cast<int>(i),
             "routing target `" + target + "`: the room delivered it " +
                 std::to_string(produced_it == produced_routing.end() ? 0 : produced_it->second) +
                 " frame(s), the fixture routes " + std::to_string(declared_routing[target]));
        return false;
      });
      frame_block.with_sub("frame", [&](lazily_test::AssertionKeys& frame) {
        check_frame(static_cast<int>(i), produced, frame);
      });
      frame_block.finish();

      queue.erase(queue.begin());
      ++checked_frames;
    }
  }

  // The fixture's top-level `assertions` block. It was on disk and read by
  // nobody: this runner hardcoded the equivalent checks inside `check_frame`, so
  // a fixture that renamed, added or flipped a claim here changed nothing
  // (#lzassertunknownkeys).
  {
    lazily_test::AssertionKeys keys("signaling/anti_spoof_session.json assertions",
                                    lazily_test::json_member(*doc, "assertions"));
    keys.assert_key_with_if_present("roster_excludes_self", [&](const Json& want) {
      REQUIRE(!observed_welcomes.empty(), "roster_excludes_self: no welcome observed");
      bool excludes = true;
      for (const auto& w : observed_welcomes)
        for (const PeerId p : w.peers)
          if (p == w.peer) excludes = false;
      if (excludes == lazily_test::fixture_flag(want, "roster_excludes_self")) return true;
      fail(-1, "roster_excludes_self");
      return false;
    });
    keys.assert_key_with_if_present("roster_sorted_ascending", [&](const Json& want) {
      REQUIRE(!observed_welcomes.empty(), "roster_sorted_ascending: no welcome observed");
      bool sorted = true;
      for (const auto& w : observed_welcomes)
        for (size_t i = 1; i < w.peers.size(); ++i)
          if (w.peers[i - 1] >= w.peers[i]) sorted = false;
      if (sorted == lazily_test::fixture_flag(want, "roster_sorted_ascending")) return true;
      fail(-1, "roster_sorted_ascending");
      return false;
    });
    keys.assert_key_with_if_present("forwarded_from_is_server_registered", [&](const Json& want) {
      REQUIRE(!observed_forward_from.empty(),
              "forwarded_from_is_server_registered: nothing was forwarded");
      bool ok = true;
      for (const PeerId from : observed_forward_from)
        if (registered_peers.count(from) == 0) ok = false;
      if (ok == lazily_test::fixture_flag(want, "forwarded_from_is_server_registered")) return true;
      fail(-1, "forwarded_from_is_server_registered");
      return false;
    });
    keys.finish();
  }

  if (failures != 0) {
    std::cout << "signaling conformance: " << failures << " failure(s)" << std::endl;
    return 1;
  }
  // Positive proof: a runner that compared nothing would print the same success.
  // The floor is the fixture's whole population now that each of the 12 elements
  // is a bound site of its own -- a skipped element fails rung 0 by name one
  // `make check` step later, and fails HERE immediately.
  if (checked_frames < 12) {
    std::cout << "FAIL: only " << checked_frames << " frames compared; the replay is vacuous"
              << std::endl;
    return 1;
  }
  REQUIRE_FIXTURES_LOADED(1);
  std::cout << "signaling conformance: " << checked_frames
            << " frames replayed from the canonical transcript" << std::endl;
  return 0;
}
