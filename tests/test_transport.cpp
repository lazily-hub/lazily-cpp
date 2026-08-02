// Zero-copy transport tests — mirror lazily-formal ZeroCopyTransport.lean:
// resolve_write identity, backend isolation, ABA generation safety, checksum
// integrity, plus an end-to-end spill→encode→decode→resolve round-trip and a
// Linux fork() cross-process smoke for ShmBackend.
#include <lazily/lazily.hpp>

#include <cassert>
#include <cstring>
#include <iostream>
#include <stdexcept>
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

// Backend-discriminator strictness, pinned (`#lzblobbackendstrict`). This test
// asserted the OPPOSITE until the clause landed: an unrecognised `backend` token
// decoded as `Shm`, on the argument that generation/epoch/checksum verification
// would turn a misrouted descriptor into an empty view. That argument inverts
// `resolve_wrong_backend` — see the reason on `blob_backend_kind_from_str` in
// ipc.hpp. ABSENCE stays lenient; a PRESENT unknown token is refused.
TEST(blob_backend_strictness_is_pinned) {
  // The tokens this binding speaks resolve to their own variants.
  assert(blob_backend_kind_from_str("shm") == BlobBackendKind::Shm);
  assert(blob_backend_kind_from_str("arrow") == BlobBackendKind::Arrow);
  assert(blob_backend_kind_from_str("in_process") == BlobBackendKind::InProcess);

  // Absence is the forward-compatibility channel, and it is the ONLY one: a
  // descriptor that carries no `backend` field at all is `Shm`. That is a
  // default-member-initialiser on ShmBlobRef, reached without consulting the
  // token table — which is why leniency here cannot be confused with leniency
  // over an unrecognised token.
  assert(ShmBlobRef{}.backend == BlobBackendKind::Shm);

  // A present token outside the enum is REFUSED, and the error names it. A
  // decoder that rejected for some unrelated reason would not satisfy this.
  const char* const kUnknownTokens[] = {"iouring", "rdma", "", "SHM", "Arrow"};
  for (const char* token : kUnknownTokens) {
    bool threw_runtime_error = false;
    try {
      (void)blob_backend_kind_from_str(token);
    } catch (const std::runtime_error& e) {
      threw_runtime_error = true;
      // Only meaningful for the non-empty tokens, but `find("")` is 0 for all.
      assert(std::string(e.what()).find(token) != std::string::npos);
    }
    assert(threw_runtime_error);
  }
  // `std::invalid_argument` derives from `std::logic_error`, so it would escape
  // the `catch (const std::runtime_error&)` every decode caller uses — the
  // regression `#lzspecdecoderbound` pinned for NodeId. Prove the refusal is
  // NOT of that family.
  {
    bool threw_logic_error = false;
    try {
      (void)blob_backend_kind_from_str("rdma");
    } catch (const std::logic_error&) {
      threw_logic_error = true;
    } catch (const std::runtime_error&) {
    }
    assert(!threw_logic_error);
  }

  // The encoder half stays total: every variant has a non-empty token, and no
  // two variants share one. Totality here is not leniency — the OMISSION rule
  // (`backend` is not written at all when it is `Shm`) lives at the call site.
  assert(std::string(blob_backend_kind_str(BlobBackendKind::Shm)) == "shm");
  assert(std::string(blob_backend_kind_str(BlobBackendKind::Arrow)) == "arrow");
  assert(std::string(blob_backend_kind_str(BlobBackendKind::InProcess)) == "in_process");

  // Through a real frame: a descriptor naming an unknown backend refuses the
  // WHOLE frame rather than routing a non-shm descriptor into the shm table.
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
    bool refused = false;
    try {
      (void)unpack_shm_blob_ref(u);
    } catch (const std::runtime_error& e) {
      refused = true;
      assert(std::string(e.what()).find("iouring") != std::string::npos);
    }
    assert(refused);
  }

  // The same frame WITHOUT the field still decodes, and arrives as `Shm`. This
  // is the anti-vacuity control: a decoder that refused every descriptor would
  // satisfy the refusal above.
  {
    MsgPacker p;
    p.map_header(5);
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
    const auto frame = std::move(p).take();
    MsgUnpacker u(frame);
    const ShmBlobRef ref = unpack_shm_blob_ref(u);
    assert(ref.backend == BlobBackendKind::Shm);
    assert(ref.len == 3);
  }

  // Resolution still routes by kind: an in_process descriptor does not resolve
  // against a router with no backend registered for its kind.
  {
    InProcessBackend backend;
    const std::vector<uint8_t> real{1, 2, 3};
    ShmBlobRef good = backend.write(real);
    assert(bytes_eq(backend.read_view(good), real));
    BlobRouter router;
    assert(!router.read_view(good));
  }
}

int main() {
  std::cout << "lazily-cpp transport tests: " << test_passed << "/" << test_count << " passed"
            << std::endl;
  return test_passed == test_count ? 0 : 1;
}
