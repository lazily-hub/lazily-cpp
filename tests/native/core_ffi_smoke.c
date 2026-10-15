#include "lazily_ffi.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  static const char frame[] = "{\"Snapshot\":{}}";
  uintptr_t channel = lazily_ffi_channel_new();
  if (channel == 0) return EXIT_FAILURE;
  if (lazily_ffi_ipc_message_validate_json((const uint8_t *)frame, strlen(frame)) != 0)
    return EXIT_FAILURE;
  int kind = 0;
  if (lazily_ffi_ipc_message_kind_json((const uint8_t *)frame, strlen(frame), &kind) != 0 || kind != 1)
    return EXIT_FAILURE;
  if (lazily_ffi_channel_send_json(channel, (const uint8_t *)frame, strlen(frame)) != 0)
    return EXIT_FAILURE;
  lazily_ffi_bytes_t received = {0};
  if (lazily_ffi_channel_recv_json(channel, &received) != 0 ||
      received.len != strlen(frame) || memcmp(received.ptr, frame, received.len) != 0)
    return EXIT_FAILURE;
  lazily_ffi_bytes_free(received);
  lazily_ffi_channel_free(channel);
  return EXIT_SUCCESS;
}
