#include <lazily/codec.hpp>
#include <lazily/command.hpp>
#include <lazily/msgpack_codec.hpp>
#include <lazily/stdlib.hpp>

#include "test_json.hpp"

#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace {

using lazily_test::Json;

const Json& required(const Json& object, const std::string& key) {
  const auto* value = object.find(key);
  if (value == nullptr) throw std::runtime_error("missing " + key);
  return *value;
}

std::string quote(const std::string& value) {
  std::ostringstream out;
  out << '"';
  for (const unsigned char ch : value) {
    switch (ch) {
    case '"':
      out << "\\\"";
      break;
    case '\\':
      out << "\\\\";
      break;
    case '\b':
      out << "\\b";
      break;
    case '\f':
      out << "\\f";
      break;
    case '\n':
      out << "\\n";
      break;
    case '\r':
      out << "\\r";
      break;
    case '\t':
      out << "\\t";
      break;
    default:
      if (ch < 0x20) {
        constexpr char hex[] = "0123456789abcdef";
        out << "\\u00" << hex[ch >> 4] << hex[ch & 0x0f];
      } else {
        out << static_cast<char>(ch);
      }
    }
  }
  out << '"';
  return out.str();
}

std::string stamp_json(const lazily::WireStamp& stamp) {
  return "{\"wall_time\":" + std::to_string(stamp.wall_time) +
         ",\"logical\":" + std::to_string(stamp.logical) +
         ",\"peer\":" + std::to_string(stamp.peer) + "}";
}

std::string state_json(const lazily::IpcValue& state) {
  if (!std::holds_alternative<lazily::IpcValueInline>(state)) {
    throw std::runtime_error("SharedBlob is outside this peer's semantic-suite profile");
  }
  std::ostringstream out;
  out << "{\"Inline\":[";
  const auto& bytes = std::get<lazily::IpcValueInline>(state).bytes;
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index != 0) out << ',';
    out << static_cast<unsigned>(bytes[index]);
  }
  out << "]}";
  return out.str();
}

std::string op_json(const lazily::CrdtOp& op) {
  return "{\"node\":" + std::to_string(op.node) +
         ",\"key\":" + (op.key ? quote(op.key->to_wire()) : "null") +
         ",\"stamp\":" + stamp_json(op.stamp) + ",\"state\":" + state_json(op.state) + "}";
}

std::string sync_json(const lazily::CrdtSync& sync) {
  std::ostringstream out;
  out << "{\"CrdtSync\":{\"frontier\":[";
  for (std::size_t index = 0; index < sync.frontier.size(); ++index) {
    if (index != 0) out << ',';
    const auto& entry = sync.frontier[index];
    out << '[' << entry.peer << ',' << stamp_json(entry.stamp) << ']';
  }
  out << "],\"ops\":[";
  for (std::size_t index = 0; index < sync.ops.size(); ++index) {
    if (index != 0) out << ',';
    out << op_json(sync.ops[index]);
  }
  out << "]}}";
  return out.str();
}

lazily::WireStamp parse_stamp(const Json& value) {
  return {
      required(value, "wall_time").as_int(),
      required(value, "logical").as_int(),
      required(value, "peer").as_int(),
  };
}

lazily::IpcValue parse_state(const Json& value) {
  const auto& bytes = required(value, "Inline");
  if (!bytes.is_array()) throw std::runtime_error("state.Inline must be an array");
  std::vector<std::uint8_t> result;
  result.reserve(bytes.array.size());
  for (const auto& byte : bytes.array) {
    const auto number = byte->as_int();
    if (number < 0 || number > 255) throw std::runtime_error("Inline byte out of range");
    result.push_back(static_cast<std::uint8_t>(number));
  }
  return lazily::IpcValueInline{std::move(result)};
}

lazily::CrdtOp parse_op(const Json& value) {
  std::optional<lazily::NodeKey> key;
  const auto& key_json = required(value, "key");
  if (!key_json.is_null()) {
    key = lazily::NodeKey::create(key_json.as_str());
    if (!key) throw std::runtime_error("invalid NodeKey");
  }
  return {
      required(value, "node").as_int(),
      std::move(key),
      parse_stamp(required(value, "stamp")),
      parse_state(required(value, "state")),
  };
}

lazily::CrdtSync parse_sync(const Json& frame) {
  const auto& value = required(frame, "CrdtSync");
  const auto& frontier_json = required(value, "frontier");
  const auto& ops_json = required(value, "ops");
  if (!frontier_json.is_array() || !ops_json.is_array()) {
    throw std::runtime_error("CrdtSync arrays are malformed");
  }

  lazily::CrdtSync sync;
  for (const auto& entry : frontier_json.array) {
    if (!entry->is_array() || entry->array.size() != 2) {
      throw std::runtime_error("frontier entry must be [peer, stamp]");
    }
    sync.frontier.push_back({entry->array[0]->as_int(), parse_stamp(*entry->array[1])});
  }
  for (const auto& op : ops_json.array)
    sync.ops.push_back(parse_op(*op));
  return sync;
}

lazily::CrdtSync unwrap_sync(lazily::IpcMessage decoded, const char* codec) {
  if (!std::holds_alternative<lazily::IpcMessageCrdtSync>(decoded)) {
    throw std::runtime_error(std::string(codec) + " codec changed CrdtSync variant");
  }
  return std::get<lazily::IpcMessageCrdtSync>(std::move(decoded)).value;
}

// The `msgpack` codec token this peer advertises in `hello`. protocol.md
// § Frame codecs names ONE wire for that token, and it is the externally tagged
// named-field frame in include/lazily/msgpack_codec.hpp — NOT codec.hpp's
// private internally-tagged framing (#lzcppmsgpackwire). Every frame this peer
// hands to or takes from another binding goes through that wire, so the
// advertised capability and the bytes agree.
lazily::CrdtSync normalize_msgpack(const lazily::CrdtSync& sync) {
  const lazily::IpcMessage message = lazily::IpcMessageCrdtSync{sync};
  return unwrap_sync(lazily::decode_msgpack(lazily::encode_msgpack(message)), "msgpack");
}

// The private framing, exercised only by the self-check. It is a lazily-cpp
// internal serialization and is never advertised as a codec token.
lazily::CrdtSync normalize_private_codec(const lazily::CrdtSync& sync, bool positional = false) {
  const lazily::IpcMessage message = lazily::IpcMessageCrdtSync{sync};
  const auto bytes = positional ? lazily::encode_positional(message) : lazily::encode(message);
  return unwrap_sync(lazily::decode(bytes), "private");
}

std::uint64_t u64_field(const Json& value, const std::string& field) {
  return lazily_test::json_u64(required(value, field));
}

bool bool_field(const Json& value, const std::string& field) {
  return required(value, field).as_bool();
}

std::string timer_error(lazily::TimerError error) {
  return error == lazily::TimerError::deadline_overflow ? "deadline_overflow" : "clock_regression";
}

std::string timeout_outcome(lazily::TimeoutObservation<std::string>::Outcome outcome) {
  using Outcome = lazily::TimeoutObservation<std::string>::Outcome;
  switch (outcome) {
  case Outcome::pending:
    return "pending";
  case Outcome::completed:
    return "completed";
  case Outcome::timed_out:
    return "timed_out";
  case Outcome::cancelled:
    return "cancelled";
  case Outcome::unavailable:
    return "unavailable";
  }
  return "unavailable";
}

std::string barrier_outcome(lazily::RevisionBarrierObservation::Outcome outcome) {
  using Outcome = lazily::RevisionBarrierObservation::Outcome;
  switch (outcome) {
  case Outcome::pending:
    return "pending";
  case Outcome::satisfied:
    return "satisfied";
  case Outcome::timed_out:
    return "timed_out";
  case Outcome::cancelled:
    return "cancelled";
  case Outcome::disposed:
    return "disposed";
  case Outcome::unavailable:
    return "unavailable";
  }
  return "unavailable";
}

lazily::TimeoutCancellation cancellation(const std::string& value) {
  if (value == "cancelled") return lazily::TimeoutCancellation::cancelled;
  if (value == "unavailable") return lazily::TimeoutCancellation::unavailable;
  return lazily::TimeoutCancellation::pending;
}

struct StdlibFeature {
  explicit StdlibFeature(std::string feature) : name(std::move(feature)) {}

  std::string step(const Json& value) {
    if (name == "stdlib_timer_v1")
      last = timer_step(value);
    else if (name == "stdlib_timeout_v1")
      last = timeout_step(value);
    else if (name == "stdlib_revision_barrier_v1")
      last = barrier_step(value);
    else
      throw std::runtime_error("unsupported feature " + name);
    return last;
  }

  std::string timer_step(const Json& step) {
    const auto op = required(step, "op").as_str();
    if (op == "start") {
      auto started = lazily::Timer::start(u64_field(step, "now"), u64_field(step, "duration"));
      timer = std::move(started.first);
      if (started.second)
        return "{\"outcome\":\"unavailable\",\"reason\":" + quote(timer_error(*started.second)) +
               "}";
      deadline =
          lazily::checked_deadline(u64_field(step, "now"), u64_field(step, "duration")).value;
      return "{\"outcome\":\"pending\",\"deadline\":" + std::to_string(deadline) + "}";
    }
    if (op != "observe") throw std::runtime_error("unsupported timer feature step " + op);
    if (!timer) throw std::runtime_error("timer feature is not started");
    const auto observation = timer->observe(u64_field(step, "now"));
    if (observation.outcome == lazily::TimerObservation::Outcome::fired)
      return "{\"outcome\":\"fired\",\"fired_at\":" + std::to_string(observation.fired_at) + "}";
    if (observation.outcome == lazily::TimerObservation::Outcome::unavailable)
      return "{\"outcome\":\"unavailable\",\"reason\":" + quote(timer_error(*observation.error)) +
             ",\"deadline\":" + std::to_string(observation.deadline) + "}";
    return "{\"outcome\":\"pending\",\"deadline\":" + std::to_string(observation.deadline) + "}";
  }

  std::string timeout_step(const Json& step) {
    const auto op = required(step, "op").as_str();
    if (op == "start") {
      auto started =
          lazily::Timeout<std::string>::start(u64_field(step, "now"), u64_field(step, "duration"));
      if (started.second) throw std::runtime_error(timer_error(*started.second));
      timeout = std::move(started.first);
      deadline =
          lazily::checked_deadline(u64_field(step, "now"), u64_field(step, "duration")).value;
      return "{\"outcome\":\"pending\",\"deadline\":" + std::to_string(deadline) + "}";
    }
    if (op != "poll") throw std::runtime_error("unsupported timeout feature step " + op);
    if (!timeout) throw std::runtime_error("timeout feature is not started");
    const auto operation = required(step, "operation").as_str();
    const auto cancellation_state = required(step, "cancellation").as_str();
    std::uint64_t operation_calls = 0;
    std::uint64_t cancellation_calls = 0;
    const auto observation = timeout->poll(
        u64_field(step, "now"),
        [&] {
          ++operation_calls;
          if (operation == "completed")
            return lazily::TimeoutOperation<std::string>::completed(
                required(step, "value").as_str());
          if (operation == "unavailable")
            return lazily::TimeoutOperation<std::string>::unavailable();
          return lazily::TimeoutOperation<std::string>::pending();
        },
        [&] {
          ++cancellation_calls;
          return cancellation(cancellation_state);
        });
    std::string out = "{\"outcome\":" + quote(timeout_outcome(observation.outcome));
    if (observation.outcome == lazily::TimeoutObservation<std::string>::Outcome::pending)
      out += ",\"deadline\":" + std::to_string(observation.deadline);
    if (observation.outcome == lazily::TimeoutObservation<std::string>::Outcome::completed)
      out += ",\"value\":" + quote(observation.value);
    if (!observation.reason.empty()) out += ",\"reason\":" + quote(observation.reason);
    out += ",\"operation_calls\":" + std::to_string(operation_calls) +
           ",\"cancellation_calls\":" + std::to_string(cancellation_calls) + "}";
    return out;
  }

  std::string barrier_step(const Json& step) {
    const auto op = required(step, "op").as_str();
    lazily::RevisionBarrierObservation observation;
    std::uint64_t cancellation_calls = 0;
    if (op == "start") {
      const auto& deadline_value = required(step, "deadline");
      std::optional<std::uint64_t> parsed_deadline;
      if (!deadline_value.is_null()) parsed_deadline = lazily_test::json_u64(deadline_value);
      barrier = std::make_unique<lazily::RevisionBarrier>(
          u64_field(step, "revision"), u64_field(step, "required_revision"), parsed_deadline);
      observation = barrier->receipt("");
    } else {
      if (!barrier) throw std::runtime_error("barrier feature is not started");
      if (op == "observe") {
        observation = barrier->observe(u64_field(step, "now"), bool_field(step, "predicate"), [&] {
          ++cancellation_calls;
          return cancellation(required(step, "cancellation").as_str());
        });
      } else if (op == "register_recheck") {
        observation =
            barrier->register_recheck(u64_field(step, "now"), u64_field(step, "observed_revision"),
                                      bool_field(step, "predicate"));
      } else if (op == "advance") {
        observation = barrier->advance(u64_field(step, "revision"), bool_field(step, "predicate"));
      } else if (op == "dispose") {
        observation = barrier->dispose();
      } else if (op == "receipt") {
        observation = barrier->receipt(required(step, "key").as_str());
      } else {
        throw std::runtime_error("unsupported revision barrier feature step " + op);
      }
    }
    std::string out = "{\"outcome\":" + quote(barrier_outcome(observation.outcome)) +
                      ",\"revision\":" + std::to_string(observation.revision) +
                      ",\"generation\":" + std::to_string(observation.generation);
    if (!observation.reason.empty()) out += ",\"reason\":" + quote(observation.reason);
    if (op == "observe") out += ",\"cancellation_calls\":" + std::to_string(cancellation_calls);
    return out + "}";
  }

  std::string name;
  std::unique_ptr<lazily::Timer> timer;
  std::unique_ptr<lazily::Timeout<std::string>> timeout;
  std::unique_ptr<lazily::RevisionBarrier> barrier;
  std::uint64_t deadline = 0;
  std::string last;
};

class Peer {
public:
  std::string handle(const Json& request) {
    const auto command = required(request, "cmd").as_str();
    if (command == "hello") return hello(request);
    if (command == "local_set") return local_set(request);
    if (command == "deliver") return deliver(request);
    if (command == "snapshot") return snapshot();
    if (command == "feature_reset") return feature_reset(request);
    if (command == "feature_step") return feature_step(request);
    if (command == "feature_observe") return feature_observe(request);
    if (command == "bye") {
      stopping_ = true;
      return "{\"ok\":true}";
    }
    if (command.rfind("link_", 0) == 0) {
      return "{\"ok\":false,\"error\":\"unsupported channel\","
             "\"unsupported\":true}";
    }
    throw std::runtime_error("unknown command " + command);
  }

  bool stopping() const { return stopping_; }

private:
  std::string hello(const Json& request) {
    if (required(request, "protocol_version").as_int() != 1) {
      throw std::runtime_error("unsupported protocol_version");
    }
    peer_id_ = required(request, "peer").as_int();
    runtime_ = std::make_unique<lazily::CrdtPlaneRuntime>(*peer_id_);
    logical_ = 0;
    stdlib_.clear();
    return "{\"ok\":true,\"binding\":\"lazily-cpp\","
           "\"version\":\"0.27.0\",\"protocol_version\":1,"
           "\"features\":[\"distributed_crdt\",\"stdlib_timer_v1\","
           "\"stdlib_timeout_v1\",\"stdlib_revision_barrier_v1\"],"
           // Both MUST-level frame codecs are implemented and replayed through
           // the canonical corpus: `json` by include/lazily/json_codec.hpp
           // (#lzcppjsoncodec) and `msgpack` by include/lazily/msgpack_codec.hpp
           // (#lzcppmsgpackwire). Neither is a carve-out any more.
           "\"codecs\":[\"json\",\"msgpack\"],\"channels\":[],"
           "\"channel_variants\":{},\"platform_profile\":\"portable\","
           "\"carve_outs\":[\"shared_blob\",\"transport_links\"]}";
  }

  std::string local_set(const Json& request) {
    ensure_started();
    std::optional<lazily::NodeKey> key;
    const auto& key_json = required(request, "key");
    if (!key_json.is_null()) {
      key = lazily::NodeKey::create(key_json.as_str());
      if (!key) throw std::runtime_error("invalid NodeKey");
    }
    const lazily::WireStamp stamp{required(request, "at").as_int(), ++logical_, *peer_id_};
    const lazily::CrdtOp op{
        required(request, "node").as_int(),
        std::move(key),
        stamp,
        parse_state(required(request, "state")),
    };
    lazily::CrdtSync local{{{*peer_id_, stamp}}, {op}};
    if (runtime_->ingest(local) != 1) {
      throw std::runtime_error("production runtime rejected fresh local op");
    }
    local.frontier = runtime_->frontier_entries();
    return "{\"ok\":true,\"frame\":" + sync_json(normalize_msgpack(local)) + "}";
  }

  std::string deliver(const Json& request) {
    ensure_started();
    const auto sync = normalize_msgpack(parse_sync(required(request, "frame")));
    return "{\"ok\":true,\"applied\":" + std::to_string(runtime_->ingest(sync)) + "}";
  }

  std::string snapshot() const {
    ensure_started();
    std::ostringstream out;
    out << "{\"ok\":true,\"cells\":[";
    const auto cells = runtime_->converged();
    for (std::size_t index = 0; index < cells.size(); ++index) {
      if (index != 0) out << ',';
      const auto& cell = cells[index];
      out << "{\"node\":" << cell.node << ",\"key\":" << (cell.key ? quote(*cell.key) : "null")
          << ",\"state\":" << state_json(cell.state) << '}';
    }
    out << "]}";
    return out.str();
  }

  std::string feature_reset(const Json& request) {
    const auto feature = required(request, "feature").as_str();
    if (feature != "stdlib_timer_v1" && feature != "stdlib_timeout_v1" &&
        feature != "stdlib_revision_barrier_v1")
      return "{\"ok\":false,\"error\":" + quote("unsupported feature " + feature) +
             ",\"unsupported\":true}";
    stdlib_[feature] = std::make_unique<StdlibFeature>(feature);
    return "{\"ok\":true,\"feature\":" + quote(feature) + "}";
  }

  std::string feature_step(const Json& request) {
    const auto feature = required(request, "feature").as_str();
    const auto found = stdlib_.find(feature);
    if (found == stdlib_.end())
      throw std::runtime_error("feature " + feature + " must be reset before stepping");
    const auto observation = found->second->step(required(request, "step"));
    return "{\"ok\":true,\"feature\":" + quote(feature) + ",\"observation\":" + observation + "}";
  }

  std::string feature_observe(const Json& request) {
    const auto feature = required(request, "feature").as_str();
    const auto found = stdlib_.find(feature);
    if (found == stdlib_.end())
      throw std::runtime_error("feature " + feature + " must be reset before observation");
    if (found->second->last.empty())
      throw std::runtime_error("feature " + feature + " has no observation");
    return "{\"ok\":true,\"feature\":" + quote(feature) +
           ",\"observation\":" + found->second->last + "}";
  }

  void ensure_started() const {
    if (!runtime_ || !peer_id_) throw std::runtime_error("hello must run first");
  }

  std::optional<lazily::PeerId> peer_id_;
  std::unique_ptr<lazily::CrdtPlaneRuntime> runtime_;
  std::map<std::string, std::unique_ptr<StdlibFeature>> stdlib_;
  std::int64_t logical_ = 0;
  bool stopping_ = false;
};

void self_check() {
  const lazily::WireStamp stamp{10, 1, 1};
  const lazily::CrdtSync original{
      {{1, stamp}},
      {{7, std::nullopt, stamp, lazily::IpcValueInline{{65}}}},
  };
  const auto spec_msgpack = normalize_msgpack(original);
  const auto keyed = normalize_private_codec(original);
  const auto positional = normalize_private_codec(original, true);
  if (spec_msgpack.ops.size() != 1 || keyed.ops.size() != 1 || positional.ops.size() != 1 ||
      spec_msgpack.ops[0].key || keyed.ops[0].key || positional.ops[0].key) {
    throw std::runtime_error("MessagePack variants lost the canonical null key");
  }
  // The advertised `msgpack` token is the SPEC wire, so prove the bytes are the
  // externally tagged frame rather than the private envelope this peer used to
  // normalize through (#lzcppmsgpackwire).
  const auto spec_bytes =
      lazily::encode_msgpack(lazily::IpcMessage{lazily::IpcMessageCrdtSync{original}});
  const auto spec_view = lazily::msgpack_to_json(spec_bytes);
  if (!spec_view.is_object() || spec_view.object.size() != 1 ||
      spec_view.object.front().first != "CrdtSync") {
    throw std::runtime_error("advertised msgpack codec is not the externally tagged spec wire");
  }

  lazily::CrdtPlaneRuntime runtime(1);
  if (runtime.ingest(keyed) != 1 || runtime.ingest(positional) != 0) {
    throw std::runtime_error("CRDT production runtime is not idempotent");
  }
  const auto cells = runtime.converged();
  if (cells.size() != 1 ||
      std::get<lazily::IpcValueInline>(cells[0].state).bytes != std::vector<std::uint8_t>{65}) {
    throw std::runtime_error("CRDT snapshot did not converge");
  }

  Peer peer;
  const auto hello =
      lazily_test::parse_json("{\"cmd\":\"hello\",\"peer\":1,\"protocol_version\":1}");
  if (peer.handle(*hello).find("\"ok\":true") == std::string::npos)
    throw std::runtime_error("peer hello self-check failed");
  const std::pair<const char*, const char*> feature_cases[] = {
      {"stdlib_timer_v1", "{\"op\":\"start\",\"now\":0,\"duration\":0}"},
      {"stdlib_timeout_v1", "{\"op\":\"start\",\"now\":0,\"duration\":1}"},
      {"stdlib_revision_barrier_v1", "{\"op\":\"start\",\"revision\":1,\"required_revision\":1,"
                                     "\"deadline\":null}"},
  };
  for (const auto& [feature, step] : feature_cases) {
    const auto reset =
        lazily_test::parse_json("{\"cmd\":\"feature_reset\",\"feature\":" + quote(feature) + "}");
    peer.handle(*reset);
    const auto request = lazily_test::parse_json(
        "{\"cmd\":\"feature_step\",\"feature\":" + quote(feature) + ",\"step\":" + step + "}");
    if (peer.handle(*request).find("\"ok\":true") == std::string::npos)
      throw std::runtime_error(std::string(feature) + " feature self-check failed");
  }
}

} // namespace

int main(int argc, char** argv) {
  try {
    if (argc > 1 && std::string(argv[1]) == "--self-check") {
      self_check();
      std::cout << "lazily-cpp interop peer self-check OK\n";
      return 0;
    }

    Peer peer;
    std::string line;
    while (std::getline(std::cin, line)) {
      try {
        const auto request = lazily_test::parse_json(line);
        std::cout << peer.handle(*request) << '\n' << std::flush;
      } catch (const std::exception& error) {
        std::cout << "{\"ok\":false,\"error\":" << quote(error.what()) << "}\n" << std::flush;
      }
      if (peer.stopping()) break;
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "lazily-cpp interop peer: " << error.what() << '\n';
    return 1;
  }
}
