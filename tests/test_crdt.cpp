#include <lazily/lazily.hpp>

#include <cassert>
#include <iostream>
#include <string>
#include <thread>

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

// -- Thread-safe context --

TEST(test_thread_safe_basic) {
  ThreadSafeContext ctx;
  auto c = ctx.cell(42);
  assert(ctx.get_cell(c) == 42);
  ctx.set_cell(c, 100);
  assert(ctx.get_cell(c) == 100);
}

TEST(test_thread_safe_concurrent) {
  ThreadSafeContext ctx;
  auto a = ctx.cell(0);
  ctx.batch([&](Context& c) {
    c.set_cell(a, 1);
  });
  assert(ctx.get_cell(a) == 1);
}

TEST(test_thread_safe_multi_thread) {
  ThreadSafeContext ctx;
  auto counter = ctx.cell(0);

  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&]() {
      for (int i = 0; i < 100; ++i) {
        ctx.batch([&](Context& c) {
          int v = c.get_cell(counter);
          c.set_cell(counter, v + 1);
        });
      }
    });
  }
  for (auto& t : threads) t.join();
  assert(ctx.get_cell(counter) == 400);
}

// -- HLC --

TEST(test_hlc_tick_monotonic) {
  Hlc hlc(1);
  auto s1 = hlc.tick(100);
  auto s2 = hlc.tick(100);
  assert(s2 > s1);
  auto s3 = hlc.tick(200);
  assert(s3 > s2);
}

TEST(test_hlc_observe) {
  Hlc hlc_a(1);
  Hlc hlc_b(2);
  auto a1 = hlc_a.tick(100);
  auto b1 = hlc_b.tick(100);
  auto observed = hlc_a.observe(b1, 100);
  assert(observed > b1);
}

TEST(test_hlc_total_order) {
  HlcStamp a{100, 0, 1};
  HlcStamp b{100, 0, 2};
  HlcStamp c{100, 1, 1};
  HlcStamp d{101, 0, 1};
  assert(a < b);
  assert(b < c);
  assert(c < d);
}

// -- TextCrdt --

TEST(test_textcrdt_insert_delete) {
  TextCrdt crdt(1);
  crdt.insert_str(0, "hello");
  assert(crdt.text() == "hello");
  crdt.del(0);
  assert(crdt.text() == "ello");
  crdt.insert_str(4, " world");
  assert(crdt.text() == "ello world");
}

TEST(test_textcrdt_merge_converges) {
  TextCrdt a(1);
  TextCrdt b(2);
  a.insert_str(0, "hello");
  b.merge(a);
  assert(b.text() == "hello");

  // Concurrent inserts at same position
  a.insert(0, "A");
  b.insert(0, "B");
  a.merge(b);
  b.merge(a);
  assert(a.text() == b.text());
}

TEST(test_textcrdt_delta_sync) {
  TextCrdt a(1);
  TextCrdt b(2);
  a.insert_str(0, "hello world");
  auto vv_a = a.version_vector();
  auto delta = a.delta_since({});
  b.apply_delta(delta);
  assert(b.text() == "hello world");

  a.insert_str(5, " beautiful");
  auto vv_b = b.version_vector();
  auto delta2 = a.delta_since(vv_b);
  b.apply_delta(delta2);
  assert(b.text() == "hello beautiful world");
}

// -- SeqCrdt --

TEST(test_seqcrdt_basic) {
  SeqCrdt<std::string, std::string> crdt(1);
  crdt.insert_back("a", "alpha", 100);
  crdt.insert_back("b", "bravo", 101);
  crdt.insert_back("c", "charlie", 102);
  auto order = crdt.order();
  assert(order.size() == 3);
  assert(order[0] == "a" && order[1] == "b" && order[2] == "c");
  assert(crdt.get("a").value() == "alpha");
}

TEST(test_seqcrdt_move) {
  SeqCrdt<std::string, std::string> crdt(1);
  crdt.insert_back("a", "1", 100);
  crdt.insert_back("b", "2", 101);
  crdt.insert_back("c", "3", 102);
  std::string anchor_a = "a";
  crdt.move_between("c", nullptr, &anchor_a, 103);
  auto order = crdt.order();
  assert(order[0] == "c");
  assert(order[1] == "a");
  assert(order[2] == "b");
}

TEST(test_seqcrdt_remove) {
  SeqCrdt<int, int> crdt(1);
  crdt.insert_back(1, 10, 100);
  crdt.insert_back(2, 20, 101);
  crdt.remove(1, 102);
  auto order = crdt.order();
  assert(order.size() == 1 && order[0] == 2);
  assert(crdt.tombstone_count() == 1);
}

TEST(test_seqcrdt_merge_converges) {
  SeqCrdt<int, int> a(1);
  SeqCrdt<int, int> b(2);
  a.insert_back(1, 10, 100);
  a.insert_back(2, 20, 101);
  b.merge(a, 200);
  auto a_order = a.order();
  auto b_order = b.order();
  assert(a_order == b_order);
}

// -- Registers --

TEST(test_lww_register) {
  HlcStamp s1{100, 0, 1};
  HlcStamp s2{200, 0, 1};
  LwwRegister<int> reg(0, s1);
  assert(!reg.set(1, s1));
  assert(reg.set(2, s2));
  assert(reg.value() == 2);
}

TEST(test_pn_counter) {
  PnCounter a(1);
  PnCounter b(2);
  a.increment();
  a.increment();
  b.increment();
  b.decrement();
  a.merge(b);
  b.merge(a);
  assert(a.value() == 2);
  assert(b.value() == 2);
}

// -- Stable-id alignment --

TEST(test_stable_id_anchored) {
  auto b1 = new_anchored_block("heading", "Hello World");
  auto b2 = new_anchored_block("heading", "Goodbye World");
  auto key1 = block_key_of(b1);
  auto key2 = block_key_of(b2);
  assert(key1.equals(key2));
  assert(key1.is_anchored());
}

TEST(test_stable_id_content_hash) {
  auto b1 = new_block("Hello World");
  auto b2 = new_block("Hello  World");
  assert(block_key_of(b1).equals(block_key_of(b2)));
  auto b3 = new_block("Goodbye");
  assert(!block_key_of(b1).equals(block_key_of(b3)));
}

TEST(test_stable_id_similarity) {
  assert(similarity("hello world foo", "hello world bar") > 0.5);
  assert(similarity("hello", "goodbye") < 0.5);
  assert(similarity("", "") == 1.0);
}

TEST(test_stable_id_align) {
  std::vector<Block> old_blocks = {
    new_anchored_block("a", "alpha"),
    new_block("bravo text"),
    new_anchored_block("c", "charlie")
  };
  std::vector<Block> new_blocks = {
    new_anchored_block("a", "alpha"),
    new_block("bravo edited text"),
    new_anchored_block("c", "charlie")
  };
  auto alignment = align(old_blocks, new_blocks);
  assert(alignment.new_matches[0].kind == Match::Kind::Same);
  assert(alignment.new_matches[1].kind == Match::Kind::Edited);
  assert(alignment.new_matches[2].kind == Match::Kind::Same);
  assert(alignment.removed.empty());
}

int main() {
  std::cout << "lazily-cpp CRDT+TS tests: " << test_passed << "/" << test_count
            << " passed" << std::endl;
  return test_passed == test_count ? 0 : 1;
}
