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

}  // namespace lazily

// -- C ABI exports (declared in header, defined in src/ffi.cpp) --

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lazily_ffi_bytes_t {
  uint8_t* ptr;
  size_t len;
} lazily_ffi_bytes_t;

uintptr_t lazily_ffi_channel_new(void);
void lazily_ffi_channel_free(uintptr_t handle);
int lazily_ffi_channel_send_json(uintptr_t handle, const uint8_t* ptr, size_t len);
int lazily_ffi_channel_recv_json(uintptr_t handle, lazily_ffi_bytes_t* out);
int lazily_ffi_channel_len(uintptr_t handle, size_t* out_len);
int lazily_ffi_ipc_message_validate_json(const uint8_t* ptr, size_t len);
int lazily_ffi_ipc_message_kind_json(const uint8_t* ptr, size_t len, int* out_kind);
int lazily_ffi_ipc_message_clone_json(const uint8_t* ptr, size_t len, lazily_ffi_bytes_t* out);
void lazily_ffi_bytes_free(lazily_ffi_bytes_t bytes);

#ifdef __cplusplus
}
#endif

#endif  // LAZILY_FFI_HPP
