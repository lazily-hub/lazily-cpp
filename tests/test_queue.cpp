#include <lazily/lazily.hpp>

#include <cassert>
#include <iostream>
#include <set>
#include <string>
#include <thread>
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

// The five canonical queue fixtures (queuecell_spsc_push_pop,
// queuecell_closure_lifecycle, queuecell_popped_head_observation,
// queuecell_mpsc_multi_writer, queuecell_bounded_backpressure) used to be
// transcribed here by hand, along with a Reader/InvSnapshot harness. They now
// live in test_queue_conformance.cpp, which opens the canonical bytes instead
// of copying them (#lzqfcppfx). Do not re-add a transcription: a copy goes
// green against the contract it was written for, not the one the spec holds
// today. What remains below is genuinely local — storage-backend seams, the
// minimal raw-channel contract, and the topic tests — none of which the
// collections corpus covers.

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

TEST(test_thread_safe_queue_concurrent_producers_are_confluent) {
  ThreadSafeContext ctx;
  ThreadSafeQueueCell<int> q(ctx);
  constexpr int producers = 4;
  constexpr int per_producer = 100;
  std::vector<std::thread> threads;
  for (int producer = 0; producer < producers; ++producer) {
    threads.emplace_back([&, producer] {
      for (int i = 0; i < per_producer; ++i)
        q.push(ctx, producer * per_producer + i);
    });
  }
  for (auto &thread : threads)
    thread.join();

  assert(q.len(ctx) == producers * per_producer);
  std::set<int> observed;
  while (auto value = q.pop(ctx))
    observed.insert(*value);
  assert(observed.size() == producers * per_producer);
  for (int value = 0; value < producers * per_producer; ++value)
    assert(observed.count(value) == 1);
}

TEST(test_thread_safe_topic_concurrent_publish_is_confluent) {
  ThreadSafeContext ctx;
  ThreadSafeTopicCell<int> topic(ctx);
  topic.subscribe(ctx, "subscriber");
  constexpr int publishers = 4;
  constexpr int per_publisher = 50;
  std::vector<std::thread> threads;
  for (int publisher = 0; publisher < publishers; ++publisher) {
    threads.emplace_back([&, publisher] {
      for (int i = 0; i < per_publisher; ++i)
        topic.publish(ctx, publisher * per_publisher + i);
    });
  }
  for (auto &thread : threads)
    thread.join();

  const auto stream = topic.read_stream(ctx, "subscriber");
  assert(stream.size() == publishers * per_publisher);
  const std::set<int> observed(stream.begin(), stream.end());
  assert(observed.size() == publishers * per_publisher);
}

TEST(test_topic_pre_minted_reader_invalidates_on_subscribe) {
  Context ctx;
  TopicCell<int> topic(ctx);
  const auto reader = topic.reader_handle(ctx, "late");
  assert(ctx.get(reader).empty());
  assert(ctx.is_set(reader));
  topic.subscribe(ctx, "late");
  assert(!ctx.is_set(reader));
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
  assert(topic.read_stream(ctx, "bob") == (std::vector<std::string>{"a", "b"}));
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
