#include <lazily/lazily.hpp>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>
#include "test_spec_fixture.hpp"

using namespace lazily;

static IpcMessage frame(Epoch epoch) {
  return IpcMessageDelta{Delta{epoch - 1, epoch, {}}};
}

static std::string fixture_text() {
  return lazily_test::spec_fixture_text("reliable-sync", "outbox_store_protocol.json");
}

struct TempJournal {
  std::filesystem::path directory;
  std::filesystem::path file;

  explicit TempJournal(const std::string &suffix) {
    const auto nonce =
        std::chrono::high_resolution_clock::now().time_since_epoch().count();
    directory = std::filesystem::temp_directory_path() /
                ("lazily-cpp-outbox-" + std::to_string(nonce) + "-" + suffix);
    std::filesystem::create_directories(directory);
    file = directory / "outbox.bin";
  }

  ~TempJournal() {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
};

int main() {
  static_assert(is_outbox_store_v<InMemoryStore>,
                "in-memory byte store contract");
  static_assert(is_outbox_store_v<FileOutboxStore>, "file byte store contract");
  const auto fixture = fixture_text();
  assert(fixture.find("\"model\": \"OutboxStore\"") != std::string::npos);
  assert(fixture.find("unordered puts replay in ascending epoch order") !=
         std::string::npos);
  assert(fixture.find("ack cursor is monotone and prune-safe") !=
         std::string::npos);
  assert(fixture.find("restart reloads cursor and unacked suffix") !=
         std::string::npos);
  assert(fixture.find("stale handle cannot regress serialized cursor") !=
         std::string::npos);

  InMemoryOutbox outbox;
  outbox.append(3, frame(3));
  outbox.append(1, frame(1));
  outbox.append(2, frame(2));
  assert((outbox.retained_epochs() == std::vector<Epoch>{1, 2, 3}));
  const auto ordered = outbox.replay_from(0);
  assert(ordered.size() == 3);
  assert(ordered[0].first == 1 && ordered[1].first == 2 &&
         ordered[2].first == 3);

  InMemoryOutbox monotone;
  for (Epoch epoch = 1; epoch <= 4; ++epoch)
    monotone.append(epoch, frame(epoch));
  monotone.ack_through(2);
  monotone.ack_through(1);
  monotone.ack_through(3);
  assert(monotone.acked_through() == 3);
  assert((monotone.retained_epochs() == std::vector<Epoch>{4}));
  assert(monotone.replay_from(0).front().first == 4);

  InMemoryOutbox before_restart;
  for (Epoch epoch = 10; epoch <= 12; ++epoch)
    before_restart.append(epoch, frame(epoch));
  before_restart.ack_through(10);
  auto memory_store = std::move(before_restart).into_store();
  InMemoryOutbox after_restart(std::move(memory_store));
  assert(after_restart.acked_through() == 10);
  assert((after_restart.retained_epochs() == std::vector<Epoch>{11, 12}));

  TempJournal restart_journal("restart");
  {
    FileOutbox durable{FileOutboxStore(restart_journal.file)};
    durable.append(10, frame(10));
    durable.append(11, frame(11));
    durable.append(12, frame(12));
    durable.ack_through(10);
  }
  {
    FileOutbox reopened{FileOutboxStore(restart_journal.file)};
    assert(reopened.acked_through() == 10);
    assert((reopened.retained_epochs() == std::vector<Epoch>{11, 12}));
    const auto replay = reopened.replay_from(0);
    assert(replay.size() == 2 && replay[0].first == 11 &&
           replay[1].first == 12);
  }

  // Regression: two handles opened before either acknowledgement must serialize
  // their cursor as max(existing, incoming). The late stale writer cannot turn
  // persisted 9 back into 3, and its own protocol view refreshes to 9.
  TempJournal stale_journal("stale");
  FileOutbox current{FileOutboxStore(stale_journal.file)};
  FileOutbox stale{FileOutboxStore(stale_journal.file)};
  current.ack_through(9);
  stale.ack_through(3);
  assert(stale.acked_through() == 9);
  FileOutbox stale_reopened{FileOutboxStore(stale_journal.file)};
  assert(stale_reopened.acked_through() == 9);

  // Independent handles also produce whole, ordered records under contention.
  FileOutboxStore high(stale_journal.file);
  FileOutboxStore low(stale_journal.file);
  std::thread high_writer([&]() { high.save_cursor(25); });
  std::thread low_writer([&]() { low.save_cursor(18); });
  high_writer.join();
  low_writer.join();
  FileOutboxStore after_race(stale_journal.file);
  assert(after_race.load_cursor() == 25);
  REQUIRE_FIXTURES_LOADED(1);
  return 0;
}
