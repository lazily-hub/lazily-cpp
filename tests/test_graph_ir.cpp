#include <lazily/graph_ir.hpp>

#include <cassert>
#include <iostream>
#include <string>

namespace {

using lazily::Edge;
using lazily::GraphEdit;
using lazily::GraphIr;
using lazily::GraphIrError;
using lazily::GraphRuntime;
using lazily::JsonValue;

int failures = 0;

void expect(bool cond, const char* text, int line) {
  if (!cond) {
    std::cerr << "FAIL " << line << ": " << text << "\n";
    ++failures;
  }
}

#define EXPECT(cond) expect(static_cast<bool>(cond), #cond, __LINE__)

template <class F>
void expect_fault(const std::string& fragment, F&& fn, int line) {
  try {
    fn();
    std::cerr << "FAIL " << line << ": expected fault containing " << fragment << "\n";
    ++failures;
  } catch (const GraphIrError& err) {
    if (std::string(err.what()).find(fragment) == std::string::npos) {
      std::cerr << "FAIL " << line << ": `" << err.what() << "` missing " << fragment << "\n";
      ++failures;
    }
  }
}

#define FAULT(fragment, fn) expect_fault(fragment, fn, __LINE__)

std::string edges_of(const std::vector<Edge>& edges) {
  std::string out;
  for (const auto& edge : edges) out += edge.from + " " + edge.kind + " " + edge.to + "\n";
  return out;
}

void expect_edges(const std::vector<Edge>& got, const std::vector<Edge>& want, int line) {
  if (got != want) {
    std::cerr << "FAIL " << line << " edges\n got:\n" << edges_of(got) << " want:\n" << edges_of(want);
    ++failures;
  }
}

const char* kCounter = R"({
  "kind": "graph", "ir": "1", "id": "counter",
  "nodes": [
    {"id": "count", "kind": "source", "init": 1},
    {"id": "doubled", "kind": "computed", "eager": true,
      "expr": {"op": "mul", "args": [{"ref": "count"}, {"lit": 2}]}},
    {"id": "log", "kind": "effect", "body": {"do": "eval", "expr": {"ref": "doubled"}}}
  ]
})";

const char* kCounterEdit = R"({
  "kind": "edit", "ir": "1",
  "ops": [
    {"op": "replace", "node": {"id": "doubled", "kind": "computed",
      "expr": {"op": "add", "args": [{"ref": "count"}, {"lit": 10}]}}},
    {"op": "add", "node": {"id": "label", "kind": "source", "init": "n"}},
    {"op": "add", "node": {"id": "shown", "kind": "computed",
      "expr": {"op": "concat", "args": [{"ref": "label"}, {"lit": ":"}]}}},
    {"op": "remove", "id": "log"}
  ]
})";

const char* kCounterSource =
    "graph counter ir 1\n"
    "source count = 1\n"
    "computed doubled eager = mul(ref(count), lit(2))\n"
    "effect log = eval(ref(doubled))\n";

void test_counter() {
  const GraphIr doc = GraphIr::from_json_str(kCounter);
  EXPECT(doc.id() != nullptr && *doc.id() == "counter");
  EXPECT(doc.node_eager("doubled"));
  EXPECT(doc.node_expr_op("doubled") == "mul");
  expect_edges(doc.static_edges(),
               {Edge{"doubled", "read", "count"}, Edge{"log", "read", "doubled"}}, __LINE__);
  EXPECT(doc.emit_source() == kCounterSource);
  EXPECT(GraphIr::from_json_str(doc.to_json_text()).emit_source() == kCounterSource);

  GraphRuntime rt = GraphRuntime::lower(doc);
  EXPECT(rt.get("doubled").integer == 2);
  EXPECT(rt.effect_trace().size() == 1);
  EXPECT(rt.effect_trace()[0].id == "log");
  EXPECT(rt.effect_trace()[0].result.integer == 2);
  EXPECT(rt.observed_reads("doubled") == std::vector<std::string>{"count"});
  EXPECT(rt.observed_reads("log") == std::vector<std::string>{"doubled"});
  EXPECT(rt.is_eager("doubled"));
  EXPECT(rt.source_policy("count") == "keep-latest");

  const std::string before = rt.emit_source();
  rt.write_source("count", JsonValue::of_int(4));
  EXPECT(rt.get("doubled").integer == 8);
  EXPECT(rt.effect_trace().size() == 2);
  EXPECT(rt.effect_trace()[1].result.integer == 8);
  EXPECT(rt.emit_source() == before);

  rt.apply(GraphEdit::from_json_str(kCounterEdit));
  EXPECT(rt.get("doubled").integer == 14);
  EXPECT(rt.get("shown").string == "n:");
  EXPECT(!rt.contains("log"));
  EXPECT(!rt.is_eager("doubled"));
  const auto trace_len = rt.effect_trace().size();
  rt.write_source("count", JsonValue::of_int(5));
  EXPECT(rt.get("doubled").integer == 15);
  EXPECT(rt.effect_trace().size() == trace_len);
  EXPECT(rt.emit_source() ==
         "graph counter ir 1\n"
         "source count = 1\n"
         "computed doubled = add(ref(count), lit(10))\n"
         "source label = \"n\"\n"
         "computed shown = concat(ref(label), lit(\":\"))\n");
}

void test_dynamic_if() {
  GraphRuntime rt = GraphRuntime::load_json(R"({
    "kind":"graph","ir":"1","nodes":[
      {"id":"flag","kind":"source","init":false},
      {"id":"left","kind":"source","init":"L"},
      {"id":"right","kind":"source","init":"R"},
      {"id":"picked","kind":"computed","expr":{"op":"if","args":[{"ref":"flag"},{"ref":"left"},{"ref":"right"}]}}
    ]})");
  expect_edges(rt.static_edges(),
               {Edge{"picked", "read", "flag"}, Edge{"picked", "read", "left"}, Edge{"picked", "read", "right"}},
               __LINE__);
  EXPECT(rt.get("picked").string == "R");
  EXPECT(rt.observed_reads("picked") == (std::vector<std::string>{"flag", "right"}));
  rt.write_source("flag", JsonValue::of_bool(true));
  EXPECT(rt.get("picked").string == "L");
  EXPECT(rt.observed_reads("picked") == (std::vector<std::string>{"flag", "left"}));
}

void test_sum() {
  GraphRuntime rt = GraphRuntime::load_json(R"({
    "kind":"graph","ir":"1","nodes":[
      {"id":"tick","kind":"source","init":0},
      {"id":"total","kind":"source","init":0,"policy":"sum"},
      {"id":"bump","kind":"effect","body":{"do":"seq","steps":[
        {"do":"eval","expr":{"ref":"tick"}},
        {"do":"merge","target":"total","expr":{"lit":1}}
      ]}}
    ]})");
  EXPECT(rt.emit_source().find("policy sum") != std::string::npos);
  EXPECT(rt.emit_source().find("merge(total, lit(1))") != std::string::npos);
  EXPECT(rt.get("total").integer == 1);
  EXPECT(rt.effect_trace()[0].id == "bump");
  EXPECT(rt.effect_trace()[0].result.array.size() == 2);
  EXPECT(rt.effect_trace()[0].result.array[0].integer == 0);
  EXPECT(rt.effect_trace()[0].result.array[1].integer == 1);
  EXPECT(rt.source_policy("total") == "sum");
  rt.write_source("tick", JsonValue::of_int(1));
  EXPECT(rt.get("total").integer == 2);
  EXPECT(rt.effect_trace().size() == 2);
  rt.write_source("tick", JsonValue::of_int(1));
  EXPECT(rt.effect_trace().size() == 2);
  EXPECT(rt.get("total").integer == 2);
}

void test_structures() {
  GraphRuntime rt = GraphRuntime::load_json(R"({
    "kind":"graph","ir":"1","nodes":[
      {"id":"row","kind":"source","init":{"name":"ada","n":2}},
      {"id":"name","kind":"computed","expr":{"op":"get","args":[{"ref":"row"},{"lit":"name"}]}},
      {"id":"n","kind":"computed","expr":{"op":"get","args":[{"ref":"row"},{"lit":"n"}]}},
      {"id":"tag","kind":"computed","expr":{"op":"concat","args":[{"ref":"name"},{"lit":"#"}]}},
      {"id":"width","kind":"computed","expr":{"op":"len","args":[{"ref":"name"}]}},
      {"id":"pair","kind":"computed","expr":{"op":"array","args":[{"ref":"name"},{"ref":"n"}]}},
      {"id":"wrapped","kind":"computed","expr":{"object":[{"key":"name","value":{"ref":"name"}}]}}
    ]})");
  EXPECT(rt.get("name").string == "ada");
  EXPECT(rt.get("n").integer == 2);
  EXPECT(rt.get("tag").string == "ada#");
  EXPECT(rt.get("width").integer == 3);
  const JsonValue pair = rt.get("pair");
  EXPECT(pair.array.size() == 2 && pair.array[0].string == "ada" && pair.array[1].integer == 2);
  const JsonValue wrapped = rt.get("wrapped");
  EXPECT(wrapped.object.size() == 1 && wrapped.find("name")->string == "ada");
}

void test_cycle_and_snapshot() {
  FAULT("cycle in read edges", [] {
    GraphIr::from_json_str(
        R"({"kind":"graph","ir":"1","nodes":[{"id":"a","kind":"computed","expr":{"ref":"b"}},{"id":"b","kind":"computed","expr":{"ref":"a"}}]})");
  });
  FAULT("graph", [] {
    GraphIr::from_json_str(R"({"Snapshot":{"epoch":0,"nodes":[],"edges":[],"roots":[]}})");
  });
}

void test_policies() {
  GraphRuntime rt = GraphRuntime::load_json(R"({
    "kind":"graph","ir":"1","nodes":[
      {"id":"empty","kind":"source","init":null},
      {"id":"hi","kind":"source","init":3,"policy":"max"},
      {"id":"lo","kind":"source","init":3,"policy":"min"},
      {"id":"both","kind":"source","init":true,"policy":"and"},
      {"id":"either","kind":"source","init":false,"policy":"or"},
      {"id":"fold","kind":"effect","body":{"do":"seq","steps":[
        {"do":"merge","target":"hi","expr":{"lit":1}},
        {"do":"merge","target":"hi","expr":{"lit":5}},
        {"do":"merge","target":"hi","expr":{"lit":5}},
        {"do":"merge","target":"lo","expr":{"lit":1}},
        {"do":"merge","target":"both","expr":{"lit":false}},
        {"do":"merge","target":"either","expr":{"lit":true}}
      ]}}
    ]})");
  EXPECT(rt.get("empty").is_null());
  EXPECT(rt.get("hi").integer == 5);
  EXPECT(rt.get("lo").integer == 1);
  EXPECT(rt.get("both").boolean == false);
  EXPECT(rt.get("either").boolean == true);
  EXPECT(rt.source_policy("hi") == "max");
  EXPECT(rt.source_policy("lo") == "min");
  EXPECT(rt.source_policy("both") == "and");
  EXPECT(rt.source_policy("either") == "or");
}

void test_short_circuit() {
  GraphRuntime rt = GraphRuntime::load_json(R"({
    "kind":"graph","ir":"1","nodes":[
      {"id":"flag","kind":"source","init":false},
      {"id":"n","kind":"source","init":1},
      {"id":"bad","kind":"computed","expr":{"op":"div","args":[{"ref":"n"},{"lit":0}]}},
      {"id":"gate","kind":"computed","expr":{"op":"and","args":[{"ref":"flag"},{"ref":"bad"}]}}
    ]})");
  EXPECT(rt.get("gate").boolean == false);
  EXPECT(rt.observed_reads("gate") == std::vector<std::string>{"flag"});
  FAULT("division by zero", [&] { rt.get("bad"); });
}

void test_failed_edits() {
  GraphRuntime rt = GraphRuntime::load_json(
      R"({"kind":"graph","ir":"1","nodes":[{"id":"a","kind":"source","init":1},{"id":"b","kind":"computed","expr":{"ref":"a"}}]})");
  FAULT("edit op 0", [&] {
    rt.apply(GraphEdit::from_json_str(R"({"kind":"edit","ir":"1","ops":[{"op":"remove","id":"a"}]})"));
  });
  EXPECT(rt.contains("a") && rt.contains("b"));
  EXPECT(rt.get("b").integer == 1);

  GraphRuntime empty = GraphRuntime::load_json(R"({"kind":"graph","ir":"1","nodes":[]})");
  FAULT("edit op 0", [&] {
    empty.apply(GraphEdit::from_json_str(
        R"({"kind":"edit","ir":"1","ops":[{"op":"add","node":{"id":"a","kind":"computed","expr":{"ref":"b"}}},{"op":"add","node":{"id":"b","kind":"source","init":1}}]})"));
  });
  EXPECT(!empty.contains("a") && !empty.contains("b"));
  EXPECT(empty.emit_source() == "graph ir 1\n");
}

void test_replace_and_when() {
  GraphRuntime rt = GraphRuntime::load_json(
      R"({"kind":"graph","ir":"1","id":"g","nodes":[{"id":"count","kind":"source","init":1}]})");
  rt.write_source("count", JsonValue::of_int(9));
  EXPECT(rt.get("count").integer == 9);
  EXPECT(rt.emit_source() == "graph g ir 1\nsource count = 1\n");
  rt.apply(GraphEdit::from_json_str(
      R"({"kind":"edit","ir":"1","ops":[{"op":"replace","node":{"id":"count","kind":"source","init":0}}]})"));
  EXPECT(rt.get("count").integer == 0);
  EXPECT(rt.emit_source() == "graph g ir 1\nsource count = 0\n");

  GraphRuntime on = GraphRuntime::load_json(
      R"({"kind":"graph","ir":"1","nodes":[{"id":"on","kind":"source","init":true},{"id":"value","kind":"source","init":0},{"id":"apply","kind":"effect","body":{"do":"when","cond":{"ref":"on"},"then":{"do":"set","target":"value","expr":{"lit":7}}}}]})");
  EXPECT(on.get("value").integer == 7);
  EXPECT(on.effect_trace()[0].result.integer == 7);
  GraphRuntime off = GraphRuntime::load_json(
      R"({"kind":"graph","ir":"1","nodes":[{"id":"on","kind":"source","init":false},{"id":"value","kind":"source","init":0},{"id":"apply","kind":"effect","body":{"do":"when","cond":{"ref":"on"},"then":{"do":"set","target":"value","expr":{"lit":7}}}}]})");
  EXPECT(off.get("value").integer == 0);
  EXPECT(off.effect_trace()[0].result.is_null());
}

void test_rejections() {
  GraphRuntime rt = GraphRuntime::load_json(R"({"kind":"graph","ir":"1","nodes":[]})");
  rt.apply(GraphEdit::from_json_str(
      R"({"kind":"edit","ir":"1","ops":[{"op":"add","node":{"id":"x","kind":"source","init":1}}]})"));
  EXPECT(rt.get("x").integer == 1);
  EXPECT(rt.emit_source() == "graph ir 1\nsource x = 1\n");
  for (const char* text : {R"({"kind":"graph","ir":"2","nodes":[]})",
                           R"({"kind":"graph","ir":"1","nodes":[{"id":"1count","kind":"source","init":0}]})",
                           R"({"kind":"graph","ir":"1","nodes":[{"id":"c","kind":"computed","expr":{"op":"pow","args":[{"lit":2},{"lit":3}]}}]})",
                           R"({"kind":"graph","ir":"1","nodes":[{"id":"e","kind":"effect","body":"log"}]})"}) {
    FAULT("", [&] { GraphIr::from_json_str(text); });
  }
  FAULT("", [] { GraphEdit::from_json_str(R"({"kind":"edit","ir":"1","ops":[]})"); });
}

void test_budget() {
  const GraphIr doc = GraphIr::from_json_str(
      R"({"kind":"graph","ir":"1","nodes":[{"id":"n","kind":"source","init":0},{"id":"loop","kind":"effect","body":{"do":"set","target":"n","expr":{"op":"add","args":[{"ref":"n"},{"lit":1}]}}}]})");
  GraphRuntime rt = GraphRuntime::lower(doc, 4);
  EXPECT(rt.last_drain_exhaustion() == 4);
  EXPECT(rt.get("n").integer == 4);
}

}  // namespace

int main() {
  test_counter();
  test_dynamic_if();
  test_sum();
  test_structures();
  test_cycle_and_snapshot();
  test_policies();
  test_short_circuit();
  test_failed_edits();
  test_replace_and_when();
  test_rejections();
  test_budget();
  if (failures != 0) {
    std::cerr << failures << " failure(s)\n";
    return 1;
  }
  return 0;
}
