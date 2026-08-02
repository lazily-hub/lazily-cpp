// Round-trip tests for the lazily msgpack codec — covers every variant branch
// of the IpcMessage tree (Snapshot / Delta / CrdtSync) and the optional-key
// present/absent cases. Asserts both decoded field values and canonical
// re-encode equality: encode(decode(encode(x))) == encode(x).
#include <lazily/lazily.hpp>

#include <cassert>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

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

static bool reencode_equal(const IpcMessage& m) {
  auto b1 = encode(m);
  IpcMessage m2 = decode(b1);
  auto b2 = encode(m2);
  return b1 == b2;
}

// Positional codec round-trip + cross-format interop (#lzcpppositionalcodec).
// Asserts:
//   - encode_positional → decode yields the same message
//   - encode_positional frames are strictly smaller than string-keyed frames
//   - legacy decode (string-keyed path) is still selectable via top-level
//     MAP kind, and positional via ARRAY kind, automatically.
static bool reencode_positional_equal(const IpcMessage& m) {
  auto b1 = encode_positional(m);
  IpcMessage m2 = decode(b1);
  auto b2 = encode_positional(m2);
  return b1 == b2;
}

static bool positional_shrinks(const IpcMessage& m) {
  return encode_positional(m).size() < encode(m).size();
}

TEST(test_snapshot_roundtrip) {
  Snapshot s;
  s.epoch = 7;
  s.nodes.push_back({10, "cell", NodeStatePayload{{1, 2, 3, 4}}, NodeKey::create("doc/a")});
  s.nodes.push_back({11, "blob", NodeStateSharedBlob{{5, 6, 7, 8, 9}}, std::nullopt});
  s.nodes.push_back({12, "opaque", NodeStateOpaque{}, NodeKey::create("doc/b/c")});
  s.edges.push_back({10, 11});
  s.edges.push_back({11, 12});
  s.roots = {10};

  IpcMessage m = IpcMessageSnapshot{std::move(s)};
  auto bytes = encode(m);
  assert(!bytes.empty());
  Snapshot d = std::get<IpcMessageSnapshot>(decode(bytes)).value;
  assert(d.epoch == 7);
  assert(d.nodes.size() == 3);
  assert(d.nodes[0].node == 10 && d.nodes[0].type_tag == "cell");
  assert(std::holds_alternative<NodeStatePayload>(d.nodes[0].state));
  assert((std::get<NodeStatePayload>(d.nodes[0].state).bytes == std::vector<uint8_t>{1, 2, 3, 4}));
  assert(d.nodes[0].key.has_value() && d.nodes[0].key->path() == "doc/a");
  assert(std::holds_alternative<NodeStateSharedBlob>(d.nodes[1].state));
  assert(!d.nodes[1].key.has_value()); // optional absent → nil → nullopt
  const auto& blob = std::get<NodeStateSharedBlob>(d.nodes[1].state).blob;
  assert(blob.offset == 5 && blob.checksum == 9);
  assert(std::holds_alternative<NodeStateOpaque>(d.nodes[2].state));
  assert(d.nodes[2].key.has_value() && d.nodes[2].key->path() == "doc/b/c");
  assert(d.edges.size() == 2 && d.edges[1].dependency == 12);
  assert(d.roots == std::vector<NodeId>{10});
  assert(reencode_equal(m));
}

TEST(test_delta_all_ops_roundtrip) {
  Delta delta;
  delta.base_epoch = 3;
  delta.epoch = 4;
  delta.ops.push_back(DeltaOpCellSet{20, IpcValueInline{{0xAA, 0xBB}}});
  delta.ops.push_back(DeltaOpSlotValue{21, IpcValueSharedBlob{{1, 2, 3, 4, 5}}});
  delta.ops.push_back(DeltaOpInvalidate{22});
  delta.ops.push_back(DeltaOpNodeAdd{23, "slot", NodeStatePayload{{9}}, NodeKey::create("k/x")});
  delta.ops.push_back(DeltaOpNodeRemove{24});
  delta.ops.push_back(DeltaOpEdgeAdd{25, 26});
  delta.ops.push_back(DeltaOpEdgeRemove{27, 28});

  IpcMessage m = IpcMessageDelta{std::move(delta)};
  auto bytes = encode(m);
  Delta d = std::get<IpcMessageDelta>(decode(bytes)).value;
  assert(d.base_epoch == 3 && d.epoch == 4);
  assert(d.ops.size() == 7);
  assert(std::holds_alternative<DeltaOpCellSet>(d.ops[0]));
  assert(
      (ipc_value_equal(std::get<DeltaOpCellSet>(d.ops[0]).payload, IpcValueInline{{0xAA, 0xBB}})));
  assert(std::holds_alternative<DeltaOpSlotValue>(d.ops[1]));
  assert(std::holds_alternative<DeltaOpInvalidate>(d.ops[2]));
  auto& na = std::get<DeltaOpNodeAdd>(d.ops[3]);
  assert(na.node == 23 && na.type_tag == "slot" && na.key.has_value() && na.key->path() == "k/x");
  assert(std::holds_alternative<DeltaOpNodeRemove>(d.ops[4]));
  assert(std::holds_alternative<DeltaOpEdgeAdd>(d.ops[5]) &&
         std::get<DeltaOpEdgeAdd>(d.ops[5]).dependency == 26);
  assert(std::holds_alternative<DeltaOpEdgeRemove>(d.ops[6]) &&
         std::get<DeltaOpEdgeRemove>(d.ops[6]).dependent == 27);
  assert(reencode_equal(m));
}

TEST(test_crdt_sync_roundtrip) {
  CrdtSync c;
  c.frontier.push_back({1, {100, 2, 1}});
  c.frontier.push_back({2, {200, 0, 2}});
  c.ops.push_back({30, NodeKey::create("crdt/y"), {150, 1, 1}, IpcValueInline{{1, 2}}});
  c.ops.push_back({31, std::nullopt, {160, 0, 1}, IpcValueSharedBlob{{7, 8, 9, 8, 7}}});

  IpcMessage m = IpcMessageCrdtSync{std::move(c)};
  auto bytes = encode(m);
  CrdtSync d = std::get<IpcMessageCrdtSync>(decode(bytes)).value;
  assert(d.frontier.size() == 2);
  assert(d.frontier[0].peer == 1 && d.frontier[0].stamp.wall_time == 100);
  assert(d.frontier[1].stamp.peer == 2);
  assert(d.ops.size() == 2);
  assert(d.ops[0].node == 30 && d.ops[0].key.has_value() && d.ops[0].key->path() == "crdt/y");
  assert(d.ops[0].stamp.logical == 1);
  assert(!d.ops[1].key.has_value());
  assert((ipc_value_equal(d.ops[1].state, IpcValueSharedBlob{{7, 8, 9, 8, 7}})));
  assert(reencode_equal(m));
}

// A round-trip alone cannot distinguish the canonical [peer, stamp] frontier
// tuple from a self-consistent but incompatible {peer, stamp} implementation.
// Inspect the encoder shape independently, then feed the decoder a separately
// constructed canonical frame. The keyless op also pins required `key: nil`.
TEST(test_crdt_sync_canonical_wire_shape) {
  CrdtSync c;
  c.frontier.push_back({7, {100, 2, 7}});
  c.ops.push_back({30, std::nullopt, {150, 1, 7}, IpcValueInline{{1, 2}}});

  MsgPacker encoded;
  pack_crdt_sync(encoded, c);
  MsgUnpacker wire(encoded.bytes());
  assert(wire.read_map_header() == 2);
  assert(wire.read_str() == "frontier");
  assert(wire.read_array_header() == 1);
  assert(wire.peek_kind() == MsgUnpacker::Kind::Array);
  assert(wire.read_array_header() == 2);
  assert(wire.read_i64() == 7);
  wire.skip(); // WireStamp
  assert(wire.read_str() == "ops");
  assert(wire.read_array_header() == 1);
  assert(wire.read_map_header() == 4);
  assert(wire.read_str() == "node");
  assert(wire.read_i64() == 30);
  assert(wire.read_str() == "key");
  assert(wire.peek_kind() == MsgUnpacker::Kind::Nil);
  wire.expect_nil();
  assert(wire.read_str() == "stamp");
  wire.skip();
  assert(wire.read_str() == "state");
  wire.skip();
  assert(wire.eof());

  MsgPacker canonical;
  canonical.map_header(2);
  canonical.str("frontier");
  canonical.array_header(1);
  canonical.array_header(2);
  canonical.i64(9);
  pack_wire_stamp(canonical, {200, 3, 9});
  canonical.str("ops");
  canonical.array_header(1);
  canonical.map_header(4);
  canonical.str("node");
  canonical.i64(31);
  canonical.str("key");
  canonical.nil();
  canonical.str("stamp");
  pack_wire_stamp(canonical, {210, 4, 9});
  canonical.str("state");
  pack_ipc_value(canonical, IpcValueInline{{3, 4}});

  MsgUnpacker input(canonical.bytes());
  CrdtSync decoded = unpack_crdt_sync(input);
  assert(input.eof());
  assert(decoded.frontier.size() == 1);
  assert(decoded.frontier[0].peer == 9);
  assert(decoded.frontier[0].stamp.wall_time == 200);
  assert(decoded.ops.size() == 1);
  assert(decoded.ops[0].node == 31);
  assert(!decoded.ops[0].key.has_value());
  assert((ipc_value_equal(decoded.ops[0].state, IpcValueInline{{3, 4}})));
}

// ── Positional codec (#lzcpppositionalcodec) ──

TEST(test_positional_snapshot_roundtrip) {
  Snapshot s;
  s.epoch = 7;
  s.nodes.push_back({10, "cell", NodeStatePayload{{1, 2, 3, 4}}, NodeKey::create("doc/a")});
  s.nodes.push_back({11, "blob", NodeStateSharedBlob{{5, 6, 7, 8, 9}}, std::nullopt});
  s.nodes.push_back({12, "opaque", NodeStateOpaque{}, NodeKey::create("doc/b/c")});
  s.edges.push_back({10, 11});
  s.edges.push_back({11, 12});
  s.roots = {10};

  IpcMessage m = IpcMessageSnapshot{std::move(s)};
  auto bytes = encode_positional(m);
  assert(!bytes.empty());
  // Top-level byte must be a fixarray (0x90 | 3) — positional envelope.
  assert((bytes[0] & 0xf0) == 0x90 && (bytes[0] & 0x0f) == 3);
  Snapshot d = std::get<IpcMessageSnapshot>(decode(bytes)).value;
  assert(d.epoch == 7);
  assert(d.nodes.size() == 3);
  assert(d.nodes[0].node == 10 && d.nodes[0].type_tag == "cell");
  assert(std::holds_alternative<NodeStatePayload>(d.nodes[0].state));
  assert((std::get<NodeStatePayload>(d.nodes[0].state).bytes == std::vector<uint8_t>{1, 2, 3, 4}));
  assert(d.nodes[0].key.has_value() && d.nodes[0].key->path() == "doc/a");
  assert(std::holds_alternative<NodeStateSharedBlob>(d.nodes[1].state));
  assert(!d.nodes[1].key.has_value());
  const auto& blob = std::get<NodeStateSharedBlob>(d.nodes[1].state).blob;
  assert(blob.offset == 5 && blob.checksum == 9 && blob.backend == BlobBackendKind::Shm);
  assert(std::holds_alternative<NodeStateOpaque>(d.nodes[2].state));
  assert(d.nodes[2].key.has_value() && d.nodes[2].key->path() == "doc/b/c");
  assert(d.edges.size() == 2 && d.edges[1].dependency == 12);
  assert(d.roots == std::vector<NodeId>{10});
  assert(reencode_positional_equal(m));
  assert(positional_shrinks(m));
}

TEST(test_positional_delta_all_ops_roundtrip) {
  Delta delta;
  delta.base_epoch = 3;
  delta.epoch = 4;
  delta.ops.push_back(DeltaOpCellSet{20, IpcValueInline{{0xAA, 0xBB}}});
  delta.ops.push_back(DeltaOpSlotValue{21, IpcValueSharedBlob{{1, 2, 3, 4, 5}}});
  delta.ops.push_back(DeltaOpInvalidate{22});
  delta.ops.push_back(DeltaOpNodeAdd{23, "slot", NodeStatePayload{{9}}, NodeKey::create("k/x")});
  delta.ops.push_back(DeltaOpNodeRemove{24});
  delta.ops.push_back(DeltaOpEdgeAdd{25, 26});
  delta.ops.push_back(DeltaOpEdgeRemove{27, 28});

  IpcMessage m = IpcMessageDelta{std::move(delta)};
  auto bytes = encode_positional(m);
  Delta d = std::get<IpcMessageDelta>(decode(bytes)).value;
  assert(d.base_epoch == 3 && d.epoch == 4);
  assert(d.ops.size() == 7);
  assert(std::holds_alternative<DeltaOpCellSet>(d.ops[0]));
  assert(
      (ipc_value_equal(std::get<DeltaOpCellSet>(d.ops[0]).payload, IpcValueInline{{0xAA, 0xBB}})));
  assert(std::holds_alternative<DeltaOpSlotValue>(d.ops[1]));
  assert(std::holds_alternative<DeltaOpInvalidate>(d.ops[2]));
  auto& na = std::get<DeltaOpNodeAdd>(d.ops[3]);
  assert(na.node == 23 && na.type_tag == "slot" && na.key.has_value() && na.key->path() == "k/x");
  assert(std::holds_alternative<DeltaOpNodeRemove>(d.ops[4]));
  assert(std::holds_alternative<DeltaOpEdgeAdd>(d.ops[5]) &&
         std::get<DeltaOpEdgeAdd>(d.ops[5]).dependency == 26);
  assert(std::holds_alternative<DeltaOpEdgeRemove>(d.ops[6]) &&
         std::get<DeltaOpEdgeRemove>(d.ops[6]).dependent == 27);
  assert(reencode_positional_equal(m));
  assert(positional_shrinks(m));
}

TEST(test_positional_crdt_sync_roundtrip) {
  CrdtSync c;
  c.frontier.push_back({1, {100, 2, 1}});
  c.frontier.push_back({2, {200, 0, 2}});
  c.ops.push_back({30, NodeKey::create("crdt/y"), {150, 1, 1}, IpcValueInline{{1, 2}}});
  c.ops.push_back({31, std::nullopt, {160, 0, 1}, IpcValueSharedBlob{{7, 8, 9, 8, 7}}});

  IpcMessage m = IpcMessageCrdtSync{std::move(c)};
  auto bytes = encode_positional(m);
  CrdtSync d = std::get<IpcMessageCrdtSync>(decode(bytes)).value;
  assert(d.frontier.size() == 2);
  assert(d.frontier[0].peer == 1 && d.frontier[0].stamp.wall_time == 100);
  assert(d.frontier[1].stamp.peer == 2);
  assert(d.ops.size() == 2);
  assert(d.ops[0].node == 30 && d.ops[0].key.has_value() && d.ops[0].key->path() == "crdt/y");
  assert(d.ops[0].stamp.logical == 1);
  assert(!d.ops[1].key.has_value());
  assert((ipc_value_equal(d.ops[1].state, IpcValueSharedBlob{{7, 8, 9, 8, 7}})));
  assert(reencode_positional_equal(m));
  assert(positional_shrinks(m));
}

TEST(test_positional_control_frames_roundtrip) {
  IpcMessage resync = IpcMessageResyncRequest{ResyncRequest{42}};
  IpcMessage ack = IpcMessageOutboxAck{OutboxAck{99}};

  auto rb = encode_positional(resync);
  auto ab = encode_positional(ack);
  assert(std::get<IpcMessageResyncRequest>(decode(rb)).value.from_epoch == 42);
  assert(std::get<IpcMessageOutboxAck>(decode(ab)).value.through_epoch == 99);
  assert(reencode_positional_equal(resync));
  assert(reencode_positional_equal(ack));
}

TEST(test_positional_and_legacy_interoperate) {
  // A legacy-encoded frame must round-trip through decode() and vice versa;
  // decode() auto-detects MAP vs ARRAY at the top level.
  Snapshot s;
  s.epoch = 5;
  s.nodes.push_back({1, "cell", NodeStatePayload{{9, 9}}, NodeKey::create("p/q")});
  s.roots = {1};
  IpcMessage m = IpcMessageSnapshot{std::move(s)};

  auto legacy_bytes = encode(m);
  auto pos_bytes = encode_positional(m);
  // Top-level discriminator differs (map vs fixarray).
  assert((legacy_bytes[0] & 0xf0) == 0x80);
  assert((pos_bytes[0] & 0xf0) == 0x90);

  // Either flavor decodes through the same `decode()` to the same Snapshot.
  Snapshot from_legacy = std::get<IpcMessageSnapshot>(decode(legacy_bytes)).value;
  Snapshot from_pos = std::get<IpcMessageSnapshot>(decode(pos_bytes)).value;
  assert(from_legacy.epoch == 5 && from_pos.epoch == 5);
  assert(from_legacy.nodes[0].node == 1 && from_pos.nodes[0].node == 1);
  assert(from_pos.nodes[0].key->path() == "p/q");
}

// ─────────────────────────────────────────────────────────────────────────────
// Fail-open audit of the codec's dispatch surface.
//
// The library-side counterpart to the runner sweep. Each test below names one
// site and asserts the verdict recorded in the header comment beside it, so a
// deliberate leniency and a forgotten one stop looking alike from the outside.
// ─────────────────────────────────────────────────────────────────────────────

template <typename F> static bool throws_runtime_error(F&& f) {
  try {
    f();
  } catch (const std::runtime_error&) {
    return true;
  } catch (...) {
    return false; // wrong type is a failure, not a pass
  }
  return false;
}

// INTENTIONAL: an unknown FIELD is skipped (forward compatibility).
// FAIL CLOSED: an unknown DISCRIMINATOR is refused.
TEST(codec_forward_compat_and_closed_discriminators) {
  // (a) The leniency. A WireStamp map carrying a field this reader predates
  //     decodes, and the fields it does know survive intact.
  {
    MsgPacker p;
    p.map_header(4);
    p.str("wall_time");
    p.i64(11);
    p.str("logical");
    p.i64(22);
    p.str("peer");
    p.i64(33);
    p.str("a_field_from_a_newer_writer");
    p.str("ignored");
    MsgUnpacker u(p.bytes());
    WireStamp s = unpack_wire_stamp(u);
    assert(s.wall_time == 11 && s.logical == 22 && s.peer == 33);
  }

  // A NodeState map with the given `kind`, or with no `kind` field at all.
  auto node_state_frame = [](std::optional<int64_t> kind) {
    MsgPacker p;
    p.map_header(kind ? 2 : 1);
    if (kind) {
      p.str("kind");
      p.i64(*kind);
    }
    p.str("bytes");
    p.bin(std::vector<uint8_t>{7, 7});
    return std::move(p).take();
  };
  auto ipc_value_frame = [](std::optional<int64_t> kind) {
    MsgPacker p;
    p.map_header(kind ? 2 : 1);
    if (kind) {
      p.str("kind");
      p.i64(*kind);
    }
    p.str("bytes");
    p.bin(std::vector<uint8_t>{8, 8});
    return std::move(p).take();
  };

  // (b) The three NodeState kinds this codec writes still decode.
  {
    auto payload = node_state_frame(0);
    MsgUnpacker u(payload);
    assert(std::holds_alternative<NodeStatePayload>(unpack_node_state(u)));
    auto opaque = node_state_frame(2);
    MsgUnpacker u2(opaque);
    assert(std::holds_alternative<NodeStateOpaque>(unpack_node_state(u2)));
  }
  // (c) An unknown NodeState kind is refused, not folded into `Opaque`.
  {
    auto frame = node_state_frame(7);
    assert(throws_runtime_error([&] {
      MsgUnpacker u(frame);
      (void)unpack_node_state(u);
    }));
  }
  // (d) A NodeState map with NO discriminator is refused. It used to decode as
  //     `Opaque` — a real variant meaning "body deliberately not carried".
  {
    auto frame = node_state_frame(std::nullopt);
    assert(throws_runtime_error([&] {
      MsgUnpacker u(frame);
      (void)unpack_node_state(u);
    }));
  }
  // (e) IpcValue: kind 0 and 1 decode; anything else is refused. The old code
  //     tested only kind 0, so every other value became a `SharedBlob` holding
  //     a zero-initialised descriptor.
  {
    auto inl = ipc_value_frame(0);
    MsgUnpacker u(inl);
    assert(std::holds_alternative<IpcValueInline>(unpack_ipc_value(u)));
  }
  {
    auto frame = ipc_value_frame(9);
    assert(throws_runtime_error([&] {
      MsgUnpacker u(frame);
      (void)unpack_ipc_value(u);
    }));
  }
  {
    auto frame = ipc_value_frame(std::nullopt);
    assert(throws_runtime_error([&] {
      MsgUnpacker u(frame);
      (void)unpack_ipc_value(u);
    }));
  }
}

// INTENTIONAL: `peek_kind` classifies a tag lazily never writes as `Other`
// rather than throwing; every path that consumes the byte still refuses it.
TEST(msgpack_other_kind_is_classified_then_refused) {
  const std::vector<uint8_t> fixext1{0xd4, 0x00, 0x00}; // fixext 1: type 0, one byte
  MsgUnpacker peek(fixext1);
  assert(peek.peek_kind() == MsgUnpacker::Kind::Other);
  assert(throws_runtime_error([&] {
    MsgUnpacker u(fixext1);
    u.skip();
  }));
  assert(throws_runtime_error([&] {
    MsgUnpacker u(fixext1);
    (void)u.read_i64();
  }));
}

// FAIL CLOSED: `nil` means absent, but a PRESENT key that is not a valid
// NodeKey path is refused instead of quietly decoding as absent.
TEST(codec_refuses_an_invalid_nodekey_path) {
  auto key_frame = [](std::string_view path) {
    MsgPacker p;
    p.str(path);
    return std::move(p).take();
  };
  auto nil_frame = [] {
    MsgPacker p;
    p.nil();
    return std::move(p).take();
  };

  // The leniency that must survive (`#lzkeynullstrict`): nil decodes as absent.
  {
    auto frame = nil_frame();
    MsgUnpacker u(frame);
    assert(!unpack_optional_node_key(u).has_value());
    MsgUnpacker u2(frame);
    assert(!unpack_optional_node_key_positional(u2).has_value());
  }
  // A valid path still decodes.
  {
    auto frame = key_frame("doc/a/b");
    MsgUnpacker u(frame);
    assert(unpack_optional_node_key(u)->path() == "doc/a/b");
  }
  // An invalid path is refused by BOTH msgpack forms, matching json_codec.hpp.
  for (const char* bad : {"", "doc//b", "/leading", "trailing/"}) {
    auto frame = key_frame(bad);
    assert(throws_runtime_error([&] {
      MsgUnpacker u(frame);
      (void)unpack_optional_node_key(u);
    }));
    assert(throws_runtime_error([&] {
      MsgUnpacker u(frame);
      (void)unpack_optional_node_key_positional(u);
    }));
  }
  // A whole frame carrying one is refused, not silently stripped of its key.
  {
    MsgPacker p;
    p.map_header(2);
    p.str("type");
    p.i64(0);
    p.str("value");
    p.map_header(4);
    p.str("epoch");
    p.i64(1);
    p.str("nodes");
    p.array_header(1);
    p.map_header(4);
    p.str("node");
    p.i64(1);
    p.str("type_tag");
    p.str("cell");
    p.str("state");
    p.map_header(1);
    p.str("kind");
    p.i64(2);
    p.str("key");
    p.str("doc//b");
    p.str("edges");
    p.array_header(0);
    p.str("roots");
    p.array_header(0);
    const auto frame = std::move(p).take();
    assert(throws_runtime_error([&] { (void)decode(frame); }));
  }
}

// FAIL CLOSED: a token `stoll`/`stod` refuses must surface as the parser's own
// `std::runtime_error`, not as the `std::logic_error` those functions throw.
// A caller guarding decode with `catch (const std::runtime_error&)` — the
// documented error type — used to miss it and terminate.
TEST(json_parse_refuses_a_malformed_number_as_a_decode_error) {
  for (const char* text : {"-", "+", ".", "e", "-."}) {
    assert(throws_runtime_error([&] { (void)json_parse(text); }));
  }
  // Partial parses. `stoll`/`stod` consume the longest valid prefix and report
  // nothing, so these used to decode as 1, 1.2 and 1 respectively.
  //
  // Both branches must be probed. The number scan sets `floating` only on
  // `.`/`e`/`E`, so every token below that contains one exercises `stod` and
  // NONE of them reaches `stoll` — an all-floating list leaves the integer
  // branch's check unfalsifiable. `1-2` and `12+34` are the integer probes:
  // interior signs keep `floating` false, and `stoll` stops at the sign.
  for (const char* floating : {"1e+", "1.2.3", "1..2", "1e", "--1", "1.2e"}) {
    assert(throws_runtime_error([&] { (void)json_parse(floating); }));
  }
  for (const char* integral : {"1-2", "12+34", "1-", "5+"}) {
    assert(throws_runtime_error([&] { (void)json_parse(integral); }));
  }
  // The out-of-range half of the same contract, already covered by
  // `#lzspecdecoderbound`, must keep working.
  assert(throws_runtime_error([&] { (void)json_parse("99999999999999999999999"); }));
  // And well-formed numbers still parse.
  assert(json_parse("-12").integer == -12);
  assert(json_parse("1e2").real == 100.0);
}

// FAIL CLOSED: every `Codec` variant maps to its OWN token. Written as an
// exhaustive switch so a variant added later cannot inherit `msgpack`.
TEST(codec_token_is_distinct_per_variant) {
  assert(std::string(codec_token(Codec::Json)) == "json");
  assert(std::string(codec_token(Codec::MsgPack)) == "msgpack");
  assert(std::string(codec_token(Codec::Json)) != std::string(codec_token(Codec::MsgPack)));
  // Round-trip: the token a peer advertises resolves back to the same variant.
  assert(codec_from_token(codec_token(Codec::Json)) == Codec::Json);
  assert(codec_from_token(codec_token(Codec::MsgPack)) == Codec::MsgPack);
  // And an unspoken token fails the handshake rather than defaulting.
  assert(!codec_from_token("postcard").has_value());
  assert(!codec_from_token("").has_value());
}

int main() {
  std::cout << "lazily-cpp codec tests: " << test_passed << "/" << test_count << " passed"
            << std::endl;
  return test_passed == test_count ? 0 : 1;
}
