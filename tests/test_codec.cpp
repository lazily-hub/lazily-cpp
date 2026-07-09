// Round-trip tests for the lazily msgpack codec — covers every variant branch
// of the IpcMessage tree (Snapshot / Delta / CrdtSync) and the optional-key
// present/absent cases. Asserts both decoded field values and canonical
// re-encode equality: encode(decode(encode(x))) == encode(x).
#include <lazily/lazily.hpp>

#include <cassert>
#include <iostream>

using namespace lazily;

static int test_count = 0;
static int test_passed = 0;
#define TEST(name)                                        \
  static void name();                                     \
  struct name##_runner {                                  \
    name##_runner() { ++test_count; name(); ++test_passed; } \
  } name##_instance;                                      \
  static void name()

static bool reencode_equal(const IpcMessage& m) {
  auto b1 = encode(m);
  IpcMessage m2 = decode(b1);
  auto b2 = encode(m2);
  return b1 == b2;
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
  assert(!d.nodes[1].key.has_value());  // optional absent → nil → nullopt
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
  assert((ipc_value_equal(std::get<DeltaOpCellSet>(d.ops[0]).payload,
                          IpcValueInline{{0xAA, 0xBB}})));
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

int main() {
  std::cout << "lazily-cpp codec tests: " << test_passed << "/" << test_count
            << " passed" << std::endl;
  return test_passed == test_count ? 0 : 1;
}
