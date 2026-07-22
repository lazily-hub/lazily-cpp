#include <lazily/lazily.hpp>

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace lazily;

static int test_count = 0;
static int test_passed = 0;

#define TEST(name)                                                             \
  static void name();                                                          \
  struct name##_runner {                                                       \
    name##_runner() {                                                          \
      ++test_count;                                                            \
      name();                                                                  \
      ++test_passed;                                                           \
    }                                                                          \
  } name##_instance;                                                           \
  static void name()

// Helper: a reactive reader that caches the observed value of a QueueCell
// observable. After creation, `is_set()` reports whether the cached value is
// still valid (not invalidated); `value()` returns the cached value.
template <typename T> struct Reader {
  Context &ctx;
  Computed<T> slot;
  T cached;
  Reader(Context &c, std::function<T(Compute &)> fn)
      : ctx(c), slot(c.computed<T>([fn](Compute &cc) { return fn(cc); })) {
    cached = ctx.get(slot);
  }
  const T &value() { return cached; }
  bool is_valid() { return ctx.is_set(slot); }
  void refresh() { cached = ctx.get(slot); }
};

struct BoolReader {
  Context &ctx;
  Computed<bool> slot;
  bool cached;
  BoolReader(Context &c, std::function<bool(Compute &)> fn)
      : ctx(c), slot(c.computed<bool>([fn](Compute &cc) { return fn(cc); })) {
    cached = ctx.get(slot);
  }
  bool value() { return cached; }
  bool is_valid() { return ctx.is_set(slot); }
  void refresh() { cached = ctx.get(slot); }
};

struct OptReader {
  Context &ctx;
  Computed<std::optional<std::string>> slot;
  std::optional<std::string> cached;
  OptReader(Context &c, std::function<std::optional<std::string>(Compute &)> fn)
      : ctx(c), slot(c.computed<std::optional<std::string>>(
                    [fn](Compute &cc) { return fn(cc); })) {
    cached = ctx.get(slot);
  }
  const std::optional<std::string> &value() { return cached; }
  bool is_valid() { return ctx.is_set(slot); }
  void refresh() { cached = ctx.get(slot); }
};

struct SizeReader {
  Context &ctx;
  Computed<size_t> slot;
  size_t cached;
  SizeReader(Context &c, std::function<size_t(Compute &)> fn)
      : ctx(c), slot(c.computed<size_t>([fn](Compute &cc) { return fn(cc); })) {
    cached = ctx.get(slot);
  }
  size_t value() { return cached; }
  bool is_valid() { return ctx.is_set(slot); }
  void refresh() { cached = ctx.get(slot); }
};

// Capture all five reader kinds around a mutating op, mirroring the
// `invalidates` object in the conformance fixtures.
struct InvSnapshot {
  bool head_valid, len_valid, empty_valid, full_valid, closed_valid;
};

struct Readers {
  OptReader head;
  SizeReader len;
  BoolReader empty;
  BoolReader full;
  BoolReader closed;
  Readers(Context &ctx, QueueCell<std::string> &q)
      : head(ctx, [&](Compute &c) { return q.head(c); }),
        len(ctx, [&](Compute &c) { return q.len(c); }),
        empty(ctx, [&](Compute &c) { return q.is_empty(c); }),
        full(ctx, [&](Compute &c) { return q.is_full(c); }),
        closed(ctx, [&](Compute &c) { return q.closed(c); }) {}
  InvSnapshot snap() {
    return {head.is_valid(), len.is_valid(), empty.is_valid(), full.is_valid(),
            closed.is_valid()};
  }
  void refresh() {
    head.refresh();
    len.refresh();
    empty.refresh();
    full.refresh();
    closed.refresh();
  }
};

static void check_inv(const char *label, const InvSnapshot &s, bool head,
                      bool len, bool emp, bool full, bool clsd) {
  // `s.*_valid` is true while the reader is STILL VALID (not invalidated).
  // The expected booleans follow the conformance `invalidates` polarity:
  // true = the reader WAS invalidated (no longer valid).
  bool ok = (!s.head_valid == head) && (!s.len_valid == len) &&
            (!s.empty_valid == emp) && (!s.full_valid == full) &&
            (!s.closed_valid == clsd);
  if (!ok) {
    std::cerr << "  invalidation mismatch [" << label
              << "]: head=" << (!s.head_valid) << "/" << head
              << " len=" << (!s.len_valid) << "/" << len
              << " empty=" << (!s.empty_valid) << "/" << emp
              << " full=" << (!s.full_valid) << "/" << full
              << " closed=" << (!s.closed_valid) << "/" << clsd << "\n";
  }
  assert(ok && "invalidation mismatch");
}

// -- queuecell_spsc_push_pop.json --
TEST(test_queue_spsc_push_pop) {
  Context ctx;
  QueueCell<std::string> q(ctx);
  Readers r(ctx, q);

  // push "a" to empty: head/len/empty invalidated, is_full/closed not.
  q.push(ctx, "a");
  check_inv("push a", r.snap(), true, true, true, false, false);
  r.refresh();
  assert(r.head.value().value() == "a");
  assert(r.len.value() == 1);
  assert(!r.empty.value());

  // push "b" to nonempty: only len invalidated.
  q.push(ctx, "b");
  check_inv("push b", r.snap(), false, true, false, false, false);
  r.refresh();
  assert(r.head.value().value() == "a");
  assert(r.len.value() == 2);

  // pop "a": head/len invalidated, empty not (still nonempty).
  assert(q.pop(ctx).value() == "a");
  check_inv("pop a", r.snap(), true, true, false, false, false);
  r.refresh();
  assert(r.head.value().value() == "b");
  assert(r.len.value() == 1);

  // pop "b": head/len/empty invalidated (queue empties).
  assert(q.pop(ctx).value() == "b");
  check_inv("pop b", r.snap(), true, true, true, false, false);
  r.refresh();
  assert(!r.head.value().has_value());
  assert(r.len.value() == 0);
  assert(r.empty.value());

  // pop on empty: no invalidation, returns Empty.
  assert(!q.pop(ctx).has_value());
  check_inv("pop empty", r.snap(), false, false, false, false, false);
}

// -- queuecell_closure_lifecycle.json --
TEST(test_queue_closure_lifecycle) {
  Context ctx;
  QueueCell<std::string> q(ctx);
  Readers r(ctx, q);

  q.push(ctx, "a");
  check_inv("push a", r.snap(), true, true, true, false, false);
  r.refresh();

  q.push(ctx, "b");
  check_inv("push b", r.snap(), false, true, false, false, false);
  r.refresh();

  // close (first): only closed invalidated.
  q.close(ctx);
  check_inv("close", r.snap(), false, false, false, false, true);
  r.refresh();
  assert(r.closed.value());

  // pop on closed+nonempty: drains, head/len invalidated.
  assert(q.pop(ctx).value() == "a");
  check_inv("pop closed+nonempty", r.snap(), true, true, false, false, false);
  r.refresh();

  // pop last: empties, head/len/empty invalidated.
  assert(q.pop(ctx).value() == "b");
  check_inv("pop last", r.snap(), true, true, true, false, false);
  r.refresh();

  // try_pop on closed+empty: returns Closed, no invalidation.
  {
    auto res = q.try_pop(ctx);
    assert(res.is_closed());
  }
  check_inv("try_pop closed+empty", r.snap(), false, false, false, false,
            false);

  // try_push on closed: returns Closed, no invalidation.
  {
    auto res = q.try_push(ctx, "c");
    assert(res == PushResult::Closed);
  }
  check_inv("try_push closed", r.snap(), false, false, false, false, false);

  // close again: idempotent, no invalidation.
  q.close(ctx);
  check_inv("close idempotent", r.snap(), false, false, false, false, false);
  r.refresh();
  assert(r.closed.value());
}

// -- queuecell_popped_head_observation.json --
TEST(test_queue_popped_head_observation) {
  Context ctx;
  QueueCell<std::string> q(ctx);
  Readers r(ctx, q);

  q.push(ctx, "a");
  check_inv("push a", r.snap(), true, true, true, false, false);
  r.refresh();
  assert(r.head.value().value() == "a");

  q.push(ctx, "b");
  check_inv("push b", r.snap(), false, true, false, false, false);
  r.refresh();
  assert(r.head.value().value() == "a");

  q.push(ctx, "c");
  check_inv("push c", r.snap(), false, true, false, false, false);
  r.refresh();
  assert(r.head.value().value() == "a");

  assert(q.pop(ctx).value() == "a");
  check_inv("pop a", r.snap(), true, true, false, false, false);
  r.refresh();
  assert(r.head.value().value() == "b");

  assert(q.pop(ctx).value() == "b");
  check_inv("pop b", r.snap(), true, true, false, false, false);
  r.refresh();
  assert(r.head.value().value() == "c");

  assert(q.pop(ctx).value() == "c");
  check_inv("pop c", r.snap(), true, true, true, false, false);
  r.refresh();
  assert(!r.head.value().has_value());
  assert(r.empty.value());

  // push to empty again: head + empty invalidated.
  q.push(ctx, "d");
  check_inv("push d", r.snap(), true, true, true, false, false);
  r.refresh();
  assert(r.head.value().value() == "d");
  assert(!r.empty.value());
}

// -- queuecell_mpsc_multi_writer.json --
TEST(test_queue_mpsc_multi_writer) {
  Context ctx;
  QueueCell<std::string> q(ctx);
  Readers r(ctx, q);

  // Producer 1 pushes a1, a2 inside a batch (atomic, per-producer FIFO).
  ctx.batch([&](Context &c) {
    q.push(c, "a1");
    q.push(c, "a2");
  });
  check_inv("batch p1", r.snap(), true, true, true, false, false);
  r.refresh();
  assert(r.head.value().value() == "a1");
  assert(r.len.value() == 2);

  // Producer 2 pushes b1, b2 inside a batch. Head unchanged (nonempty).
  ctx.batch([&](Context &c) {
    q.push(c, "b1");
    q.push(c, "b2");
  });
  check_inv("batch p2", r.snap(), false, true, false, false, false);
  r.refresh();
  assert(r.head.value().value() == "a1");
  assert(r.len.value() == 4);

  // Drain — per-producer FIFO preserved (a1, a2, b1, b2).
  assert(q.pop(ctx).value() == "a1");
  r.refresh();
  assert(q.pop(ctx).value() == "a2");
  r.refresh();
  assert(q.pop(ctx).value() == "b1");
  r.refresh();
  assert(q.pop(ctx).value() == "b2");
  r.refresh();
  assert(r.empty.value());
  assert(r.len.value() == 0);
}

// -- queuecell_bounded_backpressure.json --
TEST(test_queue_bounded_backpressure) {
  Context ctx;
  auto q = QueueCell<std::string>::bounded(ctx, 2);
  Readers r(ctx, q);

  q.push(ctx, "a");
  check_inv("push a", r.snap(), true, true, true, false, false);
  r.refresh();
  assert(!r.full.value());

  // push "b": fills to capacity, is_full invalidated (false -> true).
  q.push(ctx, "b");
  check_inv("push b", r.snap(), false, true, false, true, false);
  r.refresh();
  assert(r.full.value());
  assert(q.len(ctx) == 2);

  // try_push at capacity: returns Full, no invalidation.
  {
    auto res = q.try_push(ctx, "c");
    assert(res == PushResult::Full);
  }
  check_inv("try_push full", r.snap(), false, false, false, false, false);
  r.refresh();
  assert(r.full.value());

  // pop from full: un-fills, is_full invalidated (true -> false), head/len too.
  assert(q.pop(ctx).value() == "a");
  check_inv("pop from full", r.snap(), true, true, false, true, false);
  r.refresh();
  assert(!r.full.value());
  assert(r.head.value().value() == "b");

  // push "c": fills again, is_full invalidated (false -> true).
  q.push(ctx, "c");
  check_inv("push c", r.snap(), false, true, false, true, false);
  r.refresh();
  assert(r.full.value());
  assert(q.len(ctx) == 2);
}

// -- Reactive backpressure: an effect observing is_full resumes on pop --
TEST(test_queue_backpressure_effect) {
  Context ctx;
  auto q = QueueCell<std::string>::bounded(ctx, 1);

  int resume_count = 0;
  bool last_full = false;
  auto eff = ctx.effect_void([&](Compute &c) {
    bool f = q.is_full(c);
    if (f != last_full || resume_count == 0) {
      last_full = f;
      ++resume_count;
    }
  });
  (void)eff;

  assert(resume_count == 1); // initial run, not full
  q.push(ctx, "a");
  assert(resume_count == 2); // became full -> effect reran
  assert(q.try_push(ctx, "b") == PushResult::Full);
  assert(resume_count == 2); // no change, no rerun
  assert(q.pop(ctx).value() == "a");
  assert(resume_count == 3); // capacity recovered -> effect reran
}

// -- Custom storage backend (compile-time pluggable seam) --

template <typename T> class RingStorage {
public:
  explicit RingStorage(size_t cap) : buf_(cap), cap_(cap) {}

  PushResult try_push(T v) {
    if (closed_)
      return PushResult::Closed;
    if (count_ == cap_)
      return PushResult::Full;
    buf_[tail_] = std::move(v);
    tail_ = (tail_ + 1) % cap_;
    ++count_;
    return PushResult::Ok;
  }

  PopResult<T> try_pop() {
    if (count_ == 0)
      return closed_ ? PopResult<T>::closed() : PopResult<T>::empty();
    T v = std::move(buf_[head_]);
    head_ = (head_ + 1) % cap_;
    --count_;
    return PopResult<T>::with_value(std::move(v));
  }

  std::optional<T> head() const {
    if (count_ == 0)
      return std::nullopt;
    return buf_[head_];
  }
  size_t len() const { return count_; }
  std::optional<size_t> capacity() const { return cap_; }
  bool is_full() const { return count_ == cap_; }
  bool is_closed() const { return closed_; }
  void close() { closed_ = true; }

private:
  std::vector<T> buf_;
  size_t cap_, head_ = 0, tail_ = 0, count_ = 0;
  bool closed_ = false;
};

TEST(test_queue_custom_storage) {
  Context ctx;
  QueueCell<int, RingStorage<int>> q(ctx, RingStorage<int>(2));
  assert(q.capacity().value() == 2);

  q.push(ctx, 10);
  q.push(ctx, 20);
  assert(q.is_full(ctx));
  assert(q.try_push(ctx, 30) == PushResult::Full);
  assert(q.pop(ctx).value() == 10);
  assert(q.pop(ctx).value() == 20);
  assert(q.is_empty(ctx));

  q.close(ctx);
  assert(q.try_push(ctx, 40) == PushResult::Closed);
  assert(q.try_pop(ctx).is_closed());
}

// -- Storage closure semantics: Closed distinct from Empty --
TEST(test_queue_closed_distinct_from_empty) {
  Context ctx;
  QueueCell<int> q(ctx);
  // Open + empty -> Empty
  assert(q.try_pop(ctx).is_empty());
  q.close(ctx);
  // Closed + empty -> Closed
  assert(q.try_pop(ctx).is_closed());
}

// -- Minimal contract (Phase 0 #relaycell): a raw-channel-style backend with
// ONLY try_push / try_pop / len / is_closed / close — no head, capacity, or
// is_full — is fully conforming (no head reader, never full). --
template <typename T> class MinimalFifo {
public:
  PushResult try_push(T v) {
    if (closed_)
      return PushResult::Closed;
    buf_.push_back(std::move(v));
    return PushResult::Ok;
  }
  PopResult<T> try_pop() {
    if (buf_.empty())
      return closed_ ? PopResult<T>::closed() : PopResult<T>::empty();
    T v = std::move(buf_.front());
    buf_.pop_front();
    return PopResult<T>::with_value(std::move(v));
  }
  size_t len() const { return buf_.size(); }
  bool is_closed() const { return closed_; }
  void close() { closed_ = true; }
  // NB: no head(), no capacity(), no is_full().

private:
  std::deque<T> buf_;
  bool closed_ = false;
};

TEST(test_queue_raw_channel_minimal_contract) {
  Context ctx;
  QueueCell<int, MinimalFifo<int>> q(ctx, MinimalFifo<int>());

  assert(q.is_empty(ctx));
  assert(q.try_push(ctx, 1) == PushResult::Ok);
  assert(q.try_push(ctx, 2) == PushResult::Ok);
  assert(q.len(ctx) == 2);

  // No head() capability -> no head reader (nullopt); no capacity -> never
  // full.
  assert(!q.head(ctx).has_value());
  assert(!q.is_full(ctx));
  assert(!q.capacity().has_value());

  assert(q.pop(ctx).value() == 1);
  assert(q.pop(ctx).value() == 2);
  assert(q.is_empty(ctx));

  q.close(ctx);
  assert(q.closed(ctx));
  assert(q.try_push(ctx, 3) == PushResult::Closed);
  assert(q.try_pop(ctx).is_closed());
}

// A subscribed reader over the minimal backend stays reactive (the len version
// cell is bumped each op) even without head/capacity/is_full.
TEST(test_queue_raw_channel_reader_reactive) {
  Context ctx;
  QueueCell<int, MinimalFifo<int>> q(ctx, MinimalFifo<int>());
  std::vector<size_t> log;
  auto eff = ctx.effect_void([&](Compute &c) { log.push_back(q.len(c)); });
  (void)eff;

  assert(log.size() == 1 && log[0] == 0);
  q.push(ctx, 10);
  assert(log.size() == 2 && log[1] == 1);
  q.pop(ctx);
assert(log.size() == 3 && log[2] == 0);
}

TEST(test_topic_broadcast_cursor_isolation) {
Context ctx;
TopicCell<std::string> topic(ctx);
assert(topic.subscribe(ctx, "alice") == TopicSubscribeOutcome::Subscribed);
topic.subscribe(ctx, "bob");
assert(topic.publish(ctx, "a") == 0);
assert(topic.publish(ctx, "b") == 1);
assert(topic.advance(ctx, "alice") == 1);
assert(topic.read_stream(ctx, "alice") == std::vector<std::string>{"b"});
assert(topic.read_stream(ctx, "bob") ==
(std::vector<std::string>{"a", "b"}));
}

TEST(test_topic_durable_replay_and_gc) {
Context ctx;
TopicCell<std::string> topic(ctx);
topic.subscribe(ctx, "fast");
topic.subscribe(ctx, "slow");
topic.publish(ctx, "a");
topic.publish(ctx, "b");
topic.advance(ctx, "fast", 2);
topic.advance(ctx, "slow");
topic.disconnect(ctx, "slow");
topic.publish(ctx, "c");
assert(topic.gc() == 1);
topic.reconnect(ctx, "slow");
assert(topic.read_stream(ctx, "slow") ==
(std::vector<std::string>{"b", "c"}));

Context restored_ctx;
TopicCell<std::string> restored(restored_ctx, topic.snapshot());
assert(restored.base_offset() == topic.base_offset());
assert(restored.elements() == topic.elements());
}

TEST(test_topic_ephemeral_lifecycle) {
Context ctx;
TopicCell<std::string> topic(ctx);
topic.subscribe(ctx, "durable");
topic.subscribe(ctx, "viewer", TopicDurability::Ephemeral);
topic.publish(ctx, "a");
topic.advance(ctx, "durable");
topic.disconnect(ctx, "viewer");
assert(!topic.subscription("viewer").has_value());
assert(topic.gc() == 1);
topic.subscribe(ctx, "viewer", TopicDurability::Ephemeral);
assert(topic.subscription("viewer")->cursor == topic.tail_offset());
}

TEST(test_topic_tail_and_offline_advance_are_noops) {
Context ctx;
TopicCell<std::string> topic(ctx);
topic.subscribe(ctx, "worker");
topic.publish(ctx, "a");
assert(topic.advance(ctx, "worker") == 1);
assert(topic.advance(ctx, "worker") == 1);

topic.disconnect(ctx, "worker");
topic.publish(ctx, "b");
assert(topic.read_stream(ctx, "worker").empty());
assert(topic.advance(ctx, "worker") == 1);
assert(topic.subscription("worker")->cursor == 1);

topic.reconnect(ctx, "worker");
assert(topic.read_stream(ctx, "worker") == std::vector<std::string>{"b"});
assert(topic.gc() == 1);
assert(topic.base_offset() == 1);
assert(topic.subscription("worker")->cursor == 1);
}

TEST(test_topic_snapshot_rejects_disconnected_ephemeral) {
Context ctx;
TopicSnapshot<std::string> snapshot;
snapshot.subscriptions.push_back(
{"viewer", 0, TopicDurability::Ephemeral, false});
bool rejected = false;
try {
TopicCell<std::string> topic(ctx, snapshot);
(void)topic;
} catch (const std::invalid_argument &) {
rejected = true;
}
assert(rejected);
}

int main() {
  std::cout << "lazily-cpp queue tests: " << test_passed << "/" << test_count
            << " passed" << std::endl;
  return test_passed == test_count ? 0 : 1;
}
