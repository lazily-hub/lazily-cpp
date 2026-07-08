// lazily_ffi — C-ABI boundary implementation.
#include <lazily/ffi.hpp>

#include <cstdlib>
#include <cstring>

extern "C" {

uintptr_t lazily_ffi_channel_new(void) {
  return reinterpret_cast<uintptr_t>(new lazily::LazilyFfiChannel());
}

void lazily_ffi_channel_free(uintptr_t handle) {
  delete reinterpret_cast<lazily::LazilyFfiChannel*>(handle);
}

int lazily_ffi_channel_send_json(uintptr_t handle, const uint8_t* ptr, size_t len) {
  auto* channel = reinterpret_cast<lazily::LazilyFfiChannel*>(handle);
  if (!channel) return static_cast<int>(lazily::LazilyFfiStatus::NullPointer);
  lazily::LazilyFfiBytes frame;
  frame.bytes.assign(ptr, ptr + len);
  return static_cast<int>(channel->send_json_frame(frame));
}

int lazily_ffi_channel_recv_json(uintptr_t handle, lazily_ffi_bytes_t* out) {
  auto* channel = reinterpret_cast<lazily::LazilyFfiChannel*>(handle);
  if (!channel) return static_cast<int>(lazily::LazilyFfiStatus::NullPointer);
  auto [frame, status] = channel->recv_json_frame();
  if (lazily::is_ok(status)) {
    out->ptr = static_cast<uint8_t*>(std::malloc(frame.bytes.size()));
    std::memcpy(out->ptr, frame.bytes.data(), frame.bytes.size());
    out->len = frame.bytes.size();
  }
  return static_cast<int>(status);
}

int lazily_ffi_channel_len(uintptr_t handle, size_t* out_len) {
  auto* channel = reinterpret_cast<lazily::LazilyFfiChannel*>(handle);
  if (!channel) return static_cast<int>(lazily::LazilyFfiStatus::NullPointer);
  *out_len = channel->len();
  return static_cast<int>(lazily::LazilyFfiStatus::Ok);
}

int lazily_ffi_ipc_message_validate_json(const uint8_t* ptr, size_t len) {
  lazily::LazilyFfiBytes frame;
  frame.bytes.assign(ptr, ptr + len);
  return static_cast<int>(lazily::validate_json(frame));
}

int lazily_ffi_ipc_message_kind_json(const uint8_t* ptr, size_t len, int* out_kind) {
  lazily::LazilyFfiBytes frame;
  frame.bytes.assign(ptr, ptr + len);
  auto [kind, status] = lazily::kind_json(frame);
  *out_kind = static_cast<int>(kind);
  return static_cast<int>(status);
}

int lazily_ffi_ipc_message_clone_json(const uint8_t* ptr, size_t len, lazily_ffi_bytes_t* out) {
  lazily::LazilyFfiBytes frame;
  frame.bytes.assign(ptr, ptr + len);
  auto [cloned, status] = lazily::clone_json(frame);
  if (lazily::is_ok(status)) {
    out->ptr = static_cast<uint8_t*>(std::malloc(cloned.bytes.size()));
    std::memcpy(out->ptr, cloned.bytes.data(), cloned.bytes.size());
    out->len = cloned.bytes.size();
  }
  return static_cast<int>(status);
}

void lazily_ffi_bytes_free(lazily_ffi_bytes_t bytes) {
  std::free(bytes.ptr);
}

int lazily_protocol_version(void) { return lazily::kProtocolVersion; }

}  // extern "C"
