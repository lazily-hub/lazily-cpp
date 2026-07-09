// Zero-copy transport tests — mirror lazily-formal ZeroCopyTransport.lean:
// resolve_write identity, backend isolation, ABA generation safety, checksum
// integrity, plus an end-to-end spill→encode→decode→resolve round-trip and a
// Linux fork() cross-process smoke for ShmBackend.
#include <lazily/lazily.hpp>

#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#ifdef __linux__
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace lazily;

static int test_count = 0;
static int test_passed = 0;
#define TEST(name)                                        \
  static void name();                                     \
  struct name##_runner {                                  \
    name##_runner() { ++test_count; name(); ++test_passed; } \
  } name##_instance;                                      \
  static void name()

static bool bytes_eq(BlobView v, const std::vector<uint8_t>& expected) {
  return v && v.size == expected.size() &&
         std::memcmp(v.data, expected.data(), expected.size()) == 0;
}

// resolve_write identity: bytes spilled to the backend resolve zero-copy.
TEST(test_in_process_resolve_write) {
  InProcessBackend backend;
  std::vector<uint8_t> payload{1, 2, 3, 4, 5, 6, 7, 8};
  ShmBlobRef ref = backend.write(payload);
  assert(ref.backend == BlobBackendKind::InProcess);
  assert(bytes_eq(backend.read_view(ref), payload));
}

// Backend isolation (resolve_wrong_backend): an in_process descriptor does not
// resolve in an empty router; a shm descriptor does not resolve in an
// in_process-only router.
TEST(test_backend_isolation) {
  InProcessBackend inproc;
  ShmBlobRef ref = inproc.write({9, 9, 9});
  BlobRouter router;  // no backends registered
  assert(!router.read_view(ref));
  router.register_backend(inproc);
  assert(router.read_view(ref));  // now resolves
  ShmBlobRef shm_ref = ref;
  shm_ref.backend = BlobBackendKind::Shm;
  assert(!router.read_view(shm_ref));  // shm kind → no shm backend registered
}

// ABA generation safety (resolve_stale_generation): a stale generation rejects.
TEST(test_stale_generation_rejects) {
  InProcessBackend backend;
  ShmBlobRef ref = backend.write({1, 2, 3});
  ShmBlobRef stale = ref;
  stale.generation += 1;
  assert(!backend.read_view(stale));
}

// Checksum integrity (resolve_corrupt_checksum): a corrupted checksum rejects.
TEST(test_corrupt_checksum_rejects) {
  InProcessBackend backend;
  ShmBlobRef ref = backend.write({4, 5, 6});
  ShmBlobRef corrupt = ref;
  corrupt.checksum += 1;
  assert(!backend.read_view(corrupt));
}

// epoch advance invalidates prior descriptors.
TEST(test_epoch_advance_invalidates) {
  InProcessBackend backend;
  ShmBlobRef ref = backend.write({7, 8});
  assert(backend.read_view(ref));
  backend.advance_epoch();
  assert(!backend.read_view(ref));
}

// End-to-end transport round-trip: spill a large Inline payload → encode →
// decode → resolve via a BlobRouter yields the original bytes (transport_roundtrip).
TEST(test_spill_encode_decode_resolve) {
  InProcessBackend backend;
  BlobRouter router;
  router.register_backend(backend);

  std::vector<uint8_t> big(500, 0x5A);
  Delta delta;
  delta.base_epoch = 1;
  delta.epoch = 2;
  delta.ops.push_back(DeltaOpSlotValue{7, IpcValueInline{big}});
  IpcMessage msg = IpcMessageDelta{std::move(delta)};

  size_t spilled = spill(msg, backend, /*threshold=*/64);
  assert(spilled == big.size());
  // payload is now a SharedBlob descriptor, not inline bytes → small wire.
  auto bytes = encode(msg);
  IpcMessage msg2 = decode(bytes);
  auto& d2 = std::get<IpcMessageDelta>(msg2).value;
  auto& op2 = std::get<DeltaOpSlotValue>(d2.ops[0]);
  assert(std::holds_alternative<IpcValueSharedBlob>(op2.payload));
  assert(bytes_eq(router.resolve(op2.payload), big));
}

#ifdef __linux__
// ShmBackend cross-process smoke: parent writes to a POSIX shm region, child
// opens it by name (separate address space) and resolves the descriptor.
TEST(test_shm_backend_cross_process) {
  std::string name = "/lazily_shm_test_" + std::to_string(::getpid());
  ShmBackend::unlink(name);
  std::vector<uint8_t> payload(1000);
  for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(i * 7 + 1);

  ShmBackend parent(name, 1 << 20, true);
  ShmBlobRef ref = parent.write(payload);
  assert(ref.backend == BlobBackendKind::Shm);
  assert(bytes_eq(parent.read_view(ref), payload));  // same-process resolve works

  pid_t pid = ::fork();
  if (pid == 0) {
    // child: distinct address space; opens the region by name (no create).
    ShmBackend child(name, 1 << 20, false);
    BlobView view = child.read_view(ref);
    bool ok = view && view.size == payload.size() &&
              std::memcmp(view.data, payload.data(), payload.size()) == 0;
    std::quick_exit(ok ? 0 : 1);  // quick_exit avoids running the test runners' dtors
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  ShmBackend::unlink(name);
}
#endif

int main() {
  std::cout << "lazily-cpp transport tests: " << test_passed << "/" << test_count
            << " passed" << std::endl;
  return test_passed == test_count ? 0 : 1;
}
