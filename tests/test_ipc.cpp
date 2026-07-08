#include <lazily/lazily.hpp>

#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

using namespace lazily;

static int test_count = 0;
static int test_passed = 0;

#define TEST(name)                                        \
  static void name();                                     \
  struct name##_runner {                                  \
    name##_runner() {                                     \
      ++test_count;                                       \
      name();                                             \
      ++test_passed;                                      \
    }                                                     \
  } name##_instance;                                      \
  static void name()

// -- NodeKey --

TEST(test_node_key_valid) {
  auto k = NodeKey::create("foo/bar/baz");
  assert(k);
  auto segs = k->segments();
  assert(segs.size() == 3 && segs[0] == "foo" && segs[1] == "bar" && segs[2] == "baz");
}

TEST(test_node_key_invalid) {
  assert(!NodeKey::create(""));
  assert(!NodeKey::create("/leading"));
  assert(!NodeKey::create("trailing/"));
  assert(!NodeKey::create("double//slash"));
}

// -- ShmBlobArena --

TEST(test_shm_blob_arena) {
  ShmBlobArena arena(0);
  auto ref = arena.write({0x48, 0x65, 0x6c, 0x6c, 0x6f});
  auto data = arena.read(ref);
  assert(data.size() == 5 && data[0] == 0x48);

  arena.advance_epoch();
  auto stale = arena.read(ref);
  assert(stale.empty() && "stale ref after epoch advance");
}

// -- PeerPermissions --

TEST(test_permissions_default_deny) {
  PeerPermissions perms;
  assert(!perms.can_read(1, 100));
  perms.allow(1, read_op(100));
  assert(perms.can_read(1, 100));
  assert(!perms.can_read(1, 101));
  assert(!perms.can_read(2, 100));
}

TEST(test_permissions_revoke) {
  PeerPermissions perms;
  perms.allow(1, write_op(50));
  assert(perms.is_allowed(1, write_op(50)));
  perms.revoke(1, write_op(50));
  assert(!perms.is_allowed(1, write_op(50)));
}

// -- Capability negotiation --

TEST(test_capability_handshake_compatible) {
  auto a = new_capability_handshake(1, "session1");
  auto b = new_capability_handshake(2, "session1");
  assert(a.is_compatible_with(b));
}

TEST(test_capability_handshake_incompatible) {
  auto a = new_capability_handshake(1, "s");
  auto b = new_capability_handshake(2, "s");
  b.protocol_major_version = 2;
  assert(!a.is_compatible_with(b));
}

TEST(test_capability_required_feature) {
  auto a = new_capability_handshake(1, "s");
  a.features.push_back("command-plane-v1");
  auto b = new_capability_handshake(2, "s");
  auto check = a.check_compatible(b, {"command-plane-v1"});
  assert(!check.ok);
  b.features.push_back("command-plane-v1");
  check = a.check_compatible(b, {"command-plane-v1"});
  assert(check.ok);
}

// -- Delta sequencing --

TEST(test_delta_sequencing) {
  std::vector<DeltaOp> ops = {DeltaOpCellSet{1, IpcValueInline{{0x42}}}};
  auto delta = delta_next(5, ops);
  assert(delta.is_next_after(5));
  assert(!delta.is_next_after(4));

  auto status = delta.apply_status(5);
  assert(std::holds_alternative<DeltaApplyStatusApply>(status));

  auto gap = delta.apply_status(3);
  assert(std::holds_alternative<DeltaApplyStatusResync>(gap));
}

// -- Causal receipts --

TEST(test_receipt_observed_nonterminal) {
  ReceiptProjection proj;
  auto r = observed_receipt("r1", "c1", "p1", 1);
  auto status = proj.observe(1, r);
  assert(std::holds_alternative<ReceiptRecorded>(status));
  assert(!is_terminal(proj.latest_for("c1")->outcome));
}

TEST(test_receipt_terminal) {
  ReceiptProjection proj;
  auto r = applied_receipt("r1", "c1", "p1", 1);
  proj.observe(1, r);
  assert(is_terminal(proj.terminal_for("c1")->outcome));
  assert(proj.terminal_for("c1")->outcome == ReceiptOutcome::Applied);
}

TEST(test_receipt_duplicate) {
  ReceiptProjection proj;
  auto r = observed_receipt("r1", "c1", "p1", 1);
  proj.observe(1, r);
  auto status = proj.observe(1, r);
  assert(std::holds_alternative<ReceiptDuplicate>(status));
}

TEST(test_receipt_stale_generation) {
  ReceiptProjection proj;
  auto r = observed_receipt("r1", "c1", "p1", 5);
  auto status = proj.observe(1, r);
  assert(std::holds_alternative<ReceiptStaleGeneration>(status));
}

TEST(test_receipt_terminal_conflict) {
  ReceiptProjection proj;
  proj.observe(1, applied_receipt("r1", "c1", "p1", 1));
  auto status = proj.observe(1, rejected_receipt("r2", "c1", "p2", 1));
  assert(std::holds_alternative<ReceiptTerminalConflict>(status));
}

// -- State projection mirror --

TEST(test_state_projection_flush) {
  StateProjectionMirror mirror;
  mirror.mark_dirty(1);
  mirror.mark_dirty(2);
  mirror.resolve(1, IpcValueInline{{0x41}});

  auto delta = mirror.flush();
  assert(delta.ops.size() == 2);  // 1 Invalidate + 1 SlotValue
  assert(std::holds_alternative<DeltaOpInvalidate>(delta.ops[0]));
  assert(std::holds_alternative<DeltaOpSlotValue>(delta.ops[1]));
}

// -- FFI channel (C++) --

TEST(test_ffi_channel_cpp) {
  LazilyFfiChannel channel;
  assert(channel.is_empty());

  std::string msg = R"({"Snapshot":{"epoch":0,"nodes":[],"edges":[],"roots":[]}})";
  auto status = channel.send_json_frame({std::vector<uint8_t>(msg.begin(), msg.end())});
  assert(is_ok(status));
  assert(!channel.is_empty());

  auto [frame, recv_status] = channel.recv_json_frame();
  assert(is_ok(recv_status));
  assert(frame.as_json() == msg);
}

TEST(test_ffi_validate_json) {
  std::string valid = R"({"Snapshot":{"epoch":0}})";
  std::string invalid = "not json";
  assert(is_ok(validate_json({std::vector<uint8_t>(valid.begin(), valid.end())})));
  assert(!is_ok(validate_json({std::vector<uint8_t>(invalid.begin(), invalid.end())})));
}

TEST(test_ffi_kind_json) {
  std::string snap = R"({"Snapshot":{}})";
  std::string delta = R"({"Delta":{}})";
  std::string crdt = R"({"CrdtSync":{}})";

  auto [k1, s1] = kind_json({std::vector<uint8_t>(snap.begin(), snap.end())});
  assert(is_ok(s1) && k1 == LazilyFfiMessageKind::Snapshot);

  auto [k2, s2] = kind_json({std::vector<uint8_t>(delta.begin(), delta.end())});
  assert(is_ok(s2) && k2 == LazilyFfiMessageKind::Delta);

  auto [k3, s3] = kind_json({std::vector<uint8_t>(crdt.begin(), crdt.end())});
  assert(is_ok(s3) && k3 == LazilyFfiMessageKind::CrdtSync);
}

// -- FFI C ABI --

TEST(test_ffi_cabi_channel) {
  uintptr_t handle = lazily_ffi_channel_new();
  assert(handle != 0);

  std::string msg = R"({"Delta":{"base_epoch":0,"epoch":1,"ops":[]}})";
  int status = lazily_ffi_channel_send_json(handle,
    reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
  assert(status == 0);

  size_t len = 0;
  lazily_ffi_channel_len(handle, &len);
  assert(len == 1);

  lazily_ffi_bytes_t out{};
  status = lazily_ffi_channel_recv_json(handle, &out);
  assert(status == 0);
  assert(out.len == msg.size());
  assert(std::memcmp(out.ptr, msg.data(), msg.size()) == 0);
  lazily_ffi_bytes_free(out);

  lazily_ffi_channel_free(handle);
}

TEST(test_ffi_cabi_validate) {
  std::string valid = R"({"Snapshot":{}})";
  int status = lazily_ffi_ipc_message_validate_json(
    reinterpret_cast<const uint8_t*>(valid.data()), valid.size());
  assert(status == 0);
}

TEST(test_ffi_cabi_kind) {
  std::string msg = R"({"CrdtSync":{}})";
  int kind = 0;
  int status = lazily_ffi_ipc_message_kind_json(
    reinterpret_cast<const uint8_t*>(msg.data()), msg.size(), &kind);
  assert(status == 0);
  assert(kind == static_cast<int>(LazilyFfiMessageKind::CrdtSync));
}

int main() {
  std::cout << "lazily-cpp IPC+FFI tests: " << test_passed << "/" << test_count
            << " passed" << std::endl;
  return test_passed == test_count ? 0 : 1;
}
