#include <lazily/lazily.hpp>

#include <cassert>
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

// -- CellMap basics --

TEST(test_cellmap_entry_caches) {
  Context ctx;
  CellMap<std::string, int> map(ctx);
  auto a1 = map.entry(ctx, "a", 1);
  auto a2 = map.entry(ctx, "a", 999);
  assert(a1.id == a2.id);
  assert(ctx.get_cell(a1) == 1);
  assert(map.len_untracked() == 1);
}

TEST(test_cellmap_membership_reactive) {
  Context ctx;
  CellMap<std::string, int> map(ctx);
  auto a = map.entry(ctx, "a", 1);
  map.entry(ctx, "b", 2);

  auto count = ctx.computed<int>([&](Context& c) {
    return (int)map.len(c);
  });
  assert(ctx.get(count) == 2);

  ctx.set_cell(a, 100);
  assert(ctx.is_set(count) && "membership reader stayed cached on value change");

  map.entry(ctx, "c", 3);
  assert(ctx.get(count) == 3);

  map.remove(ctx, "b");
  assert(ctx.get(count) == 2);
}

TEST(test_cellmap_per_entry_independent) {
  Context ctx;
  CellMap<std::string, int> map(ctx);
  auto a = map.entry(ctx, "a", 1);
  auto b = map.entry(ctx, "b", 2);

  auto view_a = ctx.computed<int>([&](Context& c) {
    return map.get(c, "a").value_or(0) * 10;
  });
  assert(ctx.get(view_a) == 10);

  ctx.set_cell(b, 222);
  assert(ctx.is_set(view_a) && "sibling change must not invalidate");

  ctx.set_cell(a, 5);
  assert(ctx.get(view_a) == 50);
}

TEST(test_cellmap_move_preserves_identity) {
  Context ctx;
  CellMap<std::string, int> map(ctx);
  auto a = map.entry(ctx, "a", 1);
  map.entry(ctx, "b", 2);
  map.entry(ctx, "c", 3);

  assert(map.move_to(ctx, "c", 0));
  auto keys = map.keys(ctx);
  assert(keys.size() == 3 && keys[0] == "c" && keys[1] == "a" && keys[2] == "b");

  auto a_handle = map.handle("a");
  assert(a_handle && a_handle->id == a.id);
}

TEST(test_cellmap_pure_move_spares_membership) {
  Context ctx;
  CellMap<std::string, int> map(ctx);
  map.entry(ctx, "a", 1);
  map.entry(ctx, "b", 2);
  map.entry(ctx, "c", 3);

  auto count = ctx.computed<int>([&](Context& c) {
    return (int)map.len(c);
  });
  auto has_b = ctx.computed<bool>([&](Context& c) {
    return map.contains_key(c, "b");
  });
  ctx.get(count);
  ctx.get(has_b);

  map.move_to(ctx, "a", 2);
  assert(ctx.is_set(count) && "len must stay cached on pure move");
  assert(ctx.is_set(has_b) && "contains_key must stay cached on pure move");
}

TEST(test_cellmap_move_before_after) {
  Context ctx;
  CellMap<int, int> map(ctx);
  for (int k = 0; k < 4; ++k) map.entry(ctx, k, k * 10);

  assert(map.move_before(ctx, 3, 1));
  auto keys = map.keys(ctx);
  assert(keys == std::vector<int>({0, 3, 1, 2}));

  assert(map.move_after(ctx, 0, 2));
  keys = map.keys(ctx);
  assert(keys == std::vector<int>({3, 1, 2, 0}));
}

// -- CellTree --

TEST(test_celltree_basic) {
  Context ctx;
  auto root = CellTree<std::string, std::string>::leaf(ctx, "root", "doc");
  auto a = root.insert_child(ctx, "a", "alpha");
  root.insert_child(ctx, "b", "bravo");

  auto ids = root.child_ids(ctx);
  assert(ids.size() == 2 && ids[0] == "a" && ids[1] == "b");

  a.set(ctx, "ALPHA");
  assert(a.get(ctx) == "ALPHA");

  root.move_child(ctx, "a", 1);
  ids = root.child_ids(ctx);
  assert(ids.size() == 2 && ids[0] == "b" && ids[1] == "a");
}

TEST(test_celltree_remove) {
  Context ctx;
  auto root = CellTree<int, int>::leaf(ctx, 0, 0);
  root.insert_child(ctx, 1, 10);
  root.insert_child(ctx, 2, 20);
  root.insert_child(ctx, 3, 30);

  assert(root.child_count(ctx) == 3);
  assert(root.remove_child(ctx, 2));
  assert(root.child_count(ctx) == 2);
  assert(!root.has_child(2));

  auto ids = root.child_ids(ctx);
  assert(ids.size() == 2 && ids[0] == 1 && ids[1] == 3);
}

// -- Reconcile (LIS) --

TEST(test_reconcile_pure_reorder) {
  std::vector<std::pair<std::string, int>> old_seq = {{"a", 1}, {"b", 2}, {"c", 3}, {"d", 4}};
  std::vector<std::pair<std::string, int>> new_seq = {{"a", 1}, {"c", 3}, {"b", 2}, {"d", 4}};
  auto ops = reconcile(old_seq, new_seq);
  bool all_moves = true;
  for (auto& op : ops) {
    if (op.kind != DiffOp<std::string, int>::Kind::Move) all_moves = false;
  }
  assert(all_moves);
  assert(ops.size() == 1);
}

TEST(test_reconcile_insert_remove_update) {
  std::vector<std::pair<std::string, int>> old_seq = {{"a", 1}, {"b", 2}, {"c", 3}};
  std::vector<std::pair<std::string, int>> new_seq = {{"c", 3}, {"a", 9}, {"d", 4}};
  auto ops = reconcile(old_seq, new_seq);

  bool has_remove_b = false, has_insert_d = false, has_update_a = false;
  for (auto& op : ops) {
    if (op.kind == DiffOp<std::string, int>::Kind::Remove && op.key == "b") has_remove_b = true;
    if (op.kind == DiffOp<std::string, int>::Kind::Insert && op.key == "d") has_insert_d = true;
    if (op.kind == DiffOp<std::string, int>::Kind::Update && op.key == "a" && *op.value == 9) has_update_a = true;
  }
  assert(has_remove_b && has_insert_d && has_update_a);
}

TEST(test_reconcile_full_reversal) {
  std::vector<std::pair<int, int>> old_seq = {{1, 0}, {2, 0}, {3, 0}, {4, 0}};
  std::vector<std::pair<int, int>> new_seq = {{4, 0}, {3, 0}, {2, 0}, {1, 0}};
  auto ops = reconcile(old_seq, new_seq);
  size_t moves = 0;
  for (auto& op : ops) if (op.kind == DiffOp<int, int>::Kind::Move) moves++;
  assert(moves == 3);
}

TEST(test_reconcile_apply_to_map) {
  Context ctx;
  CellMap<std::string, int> map(ctx);
  map.entry(ctx, "a", 1);
  map.entry(ctx, "b", 2);
  map.entry(ctx, "c", 3);

  auto a_handle = map.handle("a").value();

  auto a_view = ctx.computed<int>([&](Context& c) {
    return map.get(c, "a").value_or(0) * 100;
  });
  assert(ctx.get(a_view) == 100);

  map.reconcile(ctx, {{"a", 1}, {"c", 3}, {"b", 2}});
  auto keys = map.keys(ctx);
  assert(keys.size() == 3 && keys[0] == "a" && keys[1] == "c" && keys[2] == "b");

  assert(ctx.is_set(a_view) && "stable entry not invalidated by sibling reorder");
  assert(map.handle("a").value().id == a_handle.id && "identity preserved");
}

TEST(test_reconcile_apply_to_tree) {
  Context ctx;
  auto root = CellTree<std::string, int>::leaf(ctx, "root", 0);
  root.insert_child(ctx, "a", 1);
  root.insert_child(ctx, "b", 2);
  root.insert_child(ctx, "c", 3);

  auto ops = reconcile<std::string, int>(
    {{"a", 1}, {"b", 2}, {"c", 3}},
    {{"c", 3}, {"a", 9}, {"d", 4}});
  apply_to_tree(ctx, root, ops);

  auto ids = root.child_ids(ctx);
  assert(ids.size() == 3 && ids[0] == "c" && ids[1] == "a" && ids[2] == "d");
  assert(root.child("a")->get(ctx) == 9);
  assert(root.child("d")->get(ctx) == 4);
  assert(!root.has_child("b"));
}

int main() {
  std::cout << "lazily-cpp collections tests: " << test_passed << "/" << test_count
            << " passed" << std::endl;
  return test_passed == test_count ? 0 : 1;
}
