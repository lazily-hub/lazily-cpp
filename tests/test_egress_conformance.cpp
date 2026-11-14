// The reactive egress contract (`#lzegress`), replayed against EVERY flavour
// this binding ships -- with a ledger that is *enforced* rather than advisory.
//
// lazily-cpp ships all three: `EgressCell` / `ThreadSafeEgressCell` /
// `AsyncEgressCell`, matching the reactive-egress, egress-thread-safe and
// egress-async coverage rows. Mirrors lazily-rs
// tests/egress_family_conformance.rs.
//
// The fixtures carry a `model` field naming the delivery authority
// (`EgressCore`) and no execution-model field; one flavour-neutral model
// template replays the same JSON against each shell. The transport attachment
// has separate unit coverage in test_egress.cpp, because the corpus describes
// the delivery authority beneath any concrete transport.
//
// Three things keep this suite from reporting green while testing nothing:
//
//   * `every_flavour_is_defined` greps include/lazily for each flavour's type
//     definition, in BOTH directions, so the ledger cannot rot.
//   * Every replay returns the number of steps it EXECUTED, and every flavour
//     asserts that count equals the corpus total loaded from disk. An op this
//     runner does not know aborts rather than being skipped.
//   * `invalidates` is asserted in BOTH directions through a cache-validity
//     probe per reader kind, and `the_invalidation_probe_discriminates` pins
//     the probe itself -- a probe that can only say "valid" would pass every
//     `false` expectation for free.

#include <lazily/egress.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "test_assertion_keys.hpp"
#include "test_json.hpp"
#include "test_require.hpp"
#include "test_spec_fixture.hpp"

using lazily_test::Json;
using lazily_test::json_bool;
using lazily_test::json_optional_u64;
using lazily_test::json_string;
using lazily_test::json_u64;
using lazily_test::JsonParser;
using lazily_test::JsonPtr;

using lazily::AsyncContext;
using lazily::AsyncEgressCell;
using lazily::Context;
using lazily::EgressAck;
using lazily::EgressAckKind;
using lazily::EgressCell;
using lazily::EgressClaim;
using lazily::EgressClaimKind;
using lazily::EgressEnvelope;
using lazily::EgressFailure;
using lazily::EgressFailureKind;
using lazily::EgressPolicy;
using lazily::EgressReconnect;
using lazily::EgressReconnectKind;
using lazily::EgressRetry;
using lazily::EgressRetryAction;
using lazily::EgressRetryActionKind;
using lazily::ThreadSafeContext;
using lazily::ThreadSafeEgressCell;

namespace {

constexpr const char* kArea = "egress";

using Payload = std::uint64_t;
using Envelope = EgressEnvelope<Payload>;

// Named explicitly rather than globbed: a fixture added upstream and not here is
// a MISSING REPLAY, and scripts/check-conformance-coverage.sh is what notices.
// (egress/latest_durable_projection.json and egress/effect_cas.json are other
// models in the same area, replayed by their own runners.)
const std::vector<std::string>& fixtures() {
  static const std::vector<std::string> names = {
      "egress_ordered_ack.json",
      "egress_inflight_window.json",
      "egress_retry_budget.json",
      "egress_generation_fence.json",
  };
  return names;
}

const Json& member(const Json& object, const std::string& key, const std::string& where) {
  const Json* found = object.find(key);
  REQUIRE(found != nullptr, where + ": missing JSON member `" + key + "`");
  return *found;
}

EgressPolicy policy_of(const Json& value, const std::string& where) {
  EgressPolicy policy;
  policy.inflight_limit =
      static_cast<std::size_t>(json_u64(member(value, "inflight_limit", where)));
  policy.retry_budget = static_cast<std::uint32_t>(json_u64(member(value, "retry_budget", where)));
  policy.retry_base = json_u64(member(value, "retry_base", where));
  policy.retry_ceiling = json_u64(member(value, "retry_ceiling", where));
  return policy;
}

// ── Library outcome -> the fixture's own `returns` vocabulary ───────────────
//
// The runner never decides an outcome; it only RENDERS what the library
// returned into the corpus's spelling, then the comparison is exact over the
// whole object (key set and values), so a field the corpus grows fails.

struct Rendered {
  std::map<std::string, std::string> scalars; // key -> canonical text
  std::map<std::string, std::map<std::string, std::string>> objects;
};

std::string u64_text(std::uint64_t value) { return std::to_string(value); }

std::map<std::string, std::string> envelope_fields(const Envelope& envelope) {
  return {{"generation", u64_text(envelope.generation)},
          {"sequence", u64_text(envelope.sequence)},
          {"attempt", u64_text(envelope.attempt)},
          {"payload", u64_text(envelope.payload)}};
}

std::map<std::string, std::string> retry_fields(const EgressRetry& retry) {
  return {{"sequence", u64_text(retry.sequence)},
          {"attempt", u64_text(retry.attempt)},
          {"backoff", u64_text(retry.backoff)},
          {"exhausted", retry.exhausted ? "true" : "false"}};
}

std::string scalar_text(const Json& value, const std::string& where) {
  switch (value.type) {
  case Json::Type::Null:
    return "null";
  case Json::Type::Bool:
    return json_bool(value) ? "true" : "false";
  case Json::Type::Number:
    return u64_text(json_u64(value));
  case Json::Type::String:
    return "\"" + json_string(value) + "\"";
  default:
    REQUIRE(false, where + ": expected a scalar, got " + lazily_test::json_debug(value));
  }
  return {};
}

std::map<std::string, std::string> flat_object(const Json& value, const std::string& where) {
  REQUIRE(value.is_object(), where + ": expected an object");
  std::map<std::string, std::string> out;
  for (const auto& entry : value.object)
    out[entry.first] = scalar_text(*entry.second, where + "." + entry.first);
  return out;
}

void require_returns(const Rendered& got, const Json& want, const std::string& where) {
  REQUIRE(want.is_object(), where + ": `returns` must be an object");
  std::set<std::string> want_keys;
  for (const auto& entry : want.object) {
    want_keys.insert(entry.first);
    const std::string at = where + ": returns." + entry.first;
    if (entry.second->is_object()) {
      const auto it = got.objects.find(entry.first);
      REQUIRE(it != got.objects.end(), at + " expected an object the library did not return");
      REQUIRE(it->second == flat_object(*entry.second, at),
              at + " disagreed with " + lazily_test::json_debug(*entry.second));
    } else {
      const auto it = got.scalars.find(entry.first);
      REQUIRE(it != got.scalars.end(), at + " expected a field the library did not return");
      REQUIRE(it->second == scalar_text(*entry.second, at),
              at + " disagreed: library said " + it->second + ", fixture " +
                  lazily_test::json_debug(*entry.second));
    }
  }
  std::set<std::string> got_keys;
  for (const auto& entry : got.scalars)
    got_keys.insert(entry.first);
  for (const auto& entry : got.objects)
    got_keys.insert(entry.first);
  REQUIRE(got_keys == want_keys,
          where + ": the returned outcome's key set differs from the fixture");
}

std::string quoted(const char* text) { return std::string("\"") + text + "\""; }

Rendered render(const EgressClaim<Payload>& claim) {
  Rendered r;
  switch (claim.kind) {
  case EgressClaimKind::Claimed:
    r.scalars["claim"] = quoted("claimed");
    r.objects["envelope"] = envelope_fields(*claim.envelope);
    break;
  case EgressClaimKind::Empty:
    r.scalars["claim"] = quoted("empty");
    break;
  case EgressClaimKind::WindowFull:
    r.scalars["claim"] = quoted("window_full");
    break;
  case EgressClaimKind::StaleGeneration:
    r.scalars["claim"] = quoted("stale_generation");
    r.scalars["current"] = u64_text(claim.current);
    break;
  }
  return r;
}

Rendered render(const EgressAck& ack) {
  Rendered r;
  switch (ack.kind) {
  case EgressAckKind::Advanced:
    r.scalars["ack"] = quoted("advanced");
    r.scalars["through"] = u64_text(*ack.through);
    break;
  case EgressAckKind::Unchanged:
    r.scalars["ack"] = quoted("unchanged");
    r.scalars["through"] = ack.through ? u64_text(*ack.through) : "null";
    break;
  case EgressAckKind::StaleGeneration:
    r.scalars["ack"] = quoted("stale_generation");
    r.scalars["current"] = u64_text(ack.current);
    break;
  }
  return r;
}

Rendered render(const EgressFailure& failure) {
  Rendered r;
  switch (failure.kind) {
  case EgressFailureKind::Retrying:
    r.scalars["failure"] = quoted("retrying");
    r.objects["retry"] = retry_fields(*failure.retry);
    break;
  case EgressFailureKind::Exhausted:
    r.scalars["failure"] = quoted("exhausted");
    r.objects["retry"] = retry_fields(*failure.retry);
    break;
  case EgressFailureKind::UnknownSequence:
    r.scalars["failure"] = quoted("unknown_sequence");
    break;
  case EgressFailureKind::StaleGeneration:
    r.scalars["failure"] = quoted("stale_generation");
    r.scalars["current"] = u64_text(failure.current);
    break;
  }
  return r;
}

Rendered render(const EgressRetryAction& action) {
  Rendered r;
  switch (action.kind) {
  case EgressRetryActionKind::Scheduled:
    r.scalars["retry_action"] = quoted("scheduled");
    r.scalars["sequence"] = u64_text(action.sequence);
    break;
  case EgressRetryActionKind::UnknownSequence:
    r.scalars["retry_action"] = quoted("unknown_sequence");
    break;
  case EgressRetryActionKind::StaleGeneration:
    r.scalars["retry_action"] = quoted("stale_generation");
    r.scalars["current"] = u64_text(action.current);
    break;
  }
  return r;
}

Rendered render(const EgressReconnect& reconnect) {
  Rendered r;
  switch (reconnect.kind) {
  case EgressReconnectKind::Advanced:
    r.scalars["reconnect"] = quoted("advanced");
    r.scalars["generation"] = u64_text(reconnect.generation);
    r.scalars["replayed"] = u64_text(reconnect.replayed);
    break;
  case EgressReconnectKind::Unchanged:
    r.scalars["reconnect"] = quoted("unchanged");
    r.scalars["generation"] = u64_text(reconnect.generation);
    break;
  case EgressReconnectKind::StaleGeneration:
    r.scalars["reconnect"] = quoted("stale_generation");
    r.scalars["current"] = u64_text(reconnect.current);
    break;
  }
  return r;
}

// ── The flavour-neutral model ───────────────────────────────────────────────

template <typename Cell, typename Cx> struct Model {
  Cx ctx;
  Cell cell;

  Model(std::uint64_t generation, const EgressPolicy& policy)
      : ctx(), cell(ctx, generation, policy) {}

  std::uint64_t enqueue(Payload payload) { return cell.enqueue(ctx, payload); }
  EgressClaim<Payload> claim(std::uint64_t generation) { return cell.claim(ctx, generation); }
  EgressAck ack(std::uint64_t generation, std::uint64_t through) {
    return cell.ack(ctx, generation, through);
  }
  EgressFailure fail(std::uint64_t generation, std::uint64_t sequence) {
    return cell.fail(ctx, generation, sequence);
  }
  EgressRetryAction retry_now(std::uint64_t generation, std::uint64_t sequence) {
    return cell.retry_now(ctx, generation, sequence);
  }
  EgressReconnect reconnect(std::uint64_t generation) { return cell.reconnect(ctx, generation); }

  std::uint64_t generation() { return cell.generation(); }
  std::uint64_t next_sequence() { return cell.next_sequence(); }
  // Reactive reads: each also re-warms its reader's cache.
  std::vector<Envelope> pending() { return cell.pending(ctx); }
  std::vector<Envelope> inflight() { return cell.inflight(ctx); }
  std::optional<std::uint64_t> acked_through() { return cell.acked_through(ctx); }
  std::optional<EgressRetry> retry() { return cell.retry(ctx); }

  /// Cache validity of the four reader kinds, in corpus order.
  std::array<bool, 4> validity() {
    return {lazily::egress_detail::is_valid(ctx, cell.pending_handle()),
            lazily::egress_detail::is_valid(ctx, cell.inflight_handle()),
            lazily::egress_detail::is_valid(ctx, cell.acked_through_handle()),
            lazily::egress_detail::is_valid(ctx, cell.retry_handle())};
  }

  void materialize() {
    (void)pending();
    (void)inflight();
    (void)acked_through();
    (void)retry();
  }
};

constexpr std::array<const char*, 4> kKinds = {"pending", "inflight", "acked_through", "retry"};

bool envelopes_match(const std::vector<Envelope>& got, const Json& want, const std::string& where) {
  REQUIRE(want.is_array(), where + ": expected an envelope array");
  if (got.size() != want.array.size()) return false;
  for (std::size_t i = 0; i < got.size(); ++i)
    if (envelope_fields(got[i]) !=
        flat_object(*want.array[i], where + "[" + std::to_string(i) + "]"))
      return false;
  return true;
}

template <typename M>
void assert_state(M& model, lazily_test::AssertionKeys& expected, const std::string& where) {
  expected.assert_key_with("generation",
                           [&](const Json& want) { return model.generation() == json_u64(want); });
  expected.assert_key_with(
      "next_sequence", [&](const Json& want) { return model.next_sequence() == json_u64(want); });
  expected.assert_key_with("pending", [&](const Json& want) {
    return envelopes_match(model.pending(), want, where + ": pending");
  });
  expected.assert_key_with("inflight", [&](const Json& want) {
    return envelopes_match(model.inflight(), want, where + ": inflight");
  });
  expected.assert_key_with("acked_through", [&](const Json& want) {
    return model.acked_through() == json_optional_u64(want);
  });
  const auto retry = model.retry();
  if (expected.value_is_object("retry")) {
    REQUIRE(retry.has_value(), where + ": retry absent");
    expected.with_sub("retry", [&](lazily_test::AssertionKeys& want_retry) {
      want_retry.assert_key_with(
          "sequence", [&](const Json& want) { return retry->sequence == json_u64(want); });
      want_retry.assert_key_with("attempt", [&](const Json& want) {
        return static_cast<std::uint64_t>(retry->attempt) == json_u64(want);
      });
      want_retry.assert_key_with(
          "backoff", [&](const Json& want) { return retry->backoff == json_u64(want); });
      want_retry.assert_key_with(
          "exhausted", [&](const Json& want) { return retry->exhausted == json_bool(want); });
    });
  } else {
    expected.assert_key_with("retry", [&](const Json& want) {
      REQUIRE(want.is_null(), where + ": retry is neither null nor an object");
      return !retry.has_value();
    });
  }
}

/// `true` means the reader went from valid to invalid across the op; `false`
/// means it stayed valid -- so over-invalidation fails as loudly as under-.
void assert_invalidation(lazily_test::AssertionKeys& expected, const std::array<bool, 4>& before,
                         const std::array<bool, 4>& after, const std::string& where) {
  expected.with_sub("invalidates", [&](lazily_test::AssertionKeys& want) {
    for (std::size_t slot = 0; slot < kKinds.size(); ++slot) {
      want.assert_key_with(kKinds[slot], [&](const Json& want_kind) {
        const bool expected_invalidated = json_bool(want_kind);
        const bool invalidated = before[slot] && !after[slot];
        REQUIRE(invalidated == expected_invalidated,
                where + ": " + kKinds[slot] + " invalidation mismatch (was valid=" +
                    std::to_string(before[slot]) + ", now valid=" + std::to_string(after[slot]) +
                    ", expected " + (expected_invalidated ? "true" : "false") + ")");
        return true;
      });
    }
  });
}

JsonPtr load(const std::string& name) {
  const std::string text = lazily_test::spec_fixture_text(kArea, name);
  JsonParser parser(text);
  JsonPtr root = parser.parse();
  REQUIRE(root && root->is_object(), name + ": fixture is not a JSON object");
  return root;
}

/// Replay one fixture. Returns the number of steps EXECUTED.
template <typename M> std::size_t replay(const Json& fixture, const std::string& name) {
  REQUIRE(json_string(member(fixture, "model", name)) == "EgressCore",
          name + ": fixture model is not EgressCore");
  REQUIRE(json_string(member(fixture, "kind", name)) == "Egress", name + ": fixture kind");
  REQUIRE(fixture.find("execution_model") == nullptr,
          name + ": the corpus must not carry an execution-model field -- the flavour axis "
                 "lives in the runner");
  M model(json_u64(member(fixture, "generation", name)),
          policy_of(member(fixture, "policy", name), name));
  const Json& steps = member(fixture, "steps", name);
  REQUIRE(steps.is_array() && !steps.array.empty(),
          name + ": no steps -- a vacuous replay would report green");

  model.materialize();
  std::size_t executed = 0;
  for (std::size_t index = 0; index < steps.array.size(); ++index) {
    const Json& step = *steps.array[index];
    const Json& op = member(step, "op", name);
    const std::string type = json_string(member(op, "type", name));
    const std::string where =
        "egress/" + name + " step " + std::to_string(index) + " (" + type + ")";
    const Json& returns = member(step, "returns", where);
    const std::array<bool, 4> before = model.validity();

    if (type == "enqueue") {
      Rendered r;
      r.scalars["sequence"] = u64_text(model.enqueue(json_u64(member(op, "payload", where))));
      require_returns(r, returns, where);
    } else if (type == "claim") {
      require_returns(render(model.claim(json_u64(member(op, "generation", where)))), returns,
                      where);
    } else if (type == "ack") {
      require_returns(render(model.ack(json_u64(member(op, "generation", where)),
                                       json_u64(member(op, "through", where)))),
                      returns, where);
    } else if (type == "fail") {
      require_returns(render(model.fail(json_u64(member(op, "generation", where)),
                                        json_u64(member(op, "sequence", where)))),
                      returns, where);
    } else if (type == "retry_now") {
      require_returns(render(model.retry_now(json_u64(member(op, "generation", where)),
                                             json_u64(member(op, "sequence", where)))),
                      returns, where);
    } else if (type == "reconnect") {
      require_returns(render(model.reconnect(json_u64(member(op, "generation", where)))), returns,
                      where);
    } else {
      REQUIRE(false, where + ": unknown egress op `" + type + "`");
    }

    // Snapshot BEFORE asserting state: `assert_state` reads every reader, which
    // re-warms the caches and would erase the invalidation being measured.
    const std::array<bool, 4> after = model.validity();
    lazily_test::AssertionKeys expected(where + " expected", member(step, "expected", where));
    assert_state(model, expected, where);
    assert_invalidation(expected, before, after, where);
    expected.finish();
    model.materialize();
    ++executed;
  }
  return executed;
}

std::size_t expected_step_total() {
  std::size_t total = 0;
  for (const auto& name : fixtures()) {
    const JsonPtr fixture = load(name);
    const Json& steps = member(*fixture, "steps", name);
    REQUIRE(steps.is_array(), name + ": no steps array");
    total += steps.array.size();
  }
  return total;
}

template <typename M> std::size_t replay_corpus() {
  std::size_t steps = 0;
  for (const auto& name : fixtures())
    steps += replay<M>(*load(name), name);
  return steps;
}

// ── The enforced ledger ─────────────────────────────────────────────────────

struct LedgerRow {
  const char* flavour;
  const char* marker_type;
  bool shipped;
};

const std::vector<LedgerRow>& ledger() {
  static const std::vector<LedgerRow> rows = {
      {"single-threaded", "class EgressCell", true},
      {"thread-safe", "class ThreadSafeEgressCell", true},
      {"async", "class AsyncEgressCell", true},
  };
  return rows;
}

std::filesystem::path find_include_dir() {
  std::filesystem::path dir = std::filesystem::current_path();
  for (int up = 0; up < 5; ++up) {
    const auto candidate = dir / "include" / "lazily";
    if (std::filesystem::is_directory(candidate)) return candidate;
    if (!dir.has_parent_path()) break;
    dir = dir.parent_path();
  }
  return {};
}

std::string header_sources() {
  std::string out;
  const std::filesystem::path root = find_include_dir();
  if (root.empty() || !std::filesystem::is_directory(root)) return out;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".hpp") continue;
    std::ifstream input(entry.path());
    std::stringstream buffer;
    buffer << input.rdbuf();
    out += buffer.str();
  }
  return out;
}

void every_flavour_is_defined(const std::map<std::string, std::size_t>& replayed) {
  const std::string sources = header_sources();
  REQUIRE(!sources.empty(),
          "read no headers from include/lazily; the ledger check would be vacuous");
  for (const auto& row : ledger()) {
    const bool defined = sources.find(row.marker_type) != std::string::npos;
    REQUIRE(defined == row.shipped, std::string("ledger row `") + row.flavour +
                                        "` claims shipped=" + (row.shipped ? "true" : "false") +
                                        " but `" + row.marker_type +
                                        "` defined=" + (defined ? "true" : "false"));
    const auto it = replayed.find(row.flavour);
    if (row.shipped)
      REQUIRE(it != replayed.end() && it->second > 0,
              std::string("flavour `") + row.flavour + "` is shipped but replayed zero steps");
    else
      REQUIRE(it == replayed.end(), std::string("flavour `") + row.flavour +
                                        "` replayed the corpus but its ledger row says unshipped");
  }
}

/// Pin the probe: reading warms the cache, an op that dirties the reader clears
/// it, and an op that does not leaves it warm.
void the_invalidation_probe_discriminates() {
  Model<EgressCell<Payload>, Context> model(1, EgressPolicy{});
  model.materialize();
  REQUIRE((model.validity() == std::array<bool, 4>{true, true, true, true}),
          "reading every reader warms every cache");
  model.enqueue(1);
  REQUIRE((model.validity() == std::array<bool, 4>{false, true, true, true}),
          "an enqueue must invalidate pending and ONLY pending -- a probe that cannot report "
          "`false` proves nothing");
}

} // namespace

int main() {
  the_invalidation_probe_discriminates();

  const std::size_t total = expected_step_total();
  REQUIRE(total > 0, "the egress corpus carries no steps at all");
  std::map<std::string, std::size_t> replayed;

  replayed["single-threaded"] = replay_corpus<Model<EgressCell<Payload>, Context>>();
  REQUIRE(replayed["single-threaded"] == total,
          "every corpus step must run against the single-threaded flavour");

  replayed["thread-safe"] =
      replay_corpus<Model<ThreadSafeEgressCell<Payload>, ThreadSafeContext>>();
  REQUIRE(replayed["thread-safe"] == total,
          "every corpus step must run against the thread-safe flavour");

  replayed["async"] = replay_corpus<Model<AsyncEgressCell<Payload>, AsyncContext>>();
  REQUIRE(replayed["async"] == total, "every corpus step must run against the async flavour");

  every_flavour_is_defined(replayed);

  REQUIRE_FIXTURES_LOADED(4);
  std::cout << "egress family: 3 shipped flavours x " << fixtures().size() << " fixtures, " << total
            << " steps each (" << (total * 3) << " step replays executed)" << std::endl;
  return 0;
}
