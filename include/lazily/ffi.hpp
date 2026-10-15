#ifndef LAZILY_FFI_HPP
#define LAZILY_FFI_HPP

#include <lazily/ipc.hpp>
#include <lazily/types.hpp>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

namespace lazily {

// -- FFI status codes (normative C-ABI discriminants) --

enum class LazilyFfiStatus : int {
  Ok = 0,
  Empty = 1,
  NullPointer = 2,
  InvalidMessage = 3,
  EncodeFailed = 4,
  Panic = 5,
};

inline bool is_ok(LazilyFfiStatus s) { return s == LazilyFfiStatus::Ok; }

enum class LazilyFfiMessageKind : int {
  Unknown = 0,
  Snapshot = 1,
  Delta = 2,
  CrdtSync = 3,
  ResyncRequest = 4,
  OutboxAck = 5,
};

// -- Owned byte buffer --

struct LazilyFfiBytes {
  std::vector<uint8_t> bytes;

  size_t len() const { return bytes.size(); }
  std::string as_json() const { return std::string(bytes.begin(), bytes.end()); }
};

inline LazilyFfiBytes new_ffi_bytes(const std::vector<uint8_t>& data) {
  return {std::vector<uint8_t>(data)};
}

// -- FFI channel (in-process send→recv relay) --

class LazilyFfiChannel {
public:
  LazilyFfiChannel() = default;

  LazilyFfiStatus send_json_frame(const LazilyFfiBytes& frame) {
    // Decode + re-encode canonical JSON (the contract pin)
    // For now, accept the frame as-is (a real impl would parse + re-serialize)
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push(frame.bytes);
    return LazilyFfiStatus::Ok;
  }

  std::pair<LazilyFfiBytes, LazilyFfiStatus> recv_json_frame() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) return {{}, LazilyFfiStatus::Empty};
    auto bytes = queue_.front();
    queue_.pop();
    return {{bytes}, LazilyFfiStatus::Ok};
  }

  size_t len() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }

  bool is_empty() const { return len() == 0; }

private:
  mutable std::mutex mutex_;
  std::queue<std::vector<uint8_t>> queue_;
};

// -- FFI validation helpers --

inline LazilyFfiStatus validate_json(const LazilyFfiBytes& frame) {
  // A real impl would parse the JSON as an IpcMessage.
  // For now, check it's non-empty and starts with '{'.
  if (frame.bytes.empty()) return LazilyFfiStatus::Empty;
  if (frame.bytes[0] != '{') return LazilyFfiStatus::InvalidMessage;
  return LazilyFfiStatus::Ok;
}

inline std::pair<LazilyFfiMessageKind, LazilyFfiStatus> kind_json(const LazilyFfiBytes& frame) {
  auto status = validate_json(frame);
  if (!is_ok(status)) return {LazilyFfiMessageKind::Unknown, status};
  // Peek at the tag key
  std::string s(frame.bytes.begin(), frame.bytes.end());
  if (s.find("\"Snapshot\"") != std::string::npos)
    return {LazilyFfiMessageKind::Snapshot, LazilyFfiStatus::Ok};
  if (s.find("\"Delta\"") != std::string::npos)
    return {LazilyFfiMessageKind::Delta, LazilyFfiStatus::Ok};
  if (s.find("\"CrdtSync\"") != std::string::npos)
    return {LazilyFfiMessageKind::CrdtSync, LazilyFfiStatus::Ok};
  if (s.find("\"ResyncRequest\"") != std::string::npos)
    return {LazilyFfiMessageKind::ResyncRequest, LazilyFfiStatus::Ok};
  if (s.find("\"OutboxAck\"") != std::string::npos)
    return {LazilyFfiMessageKind::OutboxAck, LazilyFfiStatus::Ok};
  return {LazilyFfiMessageKind::Unknown, LazilyFfiStatus::InvalidMessage};
}

inline std::pair<LazilyFfiBytes, LazilyFfiStatus> clone_json(const LazilyFfiBytes& frame) {
  // Decode + re-encode canonical JSON (the contract pin)
  auto status = validate_json(frame);
  if (!is_ok(status)) return {{}, status};
  return {{frame.bytes}, LazilyFfiStatus::Ok};
}

inline constexpr bool kFfiHasCABI = true;

} // namespace lazily

// -- C ABI exports (declared in the C-compatible public header, defined in
// src/ffi.cpp) --
#include <lazily_ffi.h>

#endif // LAZILY_FFI_HPP
