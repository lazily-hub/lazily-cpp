#ifndef LAZILY_FFI_H
#define LAZILY_FFI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lazily_ffi_bytes_t {
  uint8_t *ptr;
  size_t len;
} lazily_ffi_bytes_t;

uintptr_t lazily_ffi_channel_new(void);
void lazily_ffi_channel_free(uintptr_t handle);
int lazily_ffi_channel_send_json(uintptr_t handle, const uint8_t *ptr, size_t len);
int lazily_ffi_channel_recv_json(uintptr_t handle, lazily_ffi_bytes_t *out);
int lazily_ffi_channel_len(uintptr_t handle, size_t *out_len);
int lazily_ffi_ipc_message_validate_json(const uint8_t *ptr, size_t len);
int lazily_ffi_ipc_message_kind_json(const uint8_t *ptr, size_t len, int *out_kind);
int lazily_ffi_ipc_message_clone_json(const uint8_t *ptr, size_t len, lazily_ffi_bytes_t *out);
void lazily_ffi_bytes_free(lazily_ffi_bytes_t bytes);

#ifdef __cplusplus
}
#endif

#endif
