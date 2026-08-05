// Command/RPC message-plane conformance (`command-plane-v1`). A C++17 port of
// lazily-rs/tests/command_conformance.rs: replay the
// lazily-spec/conformance/message-passing/*.json fixtures through the
// `CommandProjection` reducer + RPC facade. Each fixture folds `frames` (each
// decodes into a `CommandMessage` or a `CausalReceipts` batch) and `expect`
// pins the reducer image, terminal-conflict fail-closed behaviour, stale-
// generation rejection, and the RPC terminal-only resolution rule.
//
// cpp `observe_receipt` returns void (unlike lazily-rs, which returns an apply
// status), so receipt-frame outcomes are asserted through their OBSERVABLE
// consequences — `terminal_for` / `has_conflict` / the final projection image —
// rather than a per-op status. Message-frame statuses are asserted directly.
//
// Fixture bytes are read from the sibling lazily-spec checkout;
// REQUIRE_FIXTURES_LOADED(8) proves the corpus actually ran.

#include <lazily/command.hpp>

#include <algorithm>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "test_assertion_keys.hpp"
#include "test_json.hpp"
#include "test_spec_fixture.hpp"

using namespace lazily;
using lazily_test::Json;

static int test_count = 0;
static int test_passed = 0;

#define TEST(name)                                                                                 \
  static void name();                                                                              \
  struct name##_runner {                                                                           \
    name##_runner() {                                                                              \
      ++test_count;                                                                                \
      name();                                                                                      \
      ++test_passed;                                                                               \
    }                                                                                              \
  } name##_instance;                                                                               \
  static void name()

// ── enum <-> string ────────────────────────────────────────────────────────
static const char* status_str(CommandStatus s) {
  switch (s) {
  case CommandStatus::Submitted:
    return "submitted";
  case CommandStatus::Accepted:
    return "accepted";
  case CommandStatus::Running:
    return "running";
  case CommandStatus::Applied:
    return "applied";
  case CommandStatus::Rejected:
    return "rejected";
  case CommandStatus::Cancelled:
    return "cancelled";
  case CommandStatus::Superseded:
    return "superseded";
  case CommandStatus::TimedOut:
    return "timed_out";
  }
  return "?";
}
static CommandStatus status_of(const std::string& s) {
  if (s == "submitted") return CommandStatus::Submitted;
  if (s == "accepted") return CommandStatus::Accepted;
  if (s == "running") return CommandStatus::Running;
  if (s == "applied") return CommandStatus::Applied;
  if (s == "rejected") return CommandStatus::Rejected;
  if (s == "cancelled") return CommandStatus::Cancelled;
  if (s == "superseded") return CommandStatus::Superseded;
  if (s == "timed_out") return CommandStatus::TimedOut;
  REQUIRE(false, "unknown command status");
  return CommandStatus::Submitted;
}
static CommandEventKind event_kind_of(const std::string& s) {
  if (s == "observed") return CommandEventKind::Observed;
  if (s == "accepted") return CommandEventKind::Accepted;
  if (s == "started") return CommandEventKind::Started;
  if (s == "progress") return CommandEventKind::Progress;
  if (s == "cancelled") return CommandEventKind::Cancelled;
  if (s == "superseded") return CommandEventKind::Superseded;
  if (s == "timed_out") return CommandEventKind::TimedOut;
  REQUIRE(false, "unknown event kind");
  return CommandEventKind::Observed;
}
static ReceiptOutcome outcome_of(const std::string& s) {
  if (s == "observed") return ReceiptOutcome::Observed;
  if (s == "accepted") return ReceiptOutcome::Accepted;
  if (s == "applied") return ReceiptOutcome::Applied;
  if (s == "rejected") return ReceiptOutcome::Rejected;
  REQUIRE(false, "unknown receipt outcome");
  return ReceiptOutcome::Observed;
}
static DedupePolicy dedupe_of(const std::string& s) {
  if (s == "none") return DedupePolicy::None;
  if (s == "same_idempotency_key") return DedupePolicy::SameIdempotencyKey;
  if (s == "same_command_id") return DedupePolicy::SameCommandId;
  REQUIRE(false, "unknown dedupe policy");
  return DedupePolicy::None;
}

static std::optional<std::string> opt_str(const Json* j, const char* key) {
  const Json* f = j->find(key);
  if (!f || f->is_null()) return std::nullopt;
  return f->str;
}

// ── decoders ───────────────────────────────────────────────────────────────
static CommandSubmit decode_submit(const Json* j) {
  CommandSubmit c;
  c.command_id = j->find("command_id")->str;
  c.causation_id = j->find("causation_id")->str;
  c.source = j->find("source")->str;
  c.target = j->find("target")->str;
  c.ns = j->find("namespace")->str;
  c.name = j->find("name")->str;
  c.authority_generation = j->find("authority_generation")->as_int();
  c.idempotency_key = j->find("idempotency_key")->str;
  c.deadline_ms = j->find("deadline_ms")->as_int();
  const Json* pol = j->find("policy");
  c.policy.dedupe = dedupe_of(pol->find("dedupe")->str);
  c.policy.supersede = pol->find("supersede")->as_bool();
  c.policy.cancel_on_preempt = pol->find("cancel_on_preempt")->as_bool();
  c.payload_type = j->find("payload_type")->str;
  c.payload_hash = j->find("payload_hash")->str;
  c.payload = IpcValueInline{}; // projection ignores payload bytes
  if (const Json* rf = j->find("required_features"))
    for (const auto& f : rf->array)
      c.required_features.push_back(f->str);
  return c;
}
static CommandCancel decode_cancel(const Json* j) {
  CommandCancel c;
  c.command_id = j->find("command_id")->str;
  c.causation_id = j->find("causation_id")->str;
  c.source = j->find("source")->str;
  c.authority_generation = j->find("authority_generation")->as_int();
  c.reason = opt_str(j, "reason");
  return c;
}
// A required numeric field. `find(...)->as_int()` segfaults on a renamed or
// missing key instead of reporting which field went missing, and `generation`
// in particular is the stale discriminator `stale_generation_ignored.json`
// exists to pin — a defaulted or crashed read there is a false negative dressed
// as a crash. Fail with the field name instead.
static long long req_int(const Json* j, const char* key) {
  const Json* node = j->find(key);
  REQUIRE(node != nullptr && node->type == Json::Type::Number,
          (std::string("fixture is missing required numeric field `") + key + "`").c_str());
  return node->as_int();
}

static CommandEvents decode_events(const Json* j) {
  CommandEvents out;
  for (const auto& e : j->find("events")->array) {
    CommandEvent ev;
    ev.event_id = e->find("event_id")->str;
    ev.command_id = e->find("command_id")->str;
    ev.kind = event_kind_of(e->find("kind")->str);
    ev.generation = req_int(e.get(), "generation");
    ev.detail = opt_str(e.get(), "detail");
    out.events.push_back(std::move(ev));
  }
  return out;
}
static CommandProjectionImage decode_projection_image(const Json* j) {
  CommandProjectionImage img;
  img.generation = req_int(j, "generation");
  for (const auto& c : j->find("commands")->array) {
    CommandProjectionEntry e;
    e.command_id = c->find("command_id")->str;
    e.status = status_of(c->find("status")->str);
    e.terminal = c->find("terminal")->as_bool();
    e.generation = req_int(c.get(), "generation");
    e.reason = opt_str(c.get(), "reason");
    e.terminal_receipt_id = opt_str(c.get(), "terminal_receipt_id");
    e.last_event_id = opt_str(c.get(), "last_event_id");
    img.commands.push_back(std::move(e));
  }
  return img;
}
static CausalReceipt decode_receipt(const Json* j) {
  CausalReceipt r;
  r.receipt_id = j->find("receipt_id")->str;
  r.causation_id = j->find("causation_id")->str;
  r.observer = j->find("observer") ? j->find("observer")->str : std::string();
  // `generation` is the stale discriminator `stale_generation_ignored.json`
  // exists to pin. Defaulting an absent/renamed key to 0 made the receipt look
  // stale, so the "ignored frames" expectations passed for the wrong reason.
  r.generation = req_int(j, "generation");
  r.outcome = outcome_of(j->find("outcome")->str);
  r.reason = opt_str(j, "reason");
  r.payload_hash = opt_str(j, "payload_hash");
  return r;
}

static CommandMessage decode_message(const Json* wire) {
  if (const Json* s = wire->find("CommandSubmit")) return CommandMessageSubmit{decode_submit(s)};
  if (const Json* c = wire->find("CommandCancel")) return CommandMessageCancel{decode_cancel(c)};
  if (const Json* e = wire->find("CommandEvents")) return CommandMessageEvents{decode_events(e)};
  if (const Json* p = wire->find("CommandProjection"))
    return CommandMessageProjection{decode_projection_image(p)};
  REQUIRE(false, "unknown message-passing wire tag");
  return CommandMessageSubmit{};
}

// Fold one frame. Returns the message apply-status for `message-passing` frames;
// `std::nullopt` for `receipts` frames (cpp observe_receipt is void).
static std::optional<CommandApplyStatus> fold_frame(CommandProjection& p, const Json* frame) {
  const std::string& schema = frame->find("schema")->str;
  const Json* wire = frame->find("wire");
  if (schema == "message-passing") return p.apply_message(decode_message(wire));
  if (schema == "receipts") {
    const Json* batch = wire->find("CausalReceipts");
    REQUIRE(batch != nullptr, "receipts wire missing CausalReceipts");
    for (const auto& r : batch->find("receipts")->array)
      p.observe_receipt(decode_receipt(r.get()));
    return std::nullopt;
  }
  REQUIRE(false, ("unknown frame schema: " + schema).c_str());
  return std::nullopt;
}

// ── assertions ───────────────────────────────────────────────────────────────
static void assert_image_eq(const CommandProjectionImage& got, const CommandProjectionImage& want,
                            const std::string& msg) {
  REQUIRE(got.generation == want.generation, ("projection generation: " + msg).c_str());
  REQUIRE(got.commands.size() == want.commands.size(),
          ("projection command count: " + msg).c_str());
  std::map<std::string, const CommandProjectionEntry*> by_id;
  for (const auto& e : got.commands)
    by_id[e.command_id] = &e;
  for (const auto& w : want.commands) {
    auto it = by_id.find(w.command_id);
    REQUIRE(it != by_id.end(), ("missing command " + w.command_id + ": " + msg).c_str());
    const CommandProjectionEntry& g = *it->second;
    REQUIRE(g.status == w.status, ("status " + w.command_id + " (" + status_str(g.status) +
                                   " != " + status_str(w.status) + "): " + msg)
                                      .c_str());
    REQUIRE(g.terminal == w.terminal, ("terminal " + w.command_id + ": " + msg).c_str());
    REQUIRE(g.generation == w.generation, ("generation " + w.command_id + ": " + msg).c_str());
    REQUIRE(g.reason == w.reason, ("reason " + w.command_id + ": " + msg).c_str());
    REQUIRE(g.terminal_receipt_id == w.terminal_receipt_id,
            ("terminal_receipt_id " + w.command_id + ": " + msg).c_str());
    REQUIRE(g.last_event_id == w.last_event_id,
            ("last_event_id " + w.command_id + ": " + msg).c_str());
  }
}

static bool image_equal(const CommandProjectionImage& left, const CommandProjectionImage& right) {
  if (left.generation != right.generation || left.commands.size() != right.commands.size())
    return false;
  std::map<std::string, const CommandProjectionEntry*> by_id;
  for (const auto& entry : right.commands)
    by_id[entry.command_id] = &entry;
  for (const auto& entry : left.commands) {
    const auto found = by_id.find(entry.command_id);
    if (found == by_id.end()) return false;
    const auto& other = *found->second;
    if (entry.status != other.status || entry.terminal != other.terminal ||
        entry.generation != other.generation || entry.reason != other.reason ||
        entry.terminal_receipt_id != other.terminal_receipt_id ||
        entry.last_event_id != other.last_event_id)
      return false;
  }
  return true;
}

static void assert_projection(const CommandProjection& p, lazily_test::AssertionKeys& expect,
                              const std::string& key, const std::string& msg) {
  expect.assert_key_set_with(key, {"commands", "generation"}, [&](const Json& want) {
    assert_image_eq(p.to_image(), decode_projection_image(&want), msg);
    return true;
  });
}

static const Json* frames_of(const Json* fx) {
  const Json* f = fx->find("frames");
  REQUIRE(f != nullptr, "fixture missing frames");
  return f;
}

static bool is_stale(const std::optional<CommandApplyStatus>& s) {
  return s.has_value() && std::holds_alternative<CommandStatusStaleGeneration>(*s);
}

// ── fixtures ─────────────────────────────────────────────────────────────────
static void load(const std::string& name, lazily_test::JsonPtr& out) {
  const std::string text = lazily_test::spec_fixture_text("message-passing", name);
  REQUIRE(text.find("command-plane") != std::string::npos ||
              text.find("message-passing") != std::string::npos,
          "fixture is not a message-passing corpus");
  out = lazily_test::parse_json(text);
}

TEST(editor_route_submit_is_nonterminal) {
  lazily_test::JsonPtr fx;
  load("editor_route_submit.json", fx);
  CommandProjection p;
  for (const auto& fr : frames_of(fx.get())->array)
    fold_frame(p, fr.get());
  lazily_test::AssertionKeys expect("message-passing/editor_route_submit.json expect",
                                    *fx->find("expect"));
  assert_projection(p, expect, "projection", "editor_route_submit");
  REQUIRE(!p.terminal_for("cmd-run-1").has_value(), "cmd-run-1 must be non-terminal");
}

TEST(sync_tmux_layout_submit_shared_blob) {
  lazily_test::JsonPtr fx;
  load("sync_tmux_layout_submit.json", fx);
  CommandProjection p;
  for (const auto& fr : frames_of(fx.get())->array)
    fold_frame(p, fr.get());
  lazily_test::AssertionKeys expect("message-passing/sync_tmux_layout_submit.json expect",
                                    *fx->find("expect"));
  assert_projection(p, expect, "projection", "sync_tmux_layout_submit");
}

TEST(accepted_then_applied_receipt_terminal_only_at_receipt) {
  lazily_test::JsonPtr fx;
  load("accepted_then_applied_receipt.json", fx);
  const Json* frames = frames_of(fx.get());
  lazily_test::AssertionKeys expect("message-passing/accepted_then_applied_receipt.json expect",
                                    *fx->find("expect"));
  CommandProjection p;
  std::optional<size_t> terminal_at;
  for (size_t i = 0; i < frames->array.size(); ++i) {
    fold_frame(p, frames->array[i].get());
    const bool is_term = p.terminal_for("cmd-run-1").has_value();
    if (is_term && !terminal_at) terminal_at = i;
  }
  REQUIRE(terminal_at.has_value(), "command never reached a terminal state");
  expect.assert_key("terminal_after_frame_index", *terminal_at,
                    [](const Json& want) { return static_cast<size_t>(want.as_int()); });
  assert_projection(p, expect, "projection", "accepted_then_applied_receipt");
}

TEST(stale_generation_events_and_receipts_ignored) {
  lazily_test::JsonPtr fx;
  load("stale_generation_ignored.json", fx);
  const Json* frames = frames_of(fx.get());
  lazily_test::AssertionKeys expect("message-passing/stale_generation_ignored.json expect",
                                    *fx->find("expect"));
  std::vector<size_t> ignored;
  for (const auto& v : expect.required("ignored_frame_indices").array)
    ignored.push_back(static_cast<size_t>(v->as_int()));
  CommandProjection p;
  for (size_t i = 0; i < frames->array.size(); ++i) {
    const auto before = p.to_image();
    auto status = fold_frame(p, frames->array[i].get());
    const bool is_ignored = std::find(ignored.begin(), ignored.end(), i) != ignored.end();
    // A stale MESSAGE frame reports StaleGeneration directly; a stale RECEIPT
    // frame (cpp observe_receipt is void) is asserted only through the final
    // projection image below.
    if (is_ignored && status.has_value())
      REQUIRE(is_stale(status),
              ("ignored message frame expected StaleGeneration: " + std::to_string(i)).c_str());
    if (is_ignored)
      REQUIRE(image_equal(before, p.to_image()),
              ("ignored frame changed the projection: " + std::to_string(i)).c_str());
  }
  expect.assert_key_with("ignored_frame_indices",
                         [&](const Json& want) { return ignored.size() == want.array.size(); });
  assert_projection(p, expect, "projection", "stale_generation_ignored");
}

TEST(terminal_conflict_fails_closed) {
  lazily_test::JsonPtr fx;
  load("terminal_conflict_fail_closed.json", fx);
  const Json* frames = frames_of(fx.get());
  lazily_test::AssertionKeys expect("message-passing/terminal_conflict_fail_closed.json expect",
                                    *fx->find("expect"));
  const std::string cmd = expect.required("conflict_command_id").str;
  CommandProjection p;
  std::optional<size_t> conflict_at;
  for (size_t i = 0; i < frames->array.size(); ++i) {
    fold_frame(p, frames->array[i].get());
    if (p.has_conflict(cmd) && !conflict_at) conflict_at = i;
  }
  expect.assert_key("conflict", p.has_conflict(cmd));
  expect.assert_key_with("conflict_command_id",
                         [&](const Json& want) { return p.has_conflict(want.str); });
  REQUIRE(conflict_at.has_value(), "terminal conflict must be flagged");
  expect.assert_key("conflict_after_frame_index", *conflict_at,
                    [](const Json& want) { return static_cast<size_t>(want.as_int()); });
  // The applied outcome is preserved (no winner selection).
  assert_projection(p, expect, "projection_before_conflict",
                    "terminal_conflict projection_before_conflict");
}

TEST(cancel_preempts_nonterminal_scenarios) {
  lazily_test::JsonPtr fx;
  load("cancel_preempts_nonterminal.json", fx);
  const Json* scenarios = fx->find("scenarios");
  REQUIRE(scenarios != nullptr, "cancel fixture missing scenarios");
  for (const auto& sv : lazily_test::scenario_views(
           "message-passing/cancel_preempts_nonterminal.json", scenarios->array)) {
    // Rung 4 books on the PAYLOAD handoff (#lzscenariobodyskip), so a body
    // that stops short of replaying stops being booked.
    const Json* scenario = &sv.replay();
    lazily_test::AssertionKeys expect(
        "message-passing/cancel_preempts_nonterminal.json scenarios[" + std::to_string(sv.index()) +
            "].expect",
        *scenario->find("expect"));
    std::vector<size_t> ignored;
    if (const Json* values = expect.find("ignored_frame_indices"))
      for (const auto& value : values->array)
        ignored.push_back(static_cast<size_t>(value->as_int()));
    CommandProjection p;
    const Json* scenario_frames = scenario->find("frames");
    for (size_t i = 0; i < scenario_frames->array.size(); ++i) {
      const auto before = p.to_image();
      fold_frame(p, scenario_frames->array[i].get());
      if (std::find(ignored.begin(), ignored.end(), i) != ignored.end())
        REQUIRE(image_equal(before, p.to_image()), "ignored cancellation frame changed projection");
    }
    expect.assert_key_with_if_present("ignored_frame_indices", [&](const Json& want) {
      return ignored.size() == want.array.size();
    });
    assert_projection(p, expect, "projection",
                      "cancel_preempts[" + scenario->find("name")->str + "]");
  }
}

TEST(reconnect_command_projection_resyncs) {
  lazily_test::JsonPtr fx;
  load("reconnect_command_projection.json", fx);
  CommandProjection p;
  for (const auto& fr : frames_of(fx.get())->array)
    fold_frame(p, fr.get());
  lazily_test::AssertionKeys expect("message-passing/reconnect_command_projection.json expect",
                                    *fx->find("expect"));
  assert_projection(p, expect, "projection", "reconnect_command_projection");
}

TEST(rpc_call_waits_for_terminal) {
  lazily_test::JsonPtr fx;
  load("rpc_call_waits_for_terminal.json", fx);
  const Json* frames = frames_of(fx.get());
  lazily_test::AssertionKeys expect("message-passing/rpc_call_waits_for_terminal.json expect",
                                    *fx->find("expect"));

  CommandProjection p;
  std::vector<size_t> unresolved;
  std::optional<size_t> resolves_at;
  const std::string cmd = "cmd-run-1";
  for (size_t i = 0; i < frames->array.size(); ++i) {
    fold_frame(p, frames->array[i].get());
    const bool resolved = p.terminal_for(cmd).has_value();
    if (resolved && !resolves_at)
      resolves_at = i;
    else if (!resolved)
      unresolved.push_back(i);
  }
  expect.with_sub("rpc", [&](lazily_test::AssertionKeys& rpc) {
    rpc.assert_key("command_id", cmd);
    REQUIRE(resolves_at.has_value(), "RPC never resolved");
    rpc.assert_key("resolves_after_frame_index", *resolves_at,
                   [](const Json& want) { return static_cast<size_t>(want.as_int()); });
    rpc.assert_key("unresolved_after_frame_indices", unresolved, [](const Json& want) {
      std::vector<size_t> values;
      for (const auto& value : want.array)
        values.push_back(static_cast<size_t>(value->as_int()));
      return values;
    });
    const auto term = p.terminal_for(cmd);
    REQUIRE(term.has_value(), "RPC has no terminal status");
    rpc.assert_key("terminal_status", std::string(status_str(term->status)));
  });
  assert_projection(p, expect, "projection", "rpc_call_waits_for_terminal");
}

int main() {
  REQUIRE_FIXTURES_LOADED(8);
  std::cout << "lazily-cpp command conformance: " << test_passed << "/" << test_count << " passed"
            << std::endl;
  return test_passed == test_count ? 0 : 1;
}
