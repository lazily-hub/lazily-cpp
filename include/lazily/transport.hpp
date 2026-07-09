#ifndef LAZILY_TRANSPORT_HPP
#define LAZILY_TRANSPORT_HPP

#include <lazily/ipc.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef __linux__
// POSIX shm backend (Linux). Included at GLOBAL scope (not inside namespace
// lazily) so glibc declares shm_open/mmap/... into the global namespace, where
// `::shm_open` etc. find them. shm_open/shm_unlink require _XOPEN_SOURCE (or
// _GNU_SOURCE); the lazily INTERFACE target propagates _GNU_SOURCE on Linux.
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

// Cross-process zero-copy transport — pluggable blob backends.
//
// Spec: lazily-spec/docs/zero-copy-transport.md
// Formal: lazily-formal/LazilyFormal/ZeroCopyTransport.lean
//
// A large payload is spilled to a blob backend (the producer mints a descriptor);
// only the descriptor crosses the wire; the receiver resolves it zero-copy. The
// `BlobBackend` interface is the adapter seam — `InProcessBackend` (wraps
// `ShmBlobArena`) ships as the default; `ShmBackend` (POSIX shm, Linux) is the
// cross-process backend; an Apache Arrow adapter plugs in by implementing the
// interface over Arrow buffers (the descriptor bytes are an Arrow IPC stream the
// receiver imports zero-copy — bring your own Arrow via this seam).

namespace lazily {

// A zero-copy view into a backend's bytes (into a vector OR raw mmap'd memory).
// `data == nullptr` means the descriptor did not resolve (unknown / stale /
// corrupt / wrong backend).
struct BlobView {
  const uint8_t* data = nullptr;
  size_t size = 0;
  explicit operator bool() const { return data != nullptr; }
};

// The adapter seam. A backend mints descriptors via `write` and resolves them
// zero-copy via `read_view`. Entries are immutable + stable-addressed for any
// descriptor's lifetime. The formal laws (resolve_write identity, wrong-backend
// isolation, stale-generation ABA safety, corrupt-checksum rejection) hold for
// every backend by construction.
class BlobBackend {
 public:
  virtual ~BlobBackend() = default;
  virtual BlobBackendKind kind() const = 0;
  virtual ShmBlobRef write(const std::vector<uint8_t>& bytes) = 0;
  virtual BlobView read_view(const ShmBlobRef& ref) const = 0;
  virtual void advance_epoch() = 0;
};

// InProcessBackend: wraps the existing ShmBlobArena (single address space — the
// FFI host / editor-plugin case). Descriptors carry backend=in_process.
class InProcessBackend : public BlobBackend {
 public:
  explicit InProcessBackend(Epoch epoch = 0) : arena_(epoch) {}
  BlobBackendKind kind() const override { return BlobBackendKind::InProcess; }
  ShmBlobRef write(const std::vector<uint8_t>& bytes) override {
    ShmBlobRef ref = arena_.write(bytes);
    ref.backend = BlobBackendKind::InProcess;
    return ref;
  }
  BlobView read_view(const ShmBlobRef& ref) const override {
    const std::vector<uint8_t>* v = arena_.read_view(ref);
    if (!v) return {};
    return {v->data(), v->size()};
  }
  void advance_epoch() override { arena_.advance_epoch(); }

 private:
  ShmBlobArena arena_;
};

// ─────────────────────────────────────────────────────────────────────────────
// POSIX shared-memory backend (Linux). A fixed-capacity shm_open + mmap region
// with a header (capacity, epoch, atomic bump offset) and per-slot
// generation/len/checksum metadata inline. `write` is a lock-free atomic
// bump-allocate. Validated by a fork() cross-process smoke test.
//
// Limitations (documented): no GC/reclamation (bumps until capacity, then
// throws); Linux-only; cross-process multi-writer relies on lock-free atomics
// being address-free (holds on Linux/x86-64). A managed region with reclamation
// plugs in behind the same BlobBackend interface.
// ─────────────────────────────────────────────────────────────────────────────
#ifdef __linux__
class ShmBackend : public BlobBackend {
  static constexpr uint64_t kMagic = 0x4c5a5348424c4f42ULL;  // "LZSHBLOB"
  struct Header {
    uint64_t magic;
    uint64_t capacity;
    std::atomic<uint64_t> bump;  // next write offset (bytes after the header)
    std::atomic<uint64_t> generation;
    std::atomic<uint64_t> epoch;
  };
  struct SlotHeader {
    uint64_t generation;
    uint64_t len;
    uint64_t checksum;
  };

 public:
  // Opens (or creates) a named POSIX shared-memory region. With create=true the
  // region is ftruncated to capacity and initialized. The caller owns unlink
  // timing (shm_unlink when no further readers/writers remain).
  ShmBackend(const std::string& name, uint64_t capacity, bool create) : name_(name) {
    int flags = O_RDWR;
    if (create) flags |= O_CREAT;
    fd_ = ::shm_open(name.c_str(), flags, 0600);
    if (fd_ < 0) throw std::runtime_error("ShmBackend: shm_open failed: " + name);
    if (create) {
      if (::ftruncate(fd_, static_cast<off_t>(capacity)) != 0) {
        ::close(fd_);
        throw std::runtime_error("ShmBackend: ftruncate failed");
      }
    }
    void* p = ::mmap(nullptr, capacity, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (p == MAP_FAILED) {
      ::close(fd_);
      throw std::runtime_error("ShmBackend: mmap failed");
    }
    base_ = static_cast<char*>(p);
    capacity_ = capacity;
    header_ = reinterpret_cast<Header*>(base_);
    if (create) {
      header_->magic = kMagic;
      header_->capacity = capacity;
      header_->bump.store(sizeof(Header), std::memory_order_relaxed);
      header_->generation.store(0, std::memory_order_relaxed);
      header_->epoch.store(0, std::memory_order_relaxed);
    }
  }
  ~ShmBackend() override { close(); }
  ShmBackend(const ShmBackend&) = delete;
  ShmBackend& operator=(const ShmBackend&) = delete;

  BlobBackendKind kind() const override { return BlobBackendKind::Shm; }

  ShmBlobRef write(const std::vector<uint8_t>& bytes) override {
    uint64_t need = sizeof(SlotHeader) + bytes.size();
    uint64_t off = header_->bump.fetch_add(need, std::memory_order_acq_rel);
    if (off + need > header_->capacity)
      throw std::runtime_error("ShmBackend: region full (bump allocator exhausted)");
    uint64_t gen = header_->generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    uint64_t ep = header_->epoch.load(std::memory_order_acquire);
    uint64_t csum = fnv1a(bytes.data(), bytes.size());
    SlotHeader* sh = new (base_ + off) SlotHeader{gen, bytes.size(), csum};
    (void)sh;
    std::memcpy(base_ + off + sizeof(SlotHeader), bytes.data(), bytes.size());
    return {static_cast<int64_t>(off), static_cast<int64_t>(bytes.size()),
            static_cast<int64_t>(gen), static_cast<int64_t>(ep),
            static_cast<int64_t>(csum), BlobBackendKind::Shm};
  }

  BlobView read_view(const ShmBlobRef& ref) const override {
    if (ref.offset < 0 || static_cast<uint64_t>(ref.offset) + sizeof(SlotHeader) >
                              header_->capacity)
      return {};
    uint64_t off = static_cast<uint64_t>(ref.offset);
    const SlotHeader* sh = reinterpret_cast<const SlotHeader*>(base_ + off);
    if (sh->generation != static_cast<uint64_t>(ref.generation)) return {};
    if (sh->len != static_cast<uint64_t>(ref.len)) return {};
    if (sh->checksum != static_cast<uint64_t>(ref.checksum)) return {};
    if (sh->generation != static_cast<uint64_t>(ref.generation)) return {};
    if (header_->epoch.load(std::memory_order_acquire) != static_cast<uint64_t>(ref.epoch))
      return {};
    if (off + sizeof(SlotHeader) + sh->len > header_->capacity) return {};
    return {reinterpret_cast<const uint8_t*>(base_ + off + sizeof(SlotHeader)),
            static_cast<size_t>(sh->len)};
  }

  void advance_epoch() override {
    header_->epoch.fetch_add(1, std::memory_order_acq_rel);
  }

  // Unlink the named region so it is reclaimed when all users unmap. Call once
  // the region is known to have no further users.
  static void unlink(const std::string& name) { ::shm_unlink(name.c_str()); }
  void close() {
    if (base_) {
      ::munmap(base_, capacity_);
      base_ = nullptr;
    }
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

 private:
  static uint64_t fnv1a(const uint8_t* data, size_t n) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < n; ++i) {
      h ^= data[i];
      h *= 0x100000001b3ULL;
    }
    return h;
  }
  std::string name_;
  int fd_ = -1;
  char* base_ = nullptr;
  uint64_t capacity_ = 0;
  Header* header_ = nullptr;
};
#endif  // __linux__

// ── Spill / resolve policy ───────────────────────────────────────────────────

// If an IpcValue is Inline and >= threshold bytes, write it to the backend and
// replace with a SharedBlob descriptor. Returns bytes spilled (0 if not spilled).
inline size_t spill(IpcValue& v, BlobBackend& backend, size_t threshold) {
  if (auto* inl = std::get_if<IpcValueInline>(&v)) {
    if (inl->bytes.size() >= threshold) {
      std::vector<uint8_t> b = std::move(inl->bytes);
      size_t n = b.size();
      ShmBlobRef ref = backend.write(b);
      v = IpcValueSharedBlob{ref};
      return n;
    }
  }
  return 0;
}

// Spill large payloads across an IpcMessage's IpcValue/NodeState sites:
// Snapshot node states, Delta CellSet/SlotValue payloads + NodeAdd states,
// CrdtSync op states. Returns total bytes spilled.
inline size_t spill(IpcMessage& m, BlobBackend& backend, size_t threshold) {
  size_t total = 0;
  auto spill_state = [&](NodeState& s) {
    if (auto* p = std::get_if<NodeStatePayload>(&s)) {
      if (p->bytes.size() >= threshold) {
        std::vector<uint8_t> b = std::move(p->bytes);
        total += b.size();
        s = NodeStateSharedBlob{backend.write(b)};
      }
    }
  };
  if (auto* snap = std::get_if<IpcMessageSnapshot>(&m)) {
    for (auto& nd : snap->value.nodes) spill_state(nd.state);
  } else if (auto* d = std::get_if<IpcMessageDelta>(&m)) {
    for (auto& op : d->value.ops) {
      if (auto* cs = std::get_if<DeltaOpCellSet>(&op))
        total += spill(cs->payload, backend, threshold);
      else if (auto* sv = std::get_if<DeltaOpSlotValue>(&op))
        total += spill(sv->payload, backend, threshold);
      else if (auto* na = std::get_if<DeltaOpNodeAdd>(&op))
        spill_state(na->state);
    }
  } else if (auto* c = std::get_if<IpcMessageCrdtSync>(&m)) {
    for (auto& op : c->value.ops) total += spill(op.state, backend, threshold);
  }
  return total;
}

// Resolve an IpcValue against a single backend: inline bytes returned directly,
// SharedBlob resolved zero-copy. Empty BlobView if a SharedBlob fails to resolve.
inline BlobView resolve(const IpcValue& v, const BlobBackend& backend) {
  if (auto* inl = std::get_if<IpcValueInline>(&v))
    return {inl->bytes.data(), inl->bytes.size()};
  if (auto* sb = std::get_if<IpcValueSharedBlob>(&v)) return backend.read_view(sb->blob);
  return {};
}

// BlobRouter: receiver-side multi-backend resolver. Holds backends by kind and
// resolves any descriptor by its `backend` discriminator (a shm descriptor
// routes to the shm backend, an arrow descriptor to the arrow backend, …).
class BlobRouter {
 public:
  BlobRouter& register_backend(BlobBackend& backend) {
    backends_[backend.kind()] = &backend;
    return *this;
  }
  BlobView read_view(const ShmBlobRef& ref) const {
    auto it = backends_.find(ref.backend);
    if (it == backends_.end()) return {};  // no backend registered for this kind
    return it->second->read_view(ref);
  }
  BlobView resolve(const IpcValue& v) const {
    if (auto* inl = std::get_if<IpcValueInline>(&v))
      return {inl->bytes.data(), inl->bytes.size()};
    if (auto* sb = std::get_if<IpcValueSharedBlob>(&v)) return read_view(sb->blob);
    return {};
  }

 private:
  std::unordered_map<BlobBackendKind, BlobBackend*> backends_;
};

}  // namespace lazily

#endif  // LAZILY_TRANSPORT_HPP
