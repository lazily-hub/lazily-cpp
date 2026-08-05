#include <lazily/lazily.hpp>

#include "test_spec_fixture.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
#include <set>
#include <string>

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

// Zero-copy read_view (optimization): returns the cached payload pointer
// without copying; nullptr on invalid/stale descriptors.
TEST(test_shm_blob_read_view) {
  ShmBlobArena arena(0);
  auto ref = arena.write({1, 2, 3, 4});
  const std::vector<uint8_t>* v = arena.read_view(ref);
  assert(v && v->size() == 4 && (*v)[2] == 3);

  ShmBlobRef bad = ref;
  bad.checksum = -1; // corrupt checksum
  assert(arena.read_view(bad) == nullptr);

  arena.advance_epoch();
  assert(arena.read_view(ref) == nullptr && "stale ref returns nullptr");

  // read (copy) still works and is consistent with read_view when valid.
  ShmBlobArena a2(0);
  auto ref2 = a2.write({9, 9});
  const std::vector<uint8_t>* vv = a2.read_view(ref2);
  auto copy = a2.read(ref2);
  assert(vv && copy == *vv);
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

// -- Negotiated-codec dispatch (#lzcppcodecdispatch) --

// The token spelling is the wire contract, and an unknown token has to fail
// closed. `postcard` is the interesting case: protocol.md defines it, so a peer
// may legitimately offer it, and this binding does not implement it — resolving
// it to anything at all would be the same class of lie as advertising `msgpack`
// over a private framing.
TEST(test_codec_tokens_resolve_both_ways) {
  assert(std::strcmp(codec_token(Codec::Json), "json") == 0);
  assert(std::strcmp(codec_token(Codec::MsgPack), "msgpack") == 0);
  assert(codec_from_token("json") == Codec::Json);
  assert(codec_from_token("msgpack") == Codec::MsgPack);
  assert(!codec_from_token("postcard").has_value());
  assert(!codec_from_token("").has_value());
  assert(!codec_from_token("MsgPack").has_value());
  // Round trip every value the enum can hold, so adding a codec without a token
  // is a test failure rather than a silent "msgpack".
  for (const Codec codec : {Codec::Json, Codec::MsgPack})
    assert(codec_from_token(codec_token(codec)) == codec);
}

static IpcMessage codec_dispatch_sample() {
  CrdtSync sync;
  sync.frontier.push_back({1, WireStamp{5, 0, 1}});
  sync.ops.push_back({7, std::nullopt, WireStamp{5, 0, 1}, IpcValueInline{{1, 2, 3}}});
  return IpcMessageCrdtSync{sync};
}

static NegotiatedCapabilities negotiate_codec_session(CapabilityHandshake local) {
  auto remote = new_capability_handshake(local.peer_id + 1, local.session_id);
  remote.codec = local.codec;
  remote.max_frame_size = local.max_frame_size;
  remote.fragmentation_supported = local.fragmentation_supported;
  const auto negotiation = local.negotiate(remote);
  assert(negotiation.ok());
  return *negotiation.capabilities;
}

// A handshake defaults to `msgpack`, and what it encodes must be the SPEC
// msgpack wire: an externally tagged frame keyed by the variant name. Checking
// only that the bytes round-trip would pass against codec.hpp's private
// framing, which is precisely the frame this test exists to exclude.
TEST(test_negotiated_msgpack_uses_the_spec_wire) {
  const auto local = new_capability_handshake(1, "s");
  assert(local.codec == Codec::MsgPack);
  const auto session = negotiate_codec_session(local);

  const IpcMessage message = codec_dispatch_sample();
  const std::vector<uint8_t> frame = negotiated_encode(session, message);
  assert(negotiated_decode(session, frame) == message);

  const JsonValue view = msgpack_to_json(frame);
  assert(view.is_object());
  assert(view.object.size() == 1);
  assert(view.object.front().first == "CrdtSync");
}

TEST(test_negotiated_json_uses_the_reference_codec) {
  auto local = new_capability_handshake(1, "s");
  local.codec = Codec::Json;
  const auto session = negotiate_codec_session(local);

  const IpcMessage message = codec_dispatch_sample();
  const std::vector<uint8_t> frame = negotiated_encode(session, message);
  assert(negotiated_decode(session, frame) == message);

  const std::string text(frame.begin(), frame.end());
  assert(text == encode_json(message));
  assert(text.rfind("{\"CrdtSync\":", 0) == 0);
}

// The point of the whole seam: no negotiated token reaches codec.hpp's private
// framing. Both directions are checked, because "the encoder is right" and "the
// decoder refuses a foreign frame" are separate failures — a dispatch that
// encoded correctly but decoded the private form would still let a peer push
// bytes nothing agreed on.
TEST(test_private_framing_is_unreachable_from_a_negotiated_token) {
  const IpcMessage message = codec_dispatch_sample();
  const std::vector<uint8_t> private_frame = encode(message);
  assert(decode(private_frame) == message); // the private codec still works

  for (const Codec codec : {Codec::Json, Codec::MsgPack}) {
    assert(codec_encode(codec, message) != private_frame);
    bool threw = false;
    try {
      (void)codec_decode(codec, private_frame);
    } catch (const std::runtime_error&) {
      threw = true;
    }
    assert(threw);
  }
}

// Two peers that negotiated the same codec exchange frames through it; a
// mismatch is caught at the handshake and names both sides.
TEST(test_negotiated_codec_mismatch_fails_the_handshake) {
  auto a = new_capability_handshake(1, "s");
  auto b = new_capability_handshake(2, "s");
  assert(a.is_compatible_with(b));

  const IpcMessage message = codec_dispatch_sample();
  const auto negotiation = a.negotiate(b);
  assert(negotiation.ok());
  assert(negotiated_decode(*negotiation.capabilities,
                           negotiated_encode(*negotiation.capabilities, message)) == message);

  b.codec = Codec::Json;
  const auto check = a.check_compatible(b, {});
  assert(!check.ok);
  assert(check.field == "codec");
  assert(check.reason.find("msgpack") != std::string::npos);
  assert(check.reason.find("json") != std::string::npos);
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

// -- Canonical root-level IPC fixtures (#rootlevelconformance) --

static const ShmBlobRef* node_shared_blob(const NodeState& state) {
  const auto* shared = std::get_if<NodeStateSharedBlob>(&state);
  return shared == nullptr ? nullptr : &shared->blob;
}

static const ShmBlobRef* value_shared_blob(const IpcValue& value) {
  const auto* shared = std::get_if<IpcValueSharedBlob>(&value);
  return shared == nullptr ? nullptr : &shared->blob;
}

static void assert_snapshot_fixture(lazily_test::AssertionKeys& expected,
                                    const Snapshot& snapshot) {
  expected.assert_key_if_present("epoch", snapshot.epoch);
  expected.assert_key_if_present("node_count", static_cast<int64_t>(snapshot.nodes.size()));
  expected.assert_key_if_present("edge_count", static_cast<int64_t>(snapshot.edges.size()));
  expected.assert_key_if_present("root_count", static_cast<int64_t>(snapshot.roots.size()));

  assert(!snapshot.nodes.empty());
  const NodeSnapshot& first = snapshot.nodes.front();
  expected.assert_key_if_present("first_node_type_tag", first.type_tag);

  const auto opaque =
      std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(), [](const NodeSnapshot& node) {
        return std::holds_alternative<NodeStateOpaque>(node.state);
      });
  expected.assert_key_if_present("has_opaque_node", opaque != snapshot.nodes.end());
  if (expected.has("opaque_node_id")) {
    assert(opaque != snapshot.nodes.end());
    expected.assert_key("opaque_node_id", opaque->node);
  }

  if (expected.has("first_node_state_kind")) {
    const bool is_shared = std::holds_alternative<NodeStateSharedBlob>(first.state);
    expected.assert_key("first_node_state_kind", std::string(is_shared ? "SharedBlob" : "other"));
  }
  if (const ShmBlobRef* blob = node_shared_blob(first.state)) {
    expected.assert_key_if_present("blob_offset", blob->offset);
    expected.assert_key_if_present("blob_len", blob->len);
    expected.assert_key_if_present("blob_epoch", blob->epoch);
  }
}

static void assert_delta_fixture(lazily_test::AssertionKeys& expected, const Delta& delta) {
  expected.assert_key_if_present("base_epoch", delta.base_epoch);
  expected.assert_key_if_present("epoch", delta.epoch);
  expected.assert_key_if_present("op_count", static_cast<int64_t>(delta.ops.size()));
  expected.assert_key_if_present("is_sequential", delta.is_next_after(delta.base_epoch));

  if (expected.has("resync_after_epoch_10")) {
    expected.assert_key("resync_after_epoch_10",
                        std::holds_alternative<DeltaApplyStatusResync>(delta.apply_status(10)));
  }

  std::set<std::string> variants;
  for (const DeltaOp& op : delta.ops)
    variants.insert(delta_op_variant_name(op));
  expected.assert_key_if_present("has_all_op_variants", variants.size() == 7);

  const bool needs_first_kind = expected.has("first_op_kind");
  const bool needs_payload_kind = expected.has("first_op_payload_kind");
  const bool needs_payload_backend = expected.has("first_op_payload_backend");
  if (!(needs_first_kind || needs_payload_kind || needs_payload_backend)) return;
  assert(!delta.ops.empty());
  const DeltaOp& first = delta.ops.front();
  if (needs_first_kind)
    expected.assert_key("first_op_kind", std::string(delta_op_variant_name(first)));
  const IpcValue* payload = nullptr;
  if (const auto* slot = std::get_if<DeltaOpSlotValue>(&first))
    payload = &slot->payload;
  else if (const auto* cell = std::get_if<DeltaOpCellSet>(&first))
    payload = &cell->payload;

  if (needs_payload_kind) {
    assert(payload != nullptr);
    expected.assert_key("first_op_payload_kind",
                        std::string(std::holds_alternative<IpcValueSharedBlob>(*payload)
                                        ? "SharedBlob"
                                        : "Inline"));
  }
  if (needs_payload_backend) {
    assert(payload != nullptr);
    const ShmBlobRef* blob = value_shared_blob(*payload);
    assert(blob != nullptr);
    expected.assert_key("first_op_payload_backend",
                        std::string(blob_backend_kind_str(blob->backend)));
  }
}

TEST(test_root_ipc_conformance_fixtures) {
  const std::vector<std::string> fixtures = {
      "snapshot_minimal.json",      "snapshot_multi_node.json", "snapshot_shared_blob.json",
      "delta_non_sequential.json",  "delta_sequential.json",    "delta_shared_blob.json",
      "delta_zero_copy_arrow.json",
  };

  for (const std::string& name : fixtures) {
    const std::string text = lazily_test::spec_fixture_text("", name);

    const auto fixture = json_parse(text);
    const IpcMessage message = json_to_ipc_message(json_required(fixture, "wire"));
    assert(decode_json(encode_json(message)) == message);

    const auto test_fixture = lazily_test::parse_json(text);
    lazily_test::AssertionKeys expected(name + " assertions",
                                        lazily_test::json_member(*test_fixture, "assertions"));
    if (const auto* snapshot = std::get_if<IpcMessageSnapshot>(&message))
      assert_snapshot_fixture(expected, snapshot->value);
    else if (const auto* delta = std::get_if<IpcMessageDelta>(&message))
      assert_delta_fixture(expected, delta->value);
    else
      assert(false && "root IPC fixture must decode as Snapshot or Delta");
  }
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
  assert(delta.ops.size() == 2); // 1 Invalidate + 1 SlotValue
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
  int status = lazily_ffi_channel_send_json(handle, reinterpret_cast<const uint8_t*>(msg.data()),
                                            msg.size());
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
  int status = lazily_ffi_ipc_message_validate_json(reinterpret_cast<const uint8_t*>(valid.data()),
                                                    valid.size());
  assert(status == 0);
}

TEST(test_ffi_cabi_kind) {
  std::string msg = R"({"CrdtSync":{}})";
  int kind = 0;
  int status = lazily_ffi_ipc_message_kind_json(reinterpret_cast<const uint8_t*>(msg.data()),
                                                msg.size(), &kind);
  assert(status == 0);
  assert(kind == static_cast<int>(LazilyFfiMessageKind::CrdtSync));
}

int main() {
  REQUIRE_FIXTURES_LOADED(7);
  std::cout << "lazily-cpp IPC+FFI tests: " << test_passed << "/" << test_count << " passed"
            << std::endl;
  return test_passed == test_count ? 0 : 1;
}
