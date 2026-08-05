// The transport-agnostic ingress contract (`#designimplementtransport`),
// replayed against EVERY flavour this binding ships -- with a ledger that is
// *enforced* rather than advisory.
//
// lazily-cpp ships all three: `IngressCell` / `ThreadSafeIngressCell` /
// `AsyncIngressCell`, matching the three coverage rows and the contract
// ../lazily-spec/docs/transport-ingress.md declares REQUIRED of every binding x
// every flavour. Mirrors lazily-rs tests/ingress_family_conformance.rs.
//
// The flavour axis lives in the RUNNER, not the corpus: the fixtures carry a
// `model` field naming the primitive and no execution-model field, and one
// flavour-neutral model template replays the same JSON against each shell.
// Nothing in the model is async-coloured, which is the finding rather than an
// oversight -- an admission decision is a function of the fence, the watermark,
// the reorder buffer, and the observed clock, so there is nothing to await and no
// settle step anywhere below.
//
// Three things keep this suite from reporting green while testing nothing -- each
// one a failure mode this family of suites has actually shipped:
//
//   * `every_flavour_is_defined` greps include/lazily for each flavour's type
//     definition, in BOTH directions. A ledger row marked shipped whose type does
//     not exist fails; a type that exists while its row says unshipped fails and
//     names the runner to extend. The ledger cannot rot, because the filesystem
//     enforces it.
//   * Every replay returns its step count, and every flavour asserts that count
//     is non-zero and equal to the corpus total. An absence guard proves the
//     fixtures exist on disk; only a positive count proves this binary opened
//     them.
//   * `invalidates` is asserted in BOTH directions through a cache-validity probe
//     per reader kind. A step expecting `false` fails if the shell invalidated
//     anyway, so over-invalidation is as visible as under-. `the_invalidation_
//     probe_discriminates` pins the probe itself, because a probe that can only
//     say "valid" would pass every negative expectation for free.
//
// `invalidates` is asserted PER CHANNEL, never by receipt count: a stale cache
// recomputes to the right count, so a count-only gate reports green.

#include <lazily/ingress.hpp>
#include <lazily/merge.hpp>

#include <algorithm>
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
using lazily::AsyncIngressCell;
using lazily::Context;
using lazily::IngressAdmission;
using lazily::IngressAuthority;
using lazily::IngressCell;
using lazily::IngressDropReason;
using lazily::IngressEnvelope;
using lazily::IngressError;
using lazily::IngressLifecycle;
using lazily::IngressPolicy;
using lazily::IngressReadiness;
using lazily::IngressRetry;
using lazily::IngressSchedule;
using lazily::IngressScopeView;
using lazily::IngressTransportKind;
using lazily::KeepLatest;
using lazily::Overflow;
using lazily::ReplayRequest;
using lazily::Sum;
using lazily::ThreadSafeContext;
using lazily::ThreadSafeIngressCell;

namespace {

constexpr const char* kArea = "ingress";

using Key = std::string;
using Payload = std::uint64_t;
using Env = IngressEnvelope<Key, Payload>;

// Every fixture the ingress corpus ships. Named explicitly rather than globbed: a
// fixture added to the corpus and not to this list is a MISSING REPLAY, and
// scripts/check-conformance-coverage.sh is what should notice, not a silently
// shorter run.
const std::vector<std::string>& fixtures() {
  static const std::vector<std::string> names = {
      "ingress_ordered_delivery.json",
      "ingress_reorder_and_duplication.json",
      "ingress_reorder_window_overflow.json",
      "ingress_disconnect_replay.json",
      "ingress_backpressure.json",
      "ingress_generation_handoff.json",
      "ingress_freshness_and_retry.json",
  };
  return names;
}

// ── Fixture decoding ────────────────────────────────────────────────────────

const Json& member(const Json& object, const std::string& key, const std::string& where) {
  const Json* found = object.find(key);
  REQUIRE(found != nullptr, where + ": missing JSON member `" + key + "`");
  return *found;
}

Overflow overflow_of(const std::string& text) {
  if (text == "block") return Overflow::Block;
  if (text == "drop_newest") return Overflow::DropNewest;
  if (text == "drop_oldest") return Overflow::DropOldest;
  if (text == "conflate") return Overflow::Conflate;
  if (text == "spill") return Overflow::Spill;
  REQUIRE(false, "unknown overflow `" + text + "`");
  return Overflow::Conflate;
}

IngressTransportKind transport_of(const std::string& text) {
  if (text == "event_channel") return IngressTransportKind::EventChannel;
  if (text == "rpc_triggered") return IngressTransportKind::RpcTriggered;
  if (text == "bounded_polling") return IngressTransportKind::BoundedPolling;
  REQUIRE(false, "unknown transport `" + text + "`");
  return IngressTransportKind::EventChannel;
}

IngressError error_of(const std::string& text) {
  if (text == "transport_closed") return IngressError::TransportClosed;
  if (text == "decode_failed") return IngressError::DecodeFailed;
  if (text == "authority_lost") return IngressError::AuthorityLost;
  REQUIRE(false, "unknown error `" + text + "`");
  return IngressError::TransportClosed;
}

IngressDropReason drop_reason_of(const std::string& text) {
  if (text == "stale_generation") return IngressDropReason::StaleGeneration;
  if (text == "duplicate_sequence") return IngressDropReason::DuplicateSequence;
  if (text == "duplicate_buffered") return IngressDropReason::DuplicateBuffered;
  if (text == "reorder_window_overflow") return IngressDropReason::ReorderWindowOverflow;
  if (text == "expired") return IngressDropReason::Expired;
  if (text == "backpressure") return IngressDropReason::Backpressure;
  if (text == "scope_closed") return IngressDropReason::ScopeClosed;
  REQUIRE(false, "unknown drop reason `" + text + "`");
  return IngressDropReason::StaleGeneration;
}

IngressLifecycle lifecycle_of(const std::string& text) {
  if (text == "opening") return IngressLifecycle::Opening;
  if (text == "live") return IngressLifecycle::Live;
  if (text == "suspended") return IngressLifecycle::Suspended;
  if (text == "closed") return IngressLifecycle::Closed;
  REQUIRE(false, "unknown lifecycle `" + text + "`");
  return IngressLifecycle::Opening;
}

IngressReadiness readiness_of(const std::string& text) {
  if (text == "unknown") return IngressReadiness::Unknown;
  if (text == "warming") return IngressReadiness::Warming;
  if (text == "ready") return IngressReadiness::Ready;
  if (text == "stale") return IngressReadiness::Stale;
  if (text == "suspended") return IngressReadiness::Suspended;
  if (text == "closed") return IngressReadiness::Closed;
  REQUIRE(false, "unknown readiness `" + text + "`");
  return IngressReadiness::Unknown;
}

IngressPolicy policy_of(const Json& value, const std::string& where) {
  IngressPolicy policy;
  policy.reorder_window =
      static_cast<std::size_t>(json_u64(member(value, "reorder_window", where)));
  policy.freshness_horizon = json_u64(member(value, "freshness_horizon", where));
  policy.high_water = json_u64(member(value, "high_water", where));
  policy.overflow = overflow_of(json_string(member(value, "overflow", where)));
  policy.receipt_capacity =
      static_cast<std::size_t>(json_u64(member(value, "receipt_capacity", where)));
  policy.retry_base = json_u64(member(value, "retry_base", where));
  policy.retry_ceiling = json_u64(member(value, "retry_ceiling", where));
  return policy;
}

IngressAdmission expected_admission(const Json& value, const std::string& where) {
  const std::string kind = json_string(member(value, "admission", where));
  if (kind == "accepted")
    return IngressAdmission::accepted(json_u64(member(value, "delivered_through", where)));
  if (kind == "conflated")
    return IngressAdmission::conflated(json_u64(member(value, "delivered_through", where)));
  if (kind == "buffered")
    return IngressAdmission::buffered(json_u64(member(value, "gap_from", where)));
  if (kind == "generation_handoff")
    return IngressAdmission::generation_handoff(json_u64(member(value, "from", where)),
                                                json_u64(member(value, "to", where)));
  if (kind == "dropped")
    return IngressAdmission::dropped(drop_reason_of(json_string(member(value, "reason", where))));
  if (kind == "blocked") return IngressAdmission::blocked();
  REQUIRE(false, where + ": unknown admission `" + kind + "`");
  return IngressAdmission::blocked();
}

std::optional<ReplayRequest> expected_replay(const Json& value, const std::string& where) {
  if (value.is_null()) return std::nullopt;
  ReplayRequest request;
  request.generation = json_u64(member(value, "generation", where));
  request.from_sequence = json_u64(member(value, "from_sequence", where));
  return request;
}

// ── The flavour-neutral model ───────────────────────────────────────────────
//
// The reader-kind probes (`*_is_valid`) are the whole reason this is a template
// rather than three copies of the runner: `invalidates` is a claim about the
// GRAPH, and only the shell can answer it.

template <typename Cell, typename Cx> struct Model {
  Cx ctx;
  Cell cell;

  Model(const IngressPolicy& policy, IngressTransportKind transport, std::uint64_t poll_interval)
      : ctx(), cell(ctx, policy, transport, poll_interval) {}

  void open(const Key& key, std::uint64_t generation) { cell.open(ctx, key, generation); }
  IngressAdmission admit(Env envelope) { return cell.admit(ctx, std::move(envelope)); }
  std::optional<ReplayRequest> suspend(const Key& key) { return cell.suspend(ctx, key); }
  ReplayRequest reconnect(const Key& key, std::uint64_t generation) {
    return cell.reconnect(ctx, key, generation);
  }
  void close(const Key& key) { cell.close(ctx, key); }
  void fail(const Key& key, IngressError error) { cell.fail(ctx, key, error); }
  void tick(std::uint64_t now) { cell.tick(ctx, now); }
  std::optional<Payload> drain(const Key& key) { return cell.drain(ctx, key); }

  // Reactive reads. Each also materializes its reader's cache, which is what
  // makes the next step's validity probe meaningful.
  std::optional<Payload> value(const Key& key) { return cell.value(ctx, key); }
  IngressReadiness readiness(const Key& key) { return cell.readiness(ctx, key); }
  std::optional<IngressAuthority> authority(const Key& key) { return cell.authority(ctx, key); }
  std::optional<IngressRetry> retry(const Key& key) { return cell.retry(ctx, key); }
  std::size_t accepted_len() { return cell.accepted(ctx).size(); }
  std::size_t dropped_len() { return cell.dropped(ctx).size(); }
  std::size_t errors_len() { return cell.errors(ctx).size(); }
  IngressSchedule schedule() { return cell.schedule(ctx); }

  // `false` when the reader is invalidated -- which is what the fixture's
  // `invalidates: true` means.
  bool value_is_valid(const Key& key) {
    return lazily::ingress_detail::is_valid(ctx, cell.value_handle(ctx, key));
  }
  bool readiness_is_valid(const Key& key) {
    return lazily::ingress_detail::is_valid(ctx, cell.readiness_handle(ctx, key));
  }
  bool authority_is_valid(const Key& key) {
    return lazily::ingress_detail::is_valid(ctx, cell.authority_handle(ctx, key));
  }
  bool retry_is_valid(const Key& key) {
    return lazily::ingress_detail::is_valid(ctx, cell.retry_handle(ctx, key));
  }
  bool accepted_is_valid() { return lazily::ingress_detail::is_valid(ctx, cell.accepted_handle()); }
  bool dropped_is_valid() { return lazily::ingress_detail::is_valid(ctx, cell.dropped_handle()); }
  bool errors_is_valid() { return lazily::ingress_detail::is_valid(ctx, cell.errors_handle()); }

  std::optional<IngressScopeView> view(const Key& key) { return cell.view(key); }
};

/// Cache-validity snapshot of every reader kind the fixture can speak about.
struct ValiditySnapshot {
  std::map<Key, std::array<bool, 4>> scopes;
  std::array<bool, 3> receipts{};
};

template <typename M> ValiditySnapshot snapshot_validity(M& model, const std::vector<Key>& keys) {
  ValiditySnapshot snapshot;
  for (const auto& key : keys)
    snapshot.scopes[key] = {model.value_is_valid(key), model.readiness_is_valid(key),
                            model.authority_is_valid(key), model.retry_is_valid(key)};
  snapshot.receipts = {model.accepted_is_valid(), model.dropped_is_valid(),
                       model.errors_is_valid()};
  return snapshot;
}

/// Read every reader kind, so the caches are warm and the next step's validity
/// probe measures THAT step's invalidation and nothing else.
template <typename M> void materialize(M& model, const std::vector<Key>& keys) {
  for (const auto& key : keys) {
    (void)model.value(key);
    (void)model.readiness(key);
    (void)model.authority(key);
    (void)model.retry(key);
  }
  (void)model.accepted_len();
  (void)model.dropped_len();
  (void)model.errors_len();
  (void)model.schedule();
}

template <typename M>
void assert_state(M& model, lazily_test::AssertionKeys& expected, const std::string& where) {
  // Descended into rather than walked by hand (`#lzsubblockkeyset`). Every
  // level of this block is an object, so every level gets its own child
  // tracker: a scope the fixture grows, a per-scope projection it grows, or an
  // authority/retry field it grows all fail by name instead of being compared
  // by nothing.
  expected.with_sub("scopes", [&](lazily_test::AssertionKeys& scopes) {
    for (const auto& name : scopes.keys()) {
      const Key key = name;
      const auto view = model.view(key);
      REQUIRE(view.has_value(), where + ": scope " + key + " absent");
      scopes.with_sub(name, [&](lazily_test::AssertionKeys& scope) {
        scope.assert_key_with("lifecycle", [&](const Json& want) {
          return view->lifecycle == lifecycle_of(json_string(want));
        });
        scope.assert_key_with("generation",
                              [&](const Json& want) { return view->generation == json_u64(want); });
        scope.assert_key_with("delivered_through", [&](const Json& want) {
          return view->delivered_through == json_optional_u64(want);
        });
        scope.assert_key_with("buffered", [&](const Json& want) {
          return view->buffered == static_cast<std::size_t>(json_u64(want));
        });
        scope.assert_key_with("consecutive_errors", [&](const Json& want) {
          return static_cast<std::uint64_t>(view->consecutive_errors) == json_u64(want);
        });
        scope.assert_key_with("window", [&](const Json& want) {
          return model.value(key) == json_optional_u64(want);
        });
        scope.assert_key_with("readiness", [&](const Json& want) {
          return model.readiness(key) == readiness_of(json_string(want));
        });

        const auto authority = model.authority(key);
        if (scope.value_is_object("authority")) {
          REQUIRE(authority.has_value(), where + ": " + key + " authority absent");
          scope.with_sub("authority", [&](lazily_test::AssertionKeys& want_authority) {
            want_authority.assert_key_with("generation", [&](const Json& want) {
              return authority->generation == json_u64(want);
            });
            want_authority.assert_key_with("delivered_through", [&](const Json& want) {
              return authority->delivered_through == json_optional_u64(want);
            });
            want_authority.assert_key_with("stamped_at", [&](const Json& want) {
              return authority->stamped_at == json_u64(want);
            });
          });
        } else {
          scope.assert_key_with("authority", [&](const Json& want) {
            REQUIRE(want.is_null(),
                    where + ": " + key + " authority is neither null nor an object");
            return !authority.has_value();
          });
        }

        const auto retry = model.retry(key);
        if (scope.value_is_object("retry")) {
          REQUIRE(retry.has_value(), where + ": " + key + " retry absent");
          scope.with_sub("retry", [&](lazily_test::AssertionKeys& want_retry) {
            want_retry.assert_key_with("attempt", [&](const Json& want) {
              return static_cast<std::uint64_t>(retry->attempt) == json_u64(want);
            });
            want_retry.assert_key_with(
                "backoff", [&](const Json& want) { return retry->backoff == json_u64(want); });
            want_retry.assert_key_with("resume_from", [&](const Json& want) {
              return retry->resume_from == json_u64(want);
            });
          });
        } else {
          scope.assert_key_with("retry", [&](const Json& want) {
            REQUIRE(want.is_null(), where + ": " + key + " retry is neither null nor an object");
            return !retry.has_value();
          });
        }
      });
    }
  });

  expected.with_sub("receipts", [&](lazily_test::AssertionKeys& receipts) {
    receipts.assert_key_with("accepted", [&](const Json& want) {
      REQUIRE(static_cast<std::uint64_t>(model.accepted_len()) == json_u64(want),
              where + ": accepted receipts");
      return true;
    });
    receipts.assert_key_with("dropped", [&](const Json& want) {
      REQUIRE(static_cast<std::uint64_t>(model.dropped_len()) == json_u64(want),
              where + ": dropped receipts");
      return true;
    });
    receipts.assert_key_with("error", [&](const Json& want) {
      REQUIRE(static_cast<std::uint64_t>(model.errors_len()) == json_u64(want),
              where + ": error receipts");
      return true;
    });
  });
}

/// Assert `invalidates` in BOTH directions. `true` means the reader's cache went
/// from valid to invalid across the op; `false` means it stayed valid -- so a
/// shell that over-invalidates fails just as loudly as one that under-.
void assert_invalidation(lazily_test::AssertionKeys& expected, const ValiditySnapshot& before,
                         const ValiditySnapshot& after, const std::string& where) {
  expected.with_sub("invalidates", [&](lazily_test::AssertionKeys& want) {
    static const char* kKinds[4] = {"value", "readiness", "authority", "retry"};
    want.with_sub("scopes", [&](lazily_test::AssertionKeys& want_scopes) {
      for (const auto& name : want_scopes.keys()) {
        const Key key = name;
        const auto before_it = before.scopes.find(key);
        const auto after_it = after.scopes.find(key);
        REQUIRE(before_it != before.scopes.end() && after_it != after.scopes.end(),
                where + ": key " + key +
                    " was never probed -- an unprobed reader would pass a `false` "
                    "expectation for free");
        want_scopes.with_sub(name, [&](lazily_test::AssertionKeys& want_scope) {
          for (std::size_t slot = 0; slot < 4; ++slot) {
            want_scope.assert_key_with(kKinds[slot], [&](const Json& want_kind) {
              const bool expected = json_bool(want_kind);
              const bool invalidated = before_it->second[slot] && !after_it->second[slot];
              REQUIRE(invalidated == expected,
                      where + ": " + key + "." + kKinds[slot] +
                          " invalidation mismatch (was valid=" +
                          std::to_string(before_it->second[slot]) +
                          ", now valid=" + std::to_string(after_it->second[slot]) + ", expected " +
                          (expected ? "true" : "false") + ")");
              return true;
            });
          }
        });
      }
    });
    static const char* kChannels[3] = {"accepted", "dropped", "error"};
    want.with_sub("receipts", [&](lazily_test::AssertionKeys& want_receipts) {
      for (std::size_t slot = 0; slot < 3; ++slot) {
        want_receipts.assert_key_with(kChannels[slot], [&](const Json& want_channel) {
          const bool expected = json_bool(want_channel);
          const bool invalidated = before.receipts[slot] && !after.receipts[slot];
          REQUIRE(invalidated == expected, where + ": receipts." + kChannels[slot] +
                                               " invalidation mismatch (expected " +
                                               (expected ? "true" : "false") + ")");
          return true;
        });
      }
    });
  });
}

/// Replay one fixture. Returns the number of steps executed, so a caller can
/// prove this binary actually opened the corpus.
template <typename M> std::size_t replay(const Json& fixture, const std::string& label) {
  const IngressPolicy policy = policy_of(member(fixture, "policy", label), label);
  const IngressTransportKind transport =
      transport_of(json_string(member(fixture, "transport", label)));
  const std::uint64_t poll_interval = json_u64(member(fixture, "poll_interval", label));
  M model(policy, transport, poll_interval);

  const Json& steps = member(fixture, "steps", label);
  REQUIRE(steps.is_array() && !steps.array.empty(),
          label + ": no steps -- a vacuous replay would report green");

  // Every key the fixture ever mentions, so a reader exists (and is probed) from
  // the FIRST step. An absent reader would silently pass a `false` invalidation
  // expectation, because a reader that was never valid cannot go invalid.
  std::vector<Key> keys;
  const auto note = [&keys](const Key& key) {
    if (std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(key);
  };
  for (const auto& step_ptr : steps.array) {
    const Json& step = *step_ptr;
    const Json* op = step.find("op");
    if (op != nullptr) {
      const Json* key = op->find("key");
      if (key != nullptr && key->type == Json::Type::String) note(key->as_str());
    }
    const Json* expected = step.find("expected");
    if (expected != nullptr) {
      const Json* scopes = expected->find("scopes");
      if (scopes != nullptr && scopes->is_object())
        for (const auto& entry : scopes->object)
          note(entry.first);
    }
  }

  materialize(model, keys);
  std::size_t executed = 0;

  for (std::size_t index = 0; index < steps.array.size(); ++index) {
    const Json& step = *steps.array[index];
    const Json& op = member(step, "op", label);
    const std::string type = json_string(member(op, "type", label));
    const std::string where = label + " step " + std::to_string(index) + " (" + type + ")";
    const ValiditySnapshot before = snapshot_validity(model, keys);
    const Json* returns = step.find("returns");

    if (type == "admit") {
      Env envelope(json_string(member(op, "key", where)), json_u64(member(op, "generation", where)),
                   json_u64(member(op, "sequence", where)),
                   json_u64(member(op, "stamped_at", where)),
                   json_u64(member(op, "payload", where)));
      const IngressAdmission admission = model.admit(std::move(envelope));
      if (returns != nullptr)
        REQUIRE(admission == expected_admission(*returns, where), where + ": admission");
    } else if (type == "open") {
      model.open(json_string(member(op, "key", where)), json_u64(member(op, "generation", where)));
    } else if (type == "drain") {
      const auto drained = model.drain(json_string(member(op, "key", where)));
      if (returns != nullptr)
        REQUIRE(drained == json_optional_u64(member(*returns, "drained", where)),
                where + ": drained value");
    } else if (type == "suspend") {
      const auto request = model.suspend(json_string(member(op, "key", where)));
      if (returns != nullptr)
        REQUIRE(request == expected_replay(member(*returns, "replay", where), where),
                where + ": replay request");
    } else if (type == "reconnect") {
      const ReplayRequest request = model.reconnect(json_string(member(op, "key", where)),
                                                    json_u64(member(op, "generation", where)));
      if (returns != nullptr) {
        const auto expected = expected_replay(member(*returns, "replay", where), where);
        REQUIRE(expected.has_value() && *expected == request, where + ": replay request");
      }
    } else if (type == "close") {
      model.close(json_string(member(op, "key", where)));
    } else if (type == "fail") {
      model.fail(json_string(member(op, "key", where)),
                 error_of(json_string(member(op, "error", where))));
    } else if (type == "tick") {
      model.tick(json_u64(member(op, "now", where)));
    } else {
      REQUIRE(false, where + ": unknown op `" + type + "`");
    }

    // Snapshot BEFORE asserting state: `assert_state` reads every reader, which
    // re-warms the caches and would erase the invalidation being measured.
    const ValiditySnapshot after = snapshot_validity(model, keys);
    // One guard over the step's whole `expected` block, shared by both
    // helpers: every key it carries has to be claimed by one of them, so a key
    // the corpus adds cannot be replayed and silently skipped
    // (#lzassertunknownkeys).
    lazily_test::AssertionKeys expected(where + " expected", member(step, "expected", where));
    assert_state(model, expected, where);
    assert_invalidation(expected, before, after, where);
    expected.finish();
    materialize(model, keys);
    ++executed;
  }

  return executed;
}

JsonPtr load(const std::string& name) {
  const std::string text = lazily_test::spec_fixture_text(kArea, name);
  JsonParser parser(text);
  JsonPtr root = parser.parse();
  REQUIRE(root && root->is_object(), name + ": fixture is not a JSON object");
  return root;
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

/// Replay the whole corpus against one flavour, dispatching the merge algebra the
/// fixture names. Returns the total step count.
template <typename SumModel, typename KeepModel> std::size_t replay_corpus() {
  std::size_t steps = 0;
  for (const auto& name : fixtures()) {
    const JsonPtr fixture = load(name);
    REQUIRE(json_string(member(*fixture, "model", name)) == "IngressCell",
            name + ": fixture model is not IngressCell");
    // The corpus carries no execution-model field: the flavour axis is the
    // runner's, which is what lets one corpus pin three shells.
    REQUIRE(fixture->find("execution_model") == nullptr,
            name + ": the corpus must not carry an execution-model field -- the "
                   "flavour axis lives in the runner");
    const std::string merge = json_string(member(*fixture, "merge", name));
    if (merge == "sum")
      steps += replay<SumModel>(*fixture, name);
    else if (merge == "keep_latest")
      steps += replay<KeepModel>(*fixture, name);
    else
      REQUIRE(false, name + ": unknown merge `" + merge + "`");
  }
  return steps;
}

// ── The enforced ledger ─────────────────────────────────────────────────────

struct LedgerRow {
  const char* flavour;
  /// Grepped, not referenced: the definition, not a doc-comment mention.
  const char* marker_type;
  bool shipped;
};

const std::vector<LedgerRow>& ledger() {
  static const std::vector<LedgerRow> rows = {
      {"single-threaded", "class IngressCell", true},
      {"thread-safe", "class ThreadSafeIngressCell", true},
      {"async", "class AsyncIngressCell", true},
  };
  return rows;
}

// Resolve include/lazily by walking up from the working directory. ctest runs
// this binary from build/, so a cwd-relative path finds nothing -- and the
// vacuity guard below turns that into a loud failure rather than a silent pass.
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
    if (!entry.is_regular_file()) continue;
    if (entry.path().extension() != ".hpp") continue;
    std::ifstream input(entry.path());
    std::stringstream buffer;
    buffer << input.rdbuf();
    out += buffer.str();
  }
  return out;
}

/// The ledger cannot rot: the filesystem enforces it, in BOTH directions.
void every_flavour_is_defined(const std::map<std::string, std::size_t>& replayed) {
  const std::string sources = header_sources();
  REQUIRE(!sources.empty(), "read no headers from include/lazily; the ledger check would be "
                            "vacuous");
  for (const auto& row : ledger()) {
    const bool defined = sources.find(row.marker_type) != std::string::npos;
    REQUIRE(defined == row.shipped, std::string("ledger row `") + row.flavour +
                                        "` claims shipped=" + (row.shipped ? "true" : "false") +
                                        " but `" + row.marker_type +
                                        "` defined=" + (defined ? "true" : "false") +
                                        "; fix the ledger or extend test_ingress_conformance.cpp");
    const auto it = replayed.find(row.flavour);
    if (row.shipped) {
      REQUIRE(it != replayed.end() && it->second > 0,
              std::string("flavour `") + row.flavour +
                  "` is shipped but replayed zero corpus steps; extend "
                  "test_ingress_conformance.cpp");
    } else {
      REQUIRE(it == replayed.end(), std::string("flavour `") + row.flavour +
                                        "` replayed the corpus but its ledger row says unshipped");
    }
  }
}

void ledger_is_not_all_skips() {
  REQUIRE(ledger().size() == 3, "one row per flavour this family defines");
  bool any = false;
  for (const auto& row : ledger())
    any = any || row.shipped;
  REQUIRE(any, "a ledger of nothing-shipped is not coverage");
}

void corpus_is_present_and_non_trivial() {
  const std::size_t total = expected_step_total();
  REQUIRE(total >= 30, "the ingress corpus replays only " + std::to_string(total) +
                           " steps; that is not the named schedule set");
}

/// The corpus asserts NEGATIVE invalidation, so the probe itself must be able to
/// fail. This pins the probe: reading warms the cache, an op that dirties the
/// reader clears it, and one that does not leaves it warm. Without this, a probe
/// hard-wired to `true` would pass every `invalidates: false` step for free.
void the_invalidation_probe_discriminates() {
  using SyncModel = Model<IngressCell<Key, Payload, Sum>, Context>;
  SyncModel model(IngressPolicy{}, IngressTransportKind::EventChannel, 25);
  const Key key = "alpha";
  (void)model.value(key);
  REQUIRE(model.value_is_valid(key), "reading warms the cache");

  model.admit(Env(key, 1, 0, 0, 1));
  REQUIRE(!model.value_is_valid(key),
          "a delivery must invalidate the value reader -- a probe that cannot "
          "report `false` proves nothing");

  (void)model.value(key);
  model.admit(Env(key, 1, 5, 0, 1));
  REQUIRE(model.value_is_valid(key), "a buffered envelope must NOT invalidate the value reader");
}

struct BoundaryDelivery {
  std::string id;
  std::set<std::string> targets;
  std::set<std::string> acked;
};

struct BoundaryModel {
  std::size_t max_buffered;
  std::uint64_t freshness_horizon;
  std::string phase = "detached";
  std::uint64_t generation = 0;
  std::optional<std::uint64_t> cursor;
  std::map<std::uint64_t, const Json*> buffered;
  std::set<std::string> source_keys;
  std::set<std::string> members;
  std::string validation = "valid";
  std::optional<std::uint64_t> replay_from;
  std::uint64_t stale_events = 0;
  std::optional<BoundaryDelivery> delivery;
  std::optional<std::uint64_t> last_stamped_at;
  std::uint64_t now = 0;
  std::uint64_t revision = 0;

  void changed() { ++revision; }

  void apply_payload(const Json& op) {
    const std::string action = json_string(member(op, "action", "boundary event"));
    if (action == "upsert") {
      source_keys.insert(json_string(member(op, "key", "boundary event")));
    } else if (action == "remove") {
      source_keys.erase(json_string(member(op, "key", "boundary event")));
    } else if (action == "validate") {
      validation = json_string(member(op, "validation", "boundary event"));
    } else {
      REQUIRE(false, "unknown boundary event action");
    }
    cursor = json_u64(member(op, "cursor", "boundary event"));
    last_stamped_at = json_u64(member(op, "stamped_at", "boundary event"));
    phase = validation == "valid" ? "live" : "invalid";
    replay_from.reset();
  }

  void drain() {
    while (cursor) {
      const auto it = buffered.find(*cursor + 1);
      if (it == buffered.end()) break;
      const Json* event = it->second;
      buffered.erase(it);
      apply_payload(*event);
    }
    if (!buffered.empty()) {
      phase = "replay_required";
      replay_from = *cursor + 1;
    }
  }

  void apply(const Json& op) {
    const std::string type = json_string(member(op, "type", "boundary op"));
    if (type == "subscribe") {
      const auto next = json_u64(member(op, "generation", type));
      if (next < generation) return;
      generation = next;
      cursor.reset();
      buffered.clear();
      source_keys.clear();
      members.clear();
      validation = "valid";
      replay_from.reset();
      phase = "bootstrapping";
      changed();
      return;
    }
    if (type == "snapshot") {
      const auto next = json_u64(member(op, "generation", type));
      if (next < generation) {
        ++stale_events;
        changed();
        return;
      }
      if (next > generation) {
        generation = next;
        buffered.clear();
      }
      cursor = json_u64(member(op, "cursor", type));
      last_stamped_at = json_u64(member(op, "stamped_at", type));
      source_keys.clear();
      for (const auto& value : member(op, "source_keys", type).array)
        source_keys.insert(json_string(*value));
      members.clear();
      for (const auto& value : member(op, "members", type).array)
        members.insert(json_string(*value));
      validation = json_string(member(op, "validation", type));
      phase = validation == "valid" ? "live" : "invalid";
      replay_from.reset();
      for (auto it = buffered.begin(); it != buffered.end();) {
        if (it->first <= *cursor)
          it = buffered.erase(it);
        else
          ++it;
      }
      drain();
      changed();
      return;
    }
    if (type == "event") {
      const auto next = json_u64(member(op, "generation", type));
      const auto event_cursor = json_u64(member(op, "cursor", type));
      if (next < generation) {
        ++stale_events;
        changed();
        return;
      }
      if (next > generation) {
        generation = next;
        cursor.reset();
        buffered.clear();
        source_keys.clear();
        members.clear();
        phase = "bootstrapping";
        replay_from.reset();
      }
      if (!cursor) {
        if (buffered.size() >= max_buffered && buffered.count(event_cursor) == 0) {
          phase = "backpressured";
          replay_from = 0;
          changed();
          return;
        }
        if (buffered.emplace(event_cursor, &op).second) changed();
        return;
      }
      if (event_cursor <= *cursor || buffered.count(event_cursor) != 0) return;
      if (event_cursor == *cursor + 1) {
        apply_payload(op);
        drain();
        changed();
        return;
      }
      if (buffered.size() >= max_buffered) {
        phase = "backpressured";
        replay_from = *cursor + 1;
        changed();
        return;
      }
      buffered.emplace(event_cursor, &op);
      phase = "replay_required";
      replay_from = *cursor + 1;
      changed();
      return;
    }
    if (type == "member_join") {
      const std::string member_name = json_string(member(op, "member", type));
      if (!members.insert(member_name).second) return;
      if (delivery && delivery->targets.empty()) delivery->targets.insert(member_name);
      changed();
      return;
    }
    if (type == "member_leave") {
      if (members.erase(json_string(member(op, "member", type))) != 0) changed();
      return;
    }
    if (type == "open_receipt") {
      delivery = BoundaryDelivery{json_string(member(op, "receipt_id", type)), members, {}};
      changed();
      return;
    }
    if (type == "ack") {
      if (!delivery || delivery->id != json_string(member(op, "receipt_id", type))) return;
      const std::string member_name = json_string(member(op, "member", type));
      if (delivery->targets.count(member_name) && delivery->acked.insert(member_name).second)
        changed();
      return;
    }
    if (type == "tick") {
      const bool before = fresh();
      now = json_u64(member(op, "now", type));
      if (fresh() != before) changed();
      return;
    }
    REQUIRE(false, "unknown boundary ingress op `" + type + "`");
  }

  bool fresh() const {
    return last_stamped_at && now - std::min(now, *last_stamped_at) <= freshness_horizon;
  }
};

std::vector<std::string> json_strings(const Json& value) {
  std::vector<std::string> out;
  for (const auto& item : value.array)
    out.push_back(json_string(*item));
  return out;
}

std::vector<std::uint64_t> json_u64s(const Json& value) {
  std::vector<std::uint64_t> out;
  for (const auto& item : value.array)
    out.push_back(json_u64(*item));
  return out;
}

void assert_boundary_projection(BoundaryModel& model, lazily_test::AssertionKeys& expected) {
  expected.assert_key_if_present("phase", model.phase);
  expected.assert_key_if_present("generation", model.generation);
  expected.assert_key_if_present("cursor", model.cursor, json_optional_u64);
  std::vector<std::uint64_t> cursors;
  for (const auto& entry : model.buffered)
    cursors.push_back(entry.first);
  expected.assert_key_if_present("buffered_cursors", cursors, json_u64s);
  expected.assert_key_if_present(
      "source_keys", std::vector<std::string>(model.source_keys.begin(), model.source_keys.end()),
      json_strings);
  expected.assert_key_if_present(
      "members", std::vector<std::string>(model.members.begin(), model.members.end()),
      json_strings);
  expected.assert_key_if_present("validation", model.validation);
  expected.assert_key_if_present("replay_from", model.replay_from, json_optional_u64);
  expected.assert_key_if_present("stale_events", model.stale_events);
  expected.assert_key_if_present("ready", model.phase == "live" && model.validation == "valid");
  expected.assert_key_if_present("fresh", model.fresh());
  expected.assert_key_if_present("observation_revision", model.revision);
  expected.assert_key_if_present("revision", model.revision);
  if (expected.value_is_object("delivery")) {
    expected.with_sub("delivery", [&](lazily_test::AssertionKeys& delivery) {
      REQUIRE(model.delivery.has_value(), "expected an active delivery receipt");
      delivery.assert_key_if_present("receipt_id", model.delivery->id);
      delivery.assert_key_if_present(
          "targets",
          std::vector<std::string>(model.delivery->targets.begin(), model.delivery->targets.end()),
          json_strings);
      delivery.assert_key_if_present(
          "acked",
          std::vector<std::string>(model.delivery->acked.begin(), model.delivery->acked.end()),
          json_strings);
      delivery.assert_key_if_present("converged", !model.delivery->targets.empty() &&
                                                      std::includes(model.delivery->acked.begin(),
                                                                    model.delivery->acked.end(),
                                                                    model.delivery->targets.begin(),
                                                                    model.delivery->targets.end()));
    });
  } else {
    expected.assert_key_if_present("delivery", !model.delivery.has_value(),
                                   [](const Json& value) { return value.is_null(); });
  }
}

void replay_boundary_ingress_contract() {
  const std::string name = "boundary_ingress_adapter.json";
  const JsonPtr fixture = load(name);
  const Json& base_policy = member(*fixture, "policy", name);
  const auto& scenarios = member(*fixture, "scenarios", name).array;
  std::size_t replayed = 0;
  for (const auto& view : lazily_test::scenario_views("ingress/" + name, scenarios)) {
    const Json& scenario = view.replay();
    const Json* scenario_policy = scenario.find("policy");
    const std::size_t max_buffered = static_cast<std::size_t>(
        json_u64(member(scenario_policy ? *scenario_policy : base_policy, "max_buffered", name)));
    BoundaryModel model{max_buffered, json_u64(member(base_policy, "freshness_horizon", name))};
    std::size_t index = 0;
    for (const auto& step : member(scenario, "steps", name).array) {
      model.apply(member(*step, "op", name));
      const std::string where = name + " step " + std::to_string(index) + " expected";
      lazily_test::AssertionKeys expected(where, member(*step, "expected", where));
      assert_boundary_projection(model, expected);
      expected.finish();
      ++index;
      ++replayed;
    }
  }
  REQUIRE(replayed > 0, "canonical boundary-ingress fixture replayed zero steps");
}

} // namespace

int main() {
  corpus_is_present_and_non_trivial();
  ledger_is_not_all_skips();
  the_invalidation_probe_discriminates();
  replay_boundary_ingress_contract();

  const std::size_t total = expected_step_total();
  std::map<std::string, std::size_t> replayed;

  replayed["single-threaded"] =
      replay_corpus<Model<IngressCell<Key, Payload, Sum>, Context>,
                    Model<IngressCell<Key, Payload, KeepLatest>, Context>>();
  REQUIRE(replayed["single-threaded"] == total && replayed["single-threaded"] > 0,
          "every corpus step must run against the single-threaded flavour");

  replayed["thread-safe"] =
      replay_corpus<Model<ThreadSafeIngressCell<Key, Payload, Sum>, ThreadSafeContext>,
                    Model<ThreadSafeIngressCell<Key, Payload, KeepLatest>, ThreadSafeContext>>();
  REQUIRE(replayed["thread-safe"] == total && replayed["thread-safe"] > 0,
          "every corpus step must run against the thread-safe flavour");

  replayed["async"] =
      replay_corpus<Model<AsyncIngressCell<Key, Payload, Sum>, AsyncContext>,
                    Model<AsyncIngressCell<Key, Payload, KeepLatest>, AsyncContext>>();
  REQUIRE(replayed["async"] == total && replayed["async"] > 0,
          "every corpus step must run against the async flavour");

  every_flavour_is_defined(replayed);

  REQUIRE_FIXTURES_LOADED(8);
  std::cout << "ingress family: 3 shipped flavours x " << fixtures().size() << " fixtures, "
            << total << " steps each (" << (total * 3) << " step replays)" << std::endl;
  return 0;
}
