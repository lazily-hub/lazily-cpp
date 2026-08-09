// Keyed-map materialization conformance for the thread-safe and async shells,
// replayed from the canonical lazily-spec fixture bytes (`#lzcppmatreplay`).
//
// coverage.json scores the materialization family as three rows -- one per
// context flavor -- and cites `observational_transparency.json` /
// `deferral_not_deallocation.json` on them. Those two fixtures used to reach
// this binding only as hand transcriptions in
// `tests/test_thread_safe_reactive_family.cpp` and
// `tests/test_async_reactive_family.cpp`: the canonical values re-typed as
// `k * 3` / `k * 2`, the read sequences as brace-initialiser lists, the present
// sets as literal vectors. A transcription cannot detect drift from the corpus
// it was copied out of, which is why all three rows scored `~`.
//
// The replay itself lives in `test_materialization_replay.hpp` and is shared
// with the single-threaded runner, so all three shells replay the SAME bytes
// rather than three independent copies of them.
//
// ## Why this is a separate binary from test_materialization_conformance
//
// `ThreadSafeContext` and `AsyncContext` force the `-pthread` wasm tier (see
// wasm-tiers.conf, which both `tests/wasm.cmake` and the matrix guard read).
// The core-tier materialization runner has to stay buildable for single-threaded
// wasm32, so the shells that need threads get their own target -- the same split
// `test_collections_fixture_conformance` / `test_collections_family_conformance`
// already uses for exactly this reason.

// The umbrella `<lazily/lazily.hpp>` would drag in `transport.hpp` and
// `reliable_sync.hpp`, whose preprocessor guards reject wasm32 outright —
// reaching for it out of habit is what made six runners native-only in
// `#lzcppwasm`. Include exactly the two shells under test.
#include <lazily/async_reactive_family.hpp>
#include <lazily/core.hpp>
#include <lazily/thread_safe_reactive_family.hpp>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "test_materialization_replay.hpp"
#include "test_require.hpp"
#include "test_spec_fixture.hpp"

using namespace lazily;

namespace {

// The `Send + Sync` shell. `observe` is the thread-safe map's read-back
// accessor; the sync map spells the same operation `get`.
class ThreadSafeModel {
public:
  static const char* shell() { return "ThreadSafeContext/ThreadSafeComputedMap"; }

  template <typename Factory> uint32_t read(const std::string& key, Factory factory) {
    return slots_.get_or_insert_with(ctx_, key, factory);
  }

  template <typename Factory>
  void materialize_all(const std::vector<std::string>& keys, Factory factory) {
    slots_.materialize_all(ctx_, keys, factory);
  }

  std::optional<uint32_t> observe(const std::string& key) { return slots_.observe(ctx_, key); }
  std::vector<std::string> present_keys() const { return slots_.present_keys(); }
  std::size_t present_count() const { return slots_.present_count(); }
  bool is_present(const std::string& key) const { return slots_.is_present(key); }

private:
  ThreadSafeContext ctx_;
  ThreadSafeComputedMap<std::string, uint32_t> slots_{ctx_};
};

// The async shell. Transparency here is EVENTUAL: a derived slot reads
// `std::nullopt` until it is driven, so both the mint-on-access path and the
// read-back path drive the handle. That is the whole difference between this
// row and the other two, and it is why `read` and `observe` are separate
// operations in the shared replay contract rather than one accessor.
class AsyncModel {
public:
  static const char* shell() { return "AsyncContext/AsyncComputedMap"; }

  template <typename Factory> uint32_t read(const std::string& key, Factory factory) {
    return slots_.get_or_insert_handle(ctx_, key, factory).get_async().get();
  }

  template <typename Factory>
  void materialize_all(const std::vector<std::string>& keys, Factory factory) {
    slots_.materialize_all(ctx_, keys, factory);
  }

  std::optional<uint32_t> observe(const std::string& key) {
    auto handle = slots_.handle(key);
    if (!handle) return std::nullopt;
    return handle->get_async().get();
  }

  std::vector<std::string> present_keys() const { return slots_.present_keys(); }
  std::size_t present_count() const { return slots_.present_count(); }
  bool is_present(const std::string& key) const { return slots_.is_present(key); }

private:
  AsyncContext ctx_;
  AsyncComputedMap<std::string, uint32_t> slots_{ctx_};
};

constexpr const char* kFixtures[] = {"observational_transparency.json",
                                     "deferral_not_deallocation.json"};

} // namespace

int main() {
  std::cout << "materialization family conformance:" << std::endl;
  for (const char* name : kFixtures) {
    const auto fixture = lazily_test::load_materialization_fixture(name);
    lazily_test::replay_materialization<ThreadSafeModel>(fixture);
    lazily_test::replay_materialization<AsyncModel>(fixture);
  }
  REQUIRE_FIXTURES_LOADED(2);
  return 0;
}
