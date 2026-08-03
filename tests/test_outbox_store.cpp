#include <lazily/lazily.hpp>

#include "test_spec_fixture.hpp"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace lazily;

static IpcMessage frame(Epoch epoch) { return IpcMessageDelta{Delta{epoch - 1, epoch, {}}}; }

static std::vector<Epoch> json_epochs(const lazily_test::Json& value) {
  std::vector<Epoch> out;
  for (const auto& item : lazily_test::json_array(value))
    out.push_back(lazily_test::json_u64(*item));
  return out;
}

static std::vector<Epoch> replay_epochs(const std::vector<std::pair<Epoch, IpcMessage>>& replay) {
  std::vector<Epoch> out;
  out.reserve(replay.size());
  for (const auto& entry : replay)
    out.push_back(entry.first);
  return out;
}

static void require_fixture_keys(const lazily_test::Json& object,
                                 std::initializer_list<const char*> expected,
                                 const std::string& where) {
  REQUIRE(object.is_object(), where + ": expected object");
  std::set<std::string> actual;
  for (const auto& entry : object.object)
    actual.insert(entry.first);
  std::set<std::string> want;
  for (const char* key : expected)
    want.insert(key);
  REQUIRE(actual == want, where + ": fixture object keys changed");
}

struct TempJournal {
  std::filesystem::path directory;
  std::filesystem::path file;

  explicit TempJournal(const std::string& suffix) {
    const auto nonce = std::chrono::high_resolution_clock::now().time_since_epoch().count();
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
  static_assert(is_outbox_store_v<InMemoryStore>, "in-memory byte store contract");
  static_assert(is_outbox_store_v<FileOutboxStore>, "file byte store contract");
  constexpr const char* fixture_id = "reliable-sync/outbox_store_protocol.json";
  const auto root = lazily_test::parse_json(
      lazily_test::spec_fixture_text("reliable-sync", "outbox_store_protocol.json"));
  require_fixture_keys(*root, {"description", "protocol_version", "kind", "model", "scenarios"},
                       fixture_id);
  REQUIRE(lazily_test::json_u64(lazily_test::json_member(*root, "protocol_version")) == 1,
          "outbox-store protocol_version");
  REQUIRE(lazily_test::json_string(lazily_test::json_member(*root, "kind")) == "ReliableSync",
          "outbox-store kind");
  REQUIRE(lazily_test::json_string(lazily_test::json_member(*root, "model")) == "OutboxStore",
          "outbox-store model");

  const auto& raw_scenarios = lazily_test::json_array(lazily_test::json_member(*root, "scenarios"));
  for (const auto& view : lazily_test::scenario_views(fixture_id, raw_scenarios)) {
    const auto& scenario = view.replay();
    const std::string where = std::string(fixture_id) + " " + view.id();
    lazily_test::AssertionKeys expected(where + " expect",
                                        lazily_test::json_member(scenario, "expect"));

    if (scenario.has("open_handles")) {
      require_fixture_keys(
          scenario, {"id", "name", "open_handles", "save_cursor", "restart", "expect"}, where);
      TempJournal journal(view.id());
      std::map<std::string, std::unique_ptr<FileOutbox>> handles;
      for (const auto& handle :
           lazily_test::json_array(lazily_test::json_member(scenario, "open_handles"))) {
        const std::string name = lazily_test::json_string(*handle);
        handles.emplace(name, std::make_unique<FileOutbox>(FileOutboxStore(journal.file)));
      }
      for (const auto& save :
           lazily_test::json_array(lazily_test::json_member(scenario, "save_cursor"))) {
        const std::string name =
            lazily_test::json_string(lazily_test::json_member(*save, "handle"));
        const Epoch epoch = lazily_test::json_u64(lazily_test::json_member(*save, "epoch"));
        REQUIRE(handles.count(name) == 1, where + ": save_cursor names an unopened handle");
        handles.at(name)->ack_through(epoch);
      }
      REQUIRE(lazily_test::json_bool(lazily_test::json_member(scenario, "restart")),
              where + ": stale-handle scenario requires restart");
      FileOutbox reopened{FileOutboxStore(journal.file)};
      expected.assert_key("loaded_cursor", reopened.acked_through());
      continue;
    }

    InMemoryOutbox outbox;
    const auto put_epochs = json_epochs(lazily_test::json_member(scenario, "put_epochs"));
    for (const Epoch epoch : put_epochs)
      outbox.append(epoch, frame(epoch));
    if (const auto* ack = scenario.find("ack_through"))
      for (const Epoch epoch : json_epochs(*ack))
        outbox.ack_through(epoch);

    if (const auto* scan_after = scenario.find("scan_after")) {
      require_fixture_keys(scenario, {"id", "name", "put_epochs", "scan_after", "expect"}, where);
      expected.assert_key("epochs",
                          replay_epochs(outbox.replay_from(lazily_test::json_u64(*scan_after))),
                          json_epochs);
    } else if (scenario.has("restart")) {
      require_fixture_keys(scenario,
                           {"id", "name", "put_epochs", "ack_through", "restart", "expect"}, where);
      REQUIRE(lazily_test::json_bool(lazily_test::json_member(scenario, "restart")),
              where + ": restart scenario must request a reopen");
      auto memory_store = std::move(outbox).into_store();
      InMemoryOutbox reopened_memory(std::move(memory_store));

      TempJournal journal(view.id());
      {
        FileOutbox durable{FileOutboxStore(journal.file)};
        for (const Epoch epoch : put_epochs)
          durable.append(epoch, frame(epoch));
        for (const Epoch epoch : json_epochs(lazily_test::json_member(scenario, "ack_through")))
          durable.ack_through(epoch);
      }
      FileOutbox reopened_file{FileOutboxStore(journal.file)};
      REQUIRE(reopened_file.acked_through() == reopened_memory.acked_through(),
              where + ": file and memory cursors disagree");
      REQUIRE(reopened_file.retained_epochs() == reopened_memory.retained_epochs(),
              where + ": file and memory retained suffixes disagree");
      REQUIRE(replay_epochs(reopened_file.replay_from(0)) ==
                  replay_epochs(reopened_memory.replay_from(0)),
              where + ": file and memory replay suffixes disagree");

      expected.assert_key("loaded_cursor", reopened_memory.acked_through());
      expected.assert_key("retained", reopened_memory.retained_epochs(), json_epochs);
      expected.assert_key("replay", replay_epochs(reopened_memory.replay_from(0)), json_epochs);
    } else {
      require_fixture_keys(scenario, {"id", "name", "put_epochs", "ack_through", "expect"}, where);
      expected.assert_key("cursor", outbox.acked_through());
      expected.assert_key("retained", outbox.retained_epochs(), json_epochs);
      expected.assert_key("replay_from_zero", replay_epochs(outbox.replay_from(0)), json_epochs);
    }
  }

  // Independent handles also produce whole, ordered records under contention.
  TempJournal stale_journal("race");
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
