// Reactive-graph conformance (`#lzspecconf` / `#lzspecedgeindex`).
//
// Replays `lazily-spec/conformance/reactive-graph/*.json` read from the sibling
// lazily-spec checkout — never from a copy in this repo. Until now lazily-cpp
// replayed NONE of this corpus; only lazily-rs did, family-wide. That gap is
// why an invalidation-cascade defect shipped undetected in two other bindings:
// a fixture encoding the violated property already existed and nothing ran it.
//
// ## What this runner does and does not claim
//
// The corpus is written against a reactive graph with first-class *teardown*:
// `begin_scope`/`end_scope`, node `dispose`, `disarm`, and fanout/churn
// generators. lazily-cpp's `Context` has none of those — it exposes
// `dispose_effect`/`dispose_signal` only, and no `TeardownScope` type exists
// anywhere in `include/`. So exactly one fixture is replayable today:
// `transitive_invalidation_reaches_depth.json`, which needs only `cell`,
// `computed`, `read`, and `set_cell`.
//
// The other eight are SKIPPED **by name, with the unsupported ops named**, and
// the skip set is asserted against `EXPECTED_UNSUPPORTED` below so it cannot
// drift silently in either direction: a fixture that becomes replayable fails
// this test until its ledger entry is removed, and a newly-unsupported op fails
// it immediately. Silence is the failure mode this whole effort exists to
// eliminate, so nothing here is allowed to be quiet.
//
// ## Positive assertion
//
// An absence guard proves the corpus is on disk; it cannot prove this binary
// read any of it. Every fixture — including the skipped ones, which are parsed
// to discover their ops — is opened through `spec_fixture_text`, so
// `REQUIRE_FIXTURES_LOADED(9)` is a positive assertion that all nine distinct
// canonical files were actually read. The runner additionally asserts (a) the
// on-disk fixture set matches `FIXTURES` exactly, so an upstream addition
// cannot arrive unexecuted, and (b) a non-zero number of ops and expectations
// actually executed.
//
// ## AsyncContext (a finding, not a pass)
//
// The fixture requires replay against *every* context a binding ships. It is
// not replayed against `AsyncContext`, and that is deliberate: `AsyncContext`
// is a stub with no dependency graph at all. `probe_async_stub()` below proves
// this directly rather than asserting it in prose, and the result is reported
// as a loud skip. It is NOT counted as conformance — a vacuous pass here is
// precisely the defect class this suite exists to catch.

#include <lazily/async_context.hpp>
#include <lazily/context.hpp>

#include <cctype>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "test_spec_fixture.hpp"

using namespace lazily;

namespace {

constexpr const char* kArea = "reactive-graph";

// The canonical fixture set. Asserted against the directory listing so a
// fixture added or renamed upstream fails loudly instead of going unrun.
const std::vector<std::string> FIXTURES = {
    "churn_returns_to_baseline.json",
    "cross_scope_teardown_hazard.json",
    "disarm_disposes_nothing.json",
    "dispose_detaches_edges_both_directions.json",
    "read_after_dispose_is_an_error.json",
    "recycled_id_inherits_nothing.json",
    "scope_teardown_equals_fold_of_disposals.json",
    "scoping_bounds_teardown_not_visibility.json",
    "transitive_invalidation_reaches_depth.json",
};

// Ops this runner can execute against the synchronous `Context`.
const std::set<std::string> SUPPORTED_OPS = {"cell", "computed", "read",
                                             "set_cell"};

// Fixtures that cannot be replayed today, with the reason. Asserted to match
// the observed skip set EXACTLY — this is a ledger of findings against the
// implementation, not a relaxation of the corpus. Every entry here is a
// capability `lazily::Context` does not have.
const std::map<std::string, std::string> EXPECTED_UNSUPPORTED = {
    {"churn_returns_to_baseline.json", "churn, dispose_fanout, fanout"},
    {"cross_scope_teardown_hazard.json", "begin_scope, dispose, end_scope"},
    {"disarm_disposes_nothing.json",
     "begin_scope, disarm, dispose, effect, end_scope"},
    {"dispose_detaches_edges_both_directions.json", "dispose, effect"},
    {"read_after_dispose_is_an_error.json", "dispose"},
    {"recycled_id_inherits_nothing.json",
     "dispose, dispose_fanout, dispose_stale_handle, fanout"},
    {"scope_teardown_equals_fold_of_disposals.json",
     "shape 'scenarios' unsupported; begin_scope, dispose, effect, end_scope"},
    {"scoping_bounds_teardown_not_visibility.json",
     "begin_scope, end_scope"},
};

// ── Minimal JSON reader ────────────────────────────────────────────────────
//
// The fixtures are parsed, not transcribed into C++ constants. A transcription
// is a vendored copy wearing a different hat: it goes green against whatever
// the transcriber typed, which is the exact drift this corpus is read from
// source to prevent.

struct Json;
using JsonPtr = std::shared_ptr<Json>;

struct Json {
  enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
  bool boolean = false;
  double number = 0;
  std::string str;
  std::vector<JsonPtr> array;
  // Ordered so replay is deterministic.
  std::vector<std::pair<std::string, JsonPtr>> object;

  const Json* find(const std::string& key) const {
    for (const auto& kv : object) {
      if (kv.first == key) return kv.second.get();
    }
    return nullptr;
  }
  bool has(const std::string& key) const { return find(key) != nullptr; }
  long long as_int() const { return static_cast<long long>(number); }
};

struct JsonParser {
  const std::string& src;
  std::size_t pos = 0;

  explicit JsonParser(const std::string& s) : src(s) {}

  void skip_ws() {
    while (pos < src.size() &&
           (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\n' ||
            src[pos] == '\r')) {
      ++pos;
    }
  }

  JsonPtr parse() {
    skip_ws();
    REQUIRE(pos < src.size(), "unexpected end of JSON fixture");
    const char c = src[pos];
    if (c == '{') return parse_object();
    if (c == '[') return parse_array();
    if (c == '"') return parse_string();
    if (c == 't' || c == 'f') return parse_bool();
    if (c == 'n') return parse_null();
    return parse_number();
  }

  JsonPtr parse_object() {
    auto node = std::make_shared<Json>();
    node->type = Json::Type::Object;
    ++pos;  // '{'
    skip_ws();
    if (pos < src.size() && src[pos] == '}') {
      ++pos;
      return node;
    }
    while (true) {
      skip_ws();
      auto key = parse_string();
      skip_ws();
      REQUIRE(pos < src.size() && src[pos] == ':', "expected ':' in JSON object");
      ++pos;
      node->object.emplace_back(key->str, parse());
      skip_ws();
      REQUIRE(pos < src.size(), "unterminated JSON object");
      if (src[pos] == ',') {
        ++pos;
        continue;
      }
      REQUIRE(src[pos] == '}', "expected ',' or '}' in JSON object");
      ++pos;
      return node;
    }
  }

  JsonPtr parse_array() {
    auto node = std::make_shared<Json>();
    node->type = Json::Type::Array;
    ++pos;  // '['
    skip_ws();
    if (pos < src.size() && src[pos] == ']') {
      ++pos;
      return node;
    }
    while (true) {
      node->array.push_back(parse());
      skip_ws();
      REQUIRE(pos < src.size(), "unterminated JSON array");
      if (src[pos] == ',') {
        ++pos;
        continue;
      }
      REQUIRE(src[pos] == ']', "expected ',' or ']' in JSON array");
      ++pos;
      return node;
    }
  }

  JsonPtr parse_string() {
    auto node = std::make_shared<Json>();
    node->type = Json::Type::String;
    REQUIRE(pos < src.size() && src[pos] == '"', "expected '\"' starting JSON string");
    ++pos;
    while (pos < src.size() && src[pos] != '"') {
      if (src[pos] == '\\') {
        ++pos;
        REQUIRE(pos < src.size(), "unterminated JSON escape");
        switch (src[pos]) {
          case 'n': node->str += '\n'; break;
          case 't': node->str += '\t'; break;
          case 'r': node->str += '\r'; break;
          case 'b': node->str += '\b'; break;
          case 'f': node->str += '\f'; break;
          case 'u': {
            // Fixture text is ASCII-plus-punctuation; keep the escape verbatim
            // rather than half-decoding UTF-16 surrogate pairs.
            REQUIRE(pos + 4 < src.size(), "truncated \\u escape");
            node->str += src.substr(pos - 1, 6);
            pos += 4;
            break;
          }
          default: node->str += src[pos]; break;
        }
        ++pos;
        continue;
      }
      node->str += src[pos++];
    }
    REQUIRE(pos < src.size(), "unterminated JSON string");
    ++pos;  // closing quote
    return node;
  }

  JsonPtr parse_bool() {
    auto node = std::make_shared<Json>();
    node->type = Json::Type::Bool;
    if (src.compare(pos, 4, "true") == 0) {
      node->boolean = true;
      pos += 4;
    } else {
      REQUIRE(src.compare(pos, 5, "false") == 0, "malformed JSON boolean");
      node->boolean = false;
      pos += 5;
    }
    return node;
  }

  JsonPtr parse_null() {
    auto node = std::make_shared<Json>();
    node->type = Json::Type::Null;
    REQUIRE(src.compare(pos, 4, "null") == 0, "malformed JSON null");
    pos += 4;
    return node;
  }

  JsonPtr parse_number() {
    auto node = std::make_shared<Json>();
    node->type = Json::Type::Number;
    const std::size_t start = pos;
    if (pos < src.size() && (src[pos] == '-' || src[pos] == '+')) ++pos;
    while (pos < src.size() &&
           (std::isdigit(static_cast<unsigned char>(src[pos])) ||
            src[pos] == '.' || src[pos] == 'e' || src[pos] == 'E' ||
            src[pos] == '-' || src[pos] == '+')) {
      ++pos;
    }
    REQUIRE(pos > start, "malformed JSON number");
    node->number = std::stod(src.substr(start, pos - start));
    return node;
  }
};

// ── Replay engine (synchronous Context) ────────────────────────────────────

struct Graph {
  Context ctx;
  std::map<std::string, CellHandle<long long>> cells;
  std::map<std::string, SlotHandle<long long>> slots;

  // Read through the tracking context `c` so a read inside a compute registers
  // the dependency edge. That edge registration is the whole point of the
  // fixture: a chain that does not register at depth stops refreshing there.
  long long read(Context& c, const std::string& id) {
    auto cell = cells.find(id);
    if (cell != cells.end()) return c.get_cell(cell->second);
    auto slot = slots.find(id);
    REQUIRE(slot != slots.end(), "fixture read of an id that was never created");
    return c.get(slot->second);
  }

  long long read(const std::string& id) { return read(ctx, id); }
};

struct Report {
  std::size_t ops = 0;
  std::size_t checks = 0;
};

// Collect every distinct `op.type` a fixture uses.
std::set<std::string> op_types(const Json& fixture) {
  std::set<std::string> types;
  const Json* steps = fixture.find("steps");
  if (steps) {
    for (const auto& step : steps->array) {
      if (const Json* op = step->find("op")) {
        if (const Json* t = op->find("type")) types.insert(t->str);
      }
    }
  }
  if (const Json* scenarios = fixture.find("scenarios")) {
    for (const auto& scenario : scenarios->array) {
      if (const Json* s = scenario->find("steps")) {
        for (const auto& step : s->array) {
          if (const Json* op = step->find("op")) {
            if (const Json* t = op->find("type")) types.insert(t->str);
          }
        }
      }
    }
  }
  return types;
}

std::string join(const std::set<std::string>& items) {
  std::string out;
  for (const auto& item : items) {
    if (!out.empty()) out += ", ";
    out += item;
  }
  return out;
}

// Replay a `steps`-shaped fixture. Any expectation mismatch is a FINDING and
// aborts — the fixture is never edited and no assertion is loosened.
Report replay_steps(const std::string& name, const Json& fixture) {
  Graph graph;
  Report report;

  const Json* steps = fixture.find("steps");
  REQUIRE(steps != nullptr, "fixture declares shape 'steps' but has no steps");

  for (const auto& step : steps->array) {
    const Json* op = step->find("op");
    REQUIRE(op != nullptr, "fixture step has no op");
    const Json* type = op->find("type");
    REQUIRE(type != nullptr, "fixture op has no type");
    const std::string& kind = type->str;
    const Json* id_node = op->find("id");
    REQUIRE(id_node != nullptr, "fixture op has no id");
    const std::string id = id_node->str;
    ++report.ops;

    if (kind == "cell") {
      const Json* value = op->find("value");
      REQUIRE(value != nullptr, "cell op has no value");
      graph.cells.emplace(id, graph.ctx.cell<long long>(value->as_int()));
    } else if (kind == "computed") {
      const Json* reads = op->find("reads");
      REQUIRE(reads != nullptr, "computed op has no reads");
      std::vector<std::string> sources;
      for (const auto& r : reads->array) sources.push_back(r->str);
      const Json* offset_node = op->find("offset");
      const long long offset = offset_node ? offset_node->as_int() : 0;
      Graph* g = &graph;
      graph.slots.emplace(
          id, graph.ctx.computed<long long>([g, sources, offset](Context& c) {
            long long sum = offset;
            for (const auto& source : sources) sum += g->read(c, source);
            return sum;
          }));
    } else if (kind == "read") {
      graph.read(id);
    } else if (kind == "set_cell") {
      const Json* value = op->find("value");
      REQUIRE(value != nullptr, "set_cell op has no value");
      auto cell = graph.cells.find(id);
      REQUIRE(cell != graph.cells.end(), "set_cell on an id that is not a cell");
      graph.ctx.set_cell(cell->second, value->as_int());
    } else {
      REQUIRE(false, "unsupported op reached the replay engine");
    }

    const Json* expect = step->find("expect");
    if (!expect) continue;

    // `value`: the value observable at this step's own id.
    if (const Json* expected = expect->find("value")) {
      const long long actual = graph.read(id);
      ++report.checks;
      if (actual != expected->as_int()) {
        std::cout << "FAIL: " << name << " op " << kind << "(" << id
                  << ") expected value " << expected->as_int() << ", observed "
                  << actual
                  << " — reactive-graph conformance FINDING (fixture is "
                     "canonical and is not edited)"
                  << std::endl;
        std::abort();
      }
    }

    // `read`: values observable at a set of ids after this step. This is the
    // discriminating assertion for transitive invalidation — depth 2 and 3
    // only refresh if the cascade reaches the whole cone.
    if (const Json* reads = expect->find("read")) {
      for (const auto& kv : reads->object) {
        const long long actual = graph.read(kv.first);
        ++report.checks;
        if (actual != kv.second->as_int()) {
          std::cout << "FAIL: " << name << " after op " << kind << "(" << id
                    << ") expected read " << kv.first << " == "
                    << kv.second->as_int() << ", observed " << actual
                    << " — reactive-graph conformance FINDING (fixture is "
                       "canonical and is not edited)"
                    << std::endl;
          std::abort();
        }
      }
    }

    // An unrecognised assertion key must not pass unnoticed.
    for (const auto& kv : expect->object) {
      REQUIRE(kv.first == "value" || kv.first == "read" || kv.first == "note",
              "unrecognised expectation key in fixture — it would be silently "
              "ignored");
    }
  }

  return report;
}

// ── AsyncContext stub probe (a finding, not a conformance run) ─────────────
//
// The fixture demands replay against every context the binding ships.
// `AsyncContext` cannot host it, and the reason must be demonstrated rather
// than asserted in a comment. `AsyncSlotNode` (async_context.hpp) carries
// `compute/value/error/state/revision/equals` and NO `dependents` or
// `dependencies` field; `AsyncContext::slot()` never registers the node in any
// registry — the returned handle owns the only reference to it.
//
// Two consequences, each proved below:
//
//   1. `get()` returns a cached value that nothing ever invalidates. After a
//      source write it keeps serving the pre-write value forever. Replaying
//      the fixture through `get()` would FAIL at depth 1 — but that failure
//      would be reported against a context that has no graph to be wrong
//      about, which is a category error, not a conformance result.
//
//   2. `get_async()` recomputes unconditionally on every call — no staleness
//      check, no cache consultation. Replaying the fixture through
//      `get_async()` would therefore PASS every step, at every depth, for the
//      same reason a function that ignores its cache always returns a fresh
//      answer. That is a VACUOUS pass: it demonstrates the absence of caching,
//      not the presence of invalidation. Counting it as conformance would be
//      exactly the failure this suite exists to eliminate, so it is not run.
//
// The honest outcome is a loud skip naming the missing capability, which is
// what `main` reports.
void probe_async_stub() {
  AsyncContext actx;
  auto source = actx.cell<long long>(1);

  int computes = 0;
  auto derived = actx.slot<long long>([&source, &computes]() -> long long {
    ++computes;
    return source.get() + 10;
  });

  derived.get_async().get();
  REQUIRE(computes == 1, "async slot did not compute once");
  REQUIRE(derived.get().has_value() && *derived.get() == 11,
          "async slot did not resolve to its computed value");

  // (1) Nothing invalidates the cache: after a source write, `get()` still
  //     serves the pre-write value. No dependency edge exists to carry the
  //     write, because `AsyncSlotNode` has no dependents/dependencies fields.
  source.set(2);
  REQUIRE(derived.get().has_value() && *derived.get() == 11,
          "AsyncContext unexpectedly invalidated a cached slot — the stub "
          "characterisation in this file and in lazily-spec needs revising");
  REQUIRE(computes == 1,
          "AsyncContext unexpectedly recomputed on a source write — the stub "
          "characterisation needs revising");

  // (2) `get_async()` recomputes unconditionally — twice in a row with no
  //     intervening write still bumps the compute count. This is what would
  //     make a fixture replay pass vacuously.
  derived.get_async().get();
  REQUIRE(computes == 2, "async get_async did not recompute after a write");
  derived.get_async().get();
  REQUIRE(computes == 3,
          "AsyncContext consulted a cache on an unchanged read — it is no "
          "longer unconditional, so the vacuity analysis needs revising");
}

}  // namespace

int main() {
  // Absence is an explicit CTest SKIP (exit 77), never a silent pass. CI also
  // asserts the directory exists and that no Conformance test reported
  // "Skipped", because ctest treats a skip as success.
  lazily_test::require_spec_checkout_or_skip(kArea);

  // The fixture set on disk must be exactly the one this runner knows about,
  // so an upstream addition cannot arrive unexecuted.
  std::set<std::string> on_disk;
  for (const auto& entry : std::filesystem::directory_iterator(
           lazily_test::spec_conformance_dir() / kArea)) {
    const auto file = entry.path().filename().string();
    if (file.size() > 5 && file.compare(file.size() - 5, 5, ".json") == 0) {
      on_disk.insert(file);
    }
  }
  const std::set<std::string> known(FIXTURES.begin(), FIXTURES.end());
  if (on_disk != known) {
    std::cout << "FAIL: reactive-graph fixture set drifted; every fixture must "
                 "be accounted for by this runner"
              << std::endl;
    return 1;
  }

  std::size_t replayed = 0;
  std::size_t total_ops = 0;
  std::size_t total_checks = 0;
  std::map<std::string, std::string> observed_unsupported;

  for (const auto& name : FIXTURES) {
    // Every fixture is opened — including skipped ones, whose ops are read
    // from the file rather than assumed. This is what makes
    // REQUIRE_FIXTURES_LOADED a real positive assertion.
    const std::string text = lazily_test::spec_fixture_text(kArea, name);
    JsonParser parser(text);
    const JsonPtr fixture = parser.parse();

    const Json* shape = fixture->find("shape");
    REQUIRE(shape != nullptr, "fixture declares no shape");

    std::set<std::string> unsupported;
    for (const auto& op : op_types(*fixture)) {
      if (SUPPORTED_OPS.count(op) == 0) unsupported.insert(op);
    }

    std::string reason;
    if (shape->str != "steps") {
      reason = "shape '" + shape->str + "' unsupported";
    }
    if (!unsupported.empty()) {
      if (!reason.empty()) reason += "; ";
      reason += join(unsupported);
    }

    if (!reason.empty()) {
      observed_unsupported.emplace(name, reason);
      std::cout << "SKIP " << name << ": unsupported by lazily::Context — "
                << reason
                << ". lazily-cpp exposes no teardown-scope or node-disposal "
                   "API (dispose_effect/dispose_signal only)."
                << std::endl;
      continue;
    }

    const Report report = replay_steps(name, *fixture);
    ++replayed;
    total_ops += report.ops;
    total_checks += report.checks;
    std::cout << "PASS " << name << " [Context]: " << report.ops << " ops, "
              << report.checks << " expectations" << std::endl;
  }

  // The skip ledger must match EXACTLY. A fixture that becomes replayable
  // fails here until its entry is removed; a newly-unsupported op fails here
  // immediately. Neither direction is allowed to be silent.
  if (observed_unsupported != EXPECTED_UNSUPPORTED) {
    std::cout << "FAIL: the unsupported-fixture ledger drifted. Observed:"
              << std::endl;
    for (const auto& kv : observed_unsupported) {
      std::cout << "  {\"" << kv.first << "\", \"" << kv.second << "\"},"
                << std::endl;
    }
    return 1;
  }

  // Positive assertions: an absence guard cannot catch a runner that replays
  // nothing.
  if (replayed == 0 || total_ops == 0 || total_checks == 0) {
    std::cout << "FAIL: reactive-graph conformance replayed nothing (" << replayed
              << " fixtures, " << total_ops << " ops, " << total_checks
              << " expectations) — the suite is not exercising the corpus it "
                 "claims to"
              << std::endl;
    return 1;
  }

  // Demonstrate, rather than assert in prose, that AsyncContext has no
  // dependency graph — and therefore that replaying the fixture against it
  // would either fail as a category error or pass vacuously.
  probe_async_stub();
  std::cout
      << "SKIP transitive_invalidation_reaches_depth.json [AsyncContext]: "
         "AsyncSlotNode has no dependents/dependencies; slot() registers the "
         "node nowhere; get_async() recomputes unconditionally and get() "
         "serves a cache nothing invalidates. Replay through get_async() "
         "would pass VACUOUSLY (no cache to serve stale) and through get() "
         "would fail as a category error (no graph to be wrong about). "
         "Neither is conformance — AsyncContext needs a dependency graph, or "
         "should declare async: none."
      << std::endl;

  // All nine canonical fixtures were actually opened and parsed.
  REQUIRE_FIXTURES_LOADED(9);

  std::cout << "reactive-graph conformance: " << replayed << "/"
            << FIXTURES.size() << " fixtures replayed against Context ("
            << total_ops << " ops, " << total_checks << " expectations), "
            << observed_unsupported.size()
            << " skipped with unsupported ops named, AsyncContext skipped as a "
               "stub"
            << std::endl;
  return 0;
}
