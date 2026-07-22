#include <lazily/lazily.hpp>

#include <cassert>
#include <string>

using namespace lazily;

struct WorkQueueReaders {
  Context &ctx;
  Computed<size_t> pending;
  Computed<bool> empty;
  Computed<size_t> in_flight;
  Computed<size_t> dead;

  WorkQueueReaders(Context &context, WorkQueueCell<std::string> &queue)
      : ctx(context), pending(ctx.computed<size_t>(
                          [&](Compute &c) { return queue.pending_len(c); })),
        empty(ctx.computed<bool>([&](Compute &c) { return queue.is_empty(c); })),
        in_flight(ctx.computed<size_t>(
            [&](Compute &c) { return queue.in_flight_len(c); })),
        dead(ctx.computed<size_t>(
            [&](Compute &c) { return queue.dead_letter_len(c); })) {
    refresh();
  }

  void check(bool pending_invalid, bool empty_invalid, bool in_flight_invalid,
             bool dead_invalid) {
    assert(ctx.is_set(pending) != pending_invalid);
    assert(ctx.is_set(empty) != empty_invalid);
    assert(ctx.is_set(in_flight) != in_flight_invalid);
    assert(ctx.is_set(dead) != dead_invalid);
    refresh();
  }

  void refresh() {
    (void)ctx.get(pending);
    (void)ctx.get(empty);
    (void)ctx.get(in_flight);
    (void)ctx.get(dead);
  }
};

static void competing_delivery() {
  Context ctx;
  WorkQueueCell<std::string> queue(ctx, 10, 3);
  WorkQueueReaders readers(ctx, queue);
  assert(queue.push(ctx, "a") == 0);
  readers.check(true, true, false, false);
  assert(queue.push(ctx, "b") == 1);
  readers.check(true, false, false, false);
  auto first = queue.claim(ctx, "alpha", 100);
  assert(first && first->delivery_id == 0 && first->item_id == 0 &&
         first->attempt == 1 && first->deadline == 110);
  readers.check(true, false, true, false);
  auto second = queue.claim(ctx, "beta", 100);
  assert(second && second->delivery_id == 1 && second->item_id == 1);
  readers.check(true, true, true, false);
  assert(!queue.claim(ctx, "gamma", 100));
  readers.check(false, false, false, false);
  assert(!queue.ack(ctx, "alpha", second->delivery_id));
  readers.check(false, false, false, false);
  assert(queue.ack(ctx, "beta", second->delivery_id));
  readers.check(false, false, true, false);
  assert(queue.nack(ctx, "alpha", first->delivery_id));
  readers.check(true, true, true, false);
  auto retry = queue.claim(ctx, "gamma", 105);
  assert(retry && retry->delivery_id == 2 && retry->item_id == 0 &&
         retry->attempt == 2 && retry->deadline == 115);
  readers.check(true, true, true, false);
  assert(queue.ack(ctx, "gamma", retry->delivery_id));
  readers.check(false, false, true, false);
}

static void visibility_and_dead_letter() {
  Context ctx;
  WorkQueueCell<std::string> queue(ctx, 10, 2);
  WorkQueueReaders readers(ctx, queue);
  queue.push(ctx, "poison");
  readers.check(true, true, false, false);
  auto first = queue.claim(ctx, "worker-1", 0);
  assert(first && first->deadline == 10);
  readers.check(true, true, true, false);
  assert(queue.reap_expired(ctx, 10) == 0);
  readers.check(false, false, false, false);
  assert(queue.reap_expired(ctx, 11) == 1);
  readers.check(true, true, true, false);
  auto second = queue.claim(ctx, "worker-2", 11);
  assert(second && second->attempt == 2 && second->deadline == 21);
  readers.check(true, true, true, false);
  assert(queue.reap_expired(ctx, 21) == 0);
  readers.check(false, false, false, false);
  assert(queue.reap_expired(ctx, 22) == 1);
  readers.check(false, false, true, true);
  const auto dead = queue.dead_letter_items();
  assert(dead.size() == 1 && dead[0].item_id == 0 && dead[0].attempts == 2 &&
         dead[0].reason == WorkQueueDeadLetterReason::Expired);
}

int main() {
  competing_delivery();
  visibility_and_dead_letter();
  return 0;
}
