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
#define TEST(name)                                                                                 \
  static void name();                                                                              \
  struct name##_runner {                                                                           \
    name##_runner() {                                                                              \
      ++test_count;                                                                                \
      name();                                                                                      \
      ++test_passed;                                                                               \
    }                                                                                              \
  } name##_instance;                                                                               \
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
  BlobRouter router; // no backends registered
  assert(!router.read_view(ref));
  router.register_backend(inproc);
  assert(router.read_view(ref)); // now resolves
  ShmBlobRef shm_ref = ref;
  shm_ref.backend = BlobBackendKind::Shm;
  assert(!router.read_view(shm_ref)); // shm kind → no shm backend registered
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
  for (size_t i = 0; i < payload.size(); ++i)
    payload[i] = static_cast<uint8_t>(i * 7 + 1);

  ShmBackend parent(name, 1 << 20, true);
  ShmBlobRef ref = parent.write(payload);
  assert(ref.backend == BlobBackendKind::Shm);
  assert(bytes_eq(parent.read_view(ref), payload)); // same-process resolve works

  pid_t pid = ::fork();
  if (pid == 0) {
    // child: distinct address space; opens the region by name (no create).
    ShmBackend child(name, 1 << 20, false);
    BlobView view = child.read_view(ref);
    bool ok = view && view.size == payload.size() &&
              std::memcmp(view.data, payload.data(), payload.size()) == 0;
    std::quick_exit(ok ? 0 : 1); // quick_exit avoids running the test runners' dtors
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  ShmBackend::unlink(name);
}
#endif

// INTENTIONAL leniency, pinned: an unrecognised `backend` token on a ShmBlobRef
// decodes as `Shm`, and the CONSEQUENCE of that default is an empty view, never
// wrong bytes. See the wire reason on `blob_backend_kind_from_str` in ipc.hpp.
TEST(blob_backend_leniency_is_pinned) {
  // The tokens this binding speaks resolve to their own variants.
  assert(blob_backend_kind_from_str("shm") == BlobBackendKind::Shm);
  assert(blob_backend_kind_from_str("arrow") == BlobBackendKind::Arrow);
  assert(blob_backend_kind_from_str("in_process") == BlobBackendKind::InProcess);

  // A backend a future peer adds, and an absent field, both resolve to `Shm`.
  assert(blob_backend_kind_from_str("iouring") == BlobBackendKind::Shm);
  assert(blob_backend_kind_from_str("") == BlobBackendKind::Shm);
  assert(blob_backend_kind_from_str("SHM") == BlobBackendKind::Shm); // case-sensitive

  // The encoder half stays total: every variant has a non-empty token, and no
  // two variants share one.
  assert(std::string(blob_backend_kind_str(BlobBackendKind::Shm)) == "shm");
  assert(std::string(blob_backend_kind_str(BlobBackendKind::Arrow)) == "arrow");
  assert(std::string(blob_backend_kind_str(BlobBackendKind::InProcess)) == "in_process");

  // Through a real frame: a descriptor naming an unknown backend decodes — the
  // frame is NOT refused — and arrives as `Shm`.
  {
    MsgPacker p;
    p.map_header(6);
    p.str("offset");
    p.i64(0);
    p.str("len");
    p.i64(3);
    p.str("generation");
    p.i64(1);
    p.str("epoch");
    p.i64(0);
    p.str("checksum");
    p.i64(0);
    p.str("backend");
    p.str("iouring");
    const auto frame = std::move(p).take();
    MsgUnpacker u(frame);
    const ShmBlobRef ref = unpack_shm_blob_ref(u);
    assert(ref.backend == BlobBackendKind::Shm);
    assert(ref.len == 3);

    // The consequence. Resolving that foreign descriptor against a live backend
    // yields an EMPTY view: generation/len/checksum are verified before any
    // payload is handed back, so an unknown backend degrades to "blob
    // unavailable" rather than to another blob's bytes.
    InProcessBackend backend;
    const std::vector<uint8_t> real{1, 2, 3};
    ShmBlobRef good = backend.write(real);
    assert(bytes_eq(backend.read_view(good), real));
    assert(!backend.read_view(ref));

    // And a router with no backend registered for a kind resolves to empty too.
    BlobRouter router;
    assert(!router.read_view(good));
  }
}

int main() {
  std::cout << "lazily-cpp transport tests: " << test_passed << "/" << test_count << " passed"
            << std::endl;
  return test_passed == test_count ? 0 : 1;
}
