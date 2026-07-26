#include <lazily/codec.hpp>
#include <lazily/command.hpp>

#include "test_json.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace {

using lazily_test::Json;

const Json &required(const Json &object, const std::string &key) {
  const auto *value = object.find(key);
  if (value == nullptr)
    throw std::runtime_error("missing " + key);
  return *value;
}

std::string quote(const std::string &value) {
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

std::string stamp_json(const lazily::WireStamp &stamp) {
  return "{\"wall_time\":" + std::to_string(stamp.wall_time) +
         ",\"logical\":" + std::to_string(stamp.logical) +
         ",\"peer\":" + std::to_string(stamp.peer) + "}";
}

std::string state_json(const lazily::IpcValue &state) {
  if (!std::holds_alternative<lazily::IpcValueInline>(state)) {
    throw std::runtime_error(
        "SharedBlob is outside this peer's semantic-suite profile");
  }
  std::ostringstream out;
  out << "{\"Inline\":[";
  const auto &bytes = std::get<lazily::IpcValueInline>(state).bytes;
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index != 0)
      out << ',';
    out << static_cast<unsigned>(bytes[index]);
  }
  out << "]}";
  return out.str();
}

std::string op_json(const lazily::CrdtOp &op) {
  return "{\"node\":" + std::to_string(op.node) +
         ",\"key\":" + (op.key ? quote(op.key->to_wire()) : "null") +
         ",\"stamp\":" + stamp_json(op.stamp) +
         ",\"state\":" + state_json(op.state) + "}";
}

std::string sync_json(const lazily::CrdtSync &sync) {
  std::ostringstream out;
  out << "{\"CrdtSync\":{\"frontier\":[";
  for (std::size_t index = 0; index < sync.frontier.size(); ++index) {
    if (index != 0)
      out << ',';
    const auto &entry = sync.frontier[index];
    out << '[' << entry.peer << ',' << stamp_json(entry.stamp) << ']';
  }
  out << "],\"ops\":[";
  for (std::size_t index = 0; index < sync.ops.size(); ++index) {
    if (index != 0)
      out << ',';
    out << op_json(sync.ops[index]);
  }
  out << "]}}";
  return out.str();
}

lazily::WireStamp parse_stamp(const Json &value) {
  return {
      required(value, "wall_time").as_int(),
      required(value, "logical").as_int(),
      required(value, "peer").as_int(),
  };
}

lazily::IpcValue parse_state(const Json &value) {
  const auto &bytes = required(value, "Inline");
  if (!bytes.is_array())
    throw std::runtime_error("state.Inline must be an array");
  std::vector<std::uint8_t> result;
  result.reserve(bytes.array.size());
  for (const auto &byte : bytes.array) {
    const auto number = byte->as_int();
    if (number < 0 || number > 255)
      throw std::runtime_error("Inline byte out of range");
    result.push_back(static_cast<std::uint8_t>(number));
  }
  return lazily::IpcValueInline{std::move(result)};
}

lazily::CrdtOp parse_op(const Json &value) {
  std::optional<lazily::NodeKey> key;
  const auto &key_json = required(value, "key");
  if (!key_json.is_null()) {
    key = lazily::NodeKey::create(key_json.as_str());
    if (!key)
      throw std::runtime_error("invalid NodeKey");
  }
  return {
      required(value, "node").as_int(),
      std::move(key),
      parse_stamp(required(value, "stamp")),
      parse_state(required(value, "state")),
  };
}

lazily::CrdtSync parse_sync(const Json &frame) {
  const auto &value = required(frame, "CrdtSync");
  const auto &frontier_json = required(value, "frontier");
  const auto &ops_json = required(value, "ops");
  if (!frontier_json.is_array() || !ops_json.is_array()) {
    throw std::runtime_error("CrdtSync arrays are malformed");
  }

  lazily::CrdtSync sync;
  for (const auto &entry : frontier_json.array) {
    if (!entry->is_array() || entry->array.size() != 2) {
      throw std::runtime_error("frontier entry must be [peer, stamp]");
    }
    sync.frontier.push_back(
        {entry->array[0]->as_int(), parse_stamp(*entry->array[1])});
  }
  for (const auto &op : ops_json.array)
    sync.ops.push_back(parse_op(*op));
  return sync;
}

lazily::CrdtSync normalize_msgpack(const lazily::CrdtSync &sync,
                                   bool positional = false) {
  const lazily::IpcMessage message = lazily::IpcMessageCrdtSync{sync};
  const auto bytes =
      positional ? lazily::encode_positional(message) : lazily::encode(message);
  auto decoded = lazily::decode(bytes);
  if (!std::holds_alternative<lazily::IpcMessageCrdtSync>(decoded)) {
    throw std::runtime_error("production codec changed CrdtSync variant");
  }
  return std::get<lazily::IpcMessageCrdtSync>(std::move(decoded)).value;
}

class Peer {
public:
  std::string handle(const Json &request) {
    const auto command = required(request, "cmd").as_str();
    if (command == "hello")
      return hello(request);
    if (command == "local_set")
      return local_set(request);
    if (command == "deliver")
      return deliver(request);
    if (command == "snapshot")
      return snapshot();
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
  std::string hello(const Json &request) {
    if (required(request, "protocol_version").as_int() != 1) {
      throw std::runtime_error("unsupported protocol_version");
    }
    peer_id_ = required(request, "peer").as_int();
    runtime_ = std::make_unique<lazily::CrdtPlaneRuntime>(*peer_id_);
    logical_ = 0;
    return "{\"ok\":true,\"binding\":\"lazily-cpp\","
           "\"version\":\"0.25.1\",\"protocol_version\":1,"
           "\"features\":[\"distributed_crdt\"],"
           "\"codecs\":[\"msgpack\"],\"channels\":[],"
           "\"channel_variants\":{},\"platform_profile\":\"portable\","
           "\"carve_outs\":[\"json\",\"shared_blob\",\"transport_links\"]}";
  }

  std::string local_set(const Json &request) {
    ensure_started();
    std::optional<lazily::NodeKey> key;
    const auto &key_json = required(request, "key");
    if (!key_json.is_null()) {
      key = lazily::NodeKey::create(key_json.as_str());
      if (!key)
        throw std::runtime_error("invalid NodeKey");
    }
    const lazily::WireStamp stamp{required(request, "at").as_int(), ++logical_,
                                  *peer_id_};
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
    return "{\"ok\":true,\"frame\":" + sync_json(normalize_msgpack(local)) +
           "}";
  }

  std::string deliver(const Json &request) {
    ensure_started();
    const auto sync = normalize_msgpack(parse_sync(required(request, "frame")));
    return "{\"ok\":true,\"applied\":" +
           std::to_string(runtime_->ingest(sync)) + "}";
  }

  std::string snapshot() const {
    ensure_started();
    std::ostringstream out;
    out << "{\"ok\":true,\"cells\":[";
    const auto cells = runtime_->converged();
    for (std::size_t index = 0; index < cells.size(); ++index) {
      if (index != 0)
        out << ',';
      const auto &cell = cells[index];
      out << "{\"node\":" << cell.node
          << ",\"key\":" << (cell.key ? quote(*cell.key) : "null")
          << ",\"state\":" << state_json(cell.state) << '}';
    }
    out << "]}";
    return out.str();
  }

  void ensure_started() const {
    if (!runtime_ || !peer_id_)
      throw std::runtime_error("hello must run first");
  }

  std::optional<lazily::PeerId> peer_id_;
  std::unique_ptr<lazily::CrdtPlaneRuntime> runtime_;
  std::int64_t logical_ = 0;
  bool stopping_ = false;
};

void self_check() {
  const lazily::WireStamp stamp{10, 1, 1};
  const lazily::CrdtSync original{
      {{1, stamp}},
      {{7, std::nullopt, stamp, lazily::IpcValueInline{{65}}}},
  };
  const auto keyed = normalize_msgpack(original);
  const auto positional = normalize_msgpack(original, true);
  if (keyed.ops.size() != 1 || positional.ops.size() != 1 || keyed.ops[0].key ||
      positional.ops[0].key) {
    throw std::runtime_error(
        "MessagePack variants lost the canonical null key");
  }

  lazily::CrdtPlaneRuntime runtime(1);
  if (runtime.ingest(keyed) != 1 || runtime.ingest(positional) != 0) {
    throw std::runtime_error("CRDT production runtime is not idempotent");
  }
  const auto cells = runtime.converged();
  if (cells.size() != 1 ||
      std::get<lazily::IpcValueInline>(cells[0].state).bytes !=
          std::vector<std::uint8_t>{65}) {
    throw std::runtime_error("CRDT snapshot did not converge");
  }
}

} // namespace

int main(int argc, char **argv) {
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
      } catch (const std::exception &error) {
        std::cout << "{\"ok\":false,\"error\":" << quote(error.what()) << "}\n"
                  << std::flush;
      }
      if (peer.stopping())
        break;
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "lazily-cpp interop peer: " << error.what() << '\n';
    return 1;
  }
}
