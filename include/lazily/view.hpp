#ifndef LAZILY_VIEW_HPP
#define LAZILY_VIEW_HPP

#include <lazily/context.hpp>
#include <lazily/json.hpp>
#include <lazily/statechart.hpp>

#include <climits>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lazily {

// Host-neutral view compute document. Signals are sources, standing expressions
// are guarded computeds, and the optional chart is one statechart cell.
class View {
 public:
  struct Value {
    enum class Kind { Null, Bool, Int, Str };
    Kind kind = Kind::Null;
    bool b = false;
    int64_t n = 0;
    std::string s;

    static Value nul() { return {}; }
    static Value boolean(bool v) {
      Value out;
      out.kind = Kind::Bool;
      out.b = v;
      return out;
    }
    static Value integer(int64_t v) {
      Value out;
      out.kind = Kind::Int;
      out.n = v;
      return out;
    }
    static Value str(std::string v) {
      Value out;
      out.kind = Kind::Str;
      out.s = std::move(v);
      return out;
    }
    bool operator==(const Value& o) const {
      if (kind != o.kind) return false;
      switch (kind) {
        case Kind::Bool:
          return b == o.b;
        case Kind::Int:
          return n == o.n;
        case Kind::Str:
          return s == o.s;
        case Kind::Null:
          return true;
      }
      return false;
    }
    bool operator!=(const Value& o) const { return !(*this == o); }
    JsonValue json() const {
      switch (kind) {
        case Kind::Bool:
          return JsonValue::of_bool(b);
        case Kind::Int:
          return JsonValue::of_int(n);
        case Kind::Str:
          return JsonValue::of_string(s);
        case Kind::Null:
          return JsonValue::null();
      }
      return JsonValue::null();
    }
  };

  static View install(Context& ctx, const JsonValue& doc);

  JsonValue signals_json() const;
  JsonValue expressions_json() const;
  JsonValue layout_json() const;
  JsonValue chart_json() const;
  JsonValue dispatch(const std::string& node, const std::string& event, const JsonValue& value);

  struct Node {
    std::string role;
    std::vector<std::string> children;
    std::optional<std::string> flow;
  };
  struct Impl : std::enable_shared_from_this<Impl> {
    Context* ctx = nullptr;
    std::string root;
    std::unordered_map<std::string, Node> nodes;
    std::unordered_map<std::string, Source<Value>> signals;
    std::unordered_map<std::string, Computed<Value>> exprs;
    std::unordered_map<std::string, JsonValue> effects;
    std::optional<StateChart> chart;
    Value payload;
    bool in_effect = false;

    void populate(Context& context, const JsonValue& doc);
    Value eval_expr(const JsonValue& expr, Compute* cx, bool in_effect_now);
    Value eval_arg(const JsonValue& value, Compute* cx, bool in_effect_now);
    Value eval_call(const JsonArray& args, Compute* cx, bool in_effect_now);
    void exec(const JsonValue& effect, JsonValue& trace);
    void walk(JsonValue& out, const std::string& id) const;
  };

  explicit View(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
  std::shared_ptr<Impl> impl_;
};

namespace view_detail {

inline void reject(const std::string& message) { throw std::runtime_error(message); }

inline bool one_of(const std::string& text, std::initializer_list<const char*> keys) {
  for (const char* key : keys)
    if (text == key) return true;
  return false;
}

inline const JsonValue& need(const JsonValue& obj, const char* key) {
  const JsonValue* found = obj.find(key);
  if (found == nullptr) reject(std::string("missing ") + key);
  return *found;
}

inline void allow(const JsonValue& obj, std::initializer_list<const char*> keys) {
  if (!obj.is_object()) reject("expected object");
  for (const auto& kv : obj.object) {
    if (!one_of(kv.first, keys)) reject("unknown field " + kv.first);
  }
}

inline const std::string& as_string(const JsonValue& value, const char* what) {
  if (!value.is_string()) reject(std::string(what) + " must be a string");
  return value.string;
}

inline View::Value literal(const JsonValue& value) {
  switch (value.type) {
    case JsonValue::Type::Null:
      return View::Value::nul();
    case JsonValue::Type::Bool:
      return View::Value::boolean(value.boolean);
    case JsonValue::Type::Int:
      return View::Value::integer(value.integer);
    case JsonValue::Type::String:
      return View::Value::str(value.string);
    case JsonValue::Type::Double:
      if (value.real != static_cast<double>(static_cast<int64_t>(value.real)))
        reject("non-integer number");
      return View::Value::integer(static_cast<int64_t>(value.real));
    default:
      reject("value must be null, boolean, integer, or string");
  }
  return {};
}

inline std::string stringify(const View::Value& value) {
  switch (value.kind) {
    case View::Value::Kind::Bool:
      return value.b ? "true" : "false";
    case View::Value::Kind::Int:
      return std::to_string(value.n);
    case View::Value::Kind::Str:
      return value.s;
    case View::Value::Kind::Null:
      return "";
  }
  return "";
}

inline bool is_space(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

inline std::string trim_copy(const std::string& text) {
  size_t begin = 0;
  while (begin < text.size() && is_space(static_cast<unsigned char>(text[begin]))) ++begin;
  size_t end = text.size();
  while (end > begin && is_space(static_cast<unsigned char>(text[end - 1]))) --end;
  return text.substr(begin, end - begin);
}

inline int64_t utf8_len(const std::string& text) {
  int64_t count = 0;
  for (unsigned char c : text) {
    if ((c & 0xC0) != 0x80) ++count;
  }
  return count;
}

inline void validate_arg(const JsonValue& value, const std::unordered_map<std::string, View::Value>& signals,
                         bool in_effect);
inline void validate_expr(const JsonValue& expr, const std::unordered_map<std::string, View::Value>& signals,
                          bool in_effect);
inline void validate_effect(const JsonValue& effect, const std::unordered_map<std::string, View::Value>& signals,
                            bool has_chart);

inline void validate_call(const JsonArray& args, const std::unordered_map<std::string, View::Value>& signals,
                          bool in_effect) {
  if (args.empty() || !args[0].is_string()) reject("call takes a name");
  const std::string& name = args[0].string;
  if (!one_of(name, {"event", "not", "add", "len"})) reject("unknown call " + name);
  const size_t rest = args.size() - 1;
  if (name == "event") {
    if (rest != 0 || !in_effect) reject("event is illegal outside an effect");
    return;
  }
  if (name == "not" && rest != 1) reject("not takes one boolean");
  if (name == "add" && rest < 2) reject("add takes two or more integers");
  if (name == "len" && rest != 1) reject("len takes one string");
  for (size_t i = 1; i < args.size(); ++i) validate_arg(args[i], signals, in_effect);
}

inline void validate_expr(const JsonValue& expr, const std::unordered_map<std::string, View::Value>& signals,
                          bool in_effect) {
  allow(expr, {"op", "args"});
  const std::string op = as_string(need(expr, "op"), "op");
  if (!one_of(op, {"lit", "ref", "call", "if", "eq", "concat", "trim"})) reject("unknown expression op " + op);
  if (!need(expr, "args").is_array()) reject("expression args must be an array");
  const auto& args = expr.find("args")->array;
  if (op == "lit") {
    if (args.size() != 1) reject("lit takes one literal");
    (void)literal(args[0]);
    return;
  }
  if (op == "ref") {
    if (args.size() != 1 || !args[0].is_string()) reject("ref takes one signal id");
    if (!signals.count(args[0].string)) reject("unknown signal " + args[0].string);
    return;
  }
  if (op == "call") return validate_call(args, signals, in_effect);
  if (op == "if" && args.size() != 3) reject("if takes three arguments");
  if (op == "eq" && args.size() != 2) reject("eq takes two arguments");
  if (op == "trim" && args.size() != 1) reject("trim takes one value");
  for (const auto& arg : args) validate_arg(arg, signals, in_effect);
}

inline void validate_arg(const JsonValue& value, const std::unordered_map<std::string, View::Value>& signals,
                         bool in_effect) {
  if (value.is_object()) return validate_expr(value, signals, in_effect);
  (void)literal(value);
}

inline void validate_effect(const JsonValue& effect, const std::unordered_map<std::string, View::Value>& signals,
                            bool has_chart) {
  allow(effect, {"op", "args"});
  const std::string op = as_string(need(effect, "op"), "op");
  if (!one_of(op, {"set", "batch", "send"})) reject("unknown effect op " + op);
  if (!need(effect, "args").is_array()) reject("effect args must be an array");
  const auto& args = effect.find("args")->array;
  if (op == "set") {
    if (args.size() != 2 || !args[0].is_string()) reject("set takes a signal id and a value");
    if (!signals.count(args[0].string)) reject("unknown signal " + args[0].string);
    return validate_arg(args[1], signals, true);
  }
  if (op == "send") {
    if (!has_chart) reject("send requires a chart");
    if ((args.size() != 1 && args.size() != 2) || !args[0].is_string())
      reject("send takes an event name and an optional payload");
    if (args.size() == 2) return validate_arg(args[1], signals, true);
    return;
  }
  if (args.empty()) reject("batch takes one or more effects");
  for (const auto& nested : args) validate_effect(nested, signals, has_chart);
}

inline std::vector<std::string> children_of(const std::string& id, const JsonValue& raw) {
  if (!raw.is_array()) reject("node " + id + " children must be an array");
  std::vector<std::string> out;
  for (const auto& child : raw.array) {
    if (!child.is_string() || child.string.empty()) reject("node " + id + " children must be non-empty strings");
    for (const auto& prev : out)
      if (prev == child.string) reject("node " + id + " repeats child " + child.string);
    out.push_back(child.string);
  }
  return out;
}

inline std::optional<std::string> layout_of(const std::string& id, const JsonValue& raw) {
  allow(raw, {"flow", "align", "grow"});
  std::optional<std::string> flow;
  if (const JsonValue* value = raw.find("flow")) {
    const std::string text = as_string(*value, "flow");
    if (!one_of(text, {"row", "column", "stack"})) reject("unknown flow " + text);
    flow = text;
  }
  if (const JsonValue* value = raw.find("align")) {
    const std::string text = as_string(*value, "align");
    if (!one_of(text, {"start", "center", "end", "stretch"})) reject("unknown align " + text);
  }
  if (const JsonValue* value = raw.find("grow")) {
    const View::Value grow = literal(*value);
    if (grow.kind != View::Value::Kind::Int || grow.n < 0) reject("node " + id + " layout.grow must be an integer >= 0");
  }
  return flow;
}

inline void check_tree(const std::unordered_map<std::string, View::Node>& nodes, const std::string& root) {
  std::unordered_map<std::string, int> state;
  const auto walk = [&](auto&& self, const std::string& id) -> void {
    const int mark = state[id];
    if (mark == 1) reject("children cycle at " + id);
    if (mark == 2) reject("node " + id + " has more than one parent");
    state[id] = 1;
    const auto found = nodes.find(id);
    if (found == nodes.end()) reject("children name unknown node " + id);
    for (const auto& child : found->second.children) {
      if (!nodes.count(child)) reject("children name unknown node " + child);
      self(self, child);
    }
    state[id] = 2;
  };
  walk(walk, root);
  if (state.size() != nodes.size()) reject("a node is not reachable from root");
}

inline JsonValue lit_expr(const JsonValue& value) {
  JsonValue expr = JsonValue::empty_object();
  expr.set("op", JsonValue::of_string("lit"));
  JsonValue args = JsonValue::empty_array();
  args.push(value);
  expr.set("args", std::move(args));
  return expr;
}

inline void add_transition(StateBuilder& sb, const std::string& event, const JsonValue& value) {
  if (value.is_string()) {
    sb.on(event, value.string);
    return;
  }
  if (!value.is_object()) reject("a transition must be a target id or a transition object");
  const JsonValue* target = value.find("target");
  if (target == nullptr || !target->is_string()) reject("transition object has no target");
  TransitionBuilder tb = TransitionBuilder::to(target->string);
  if (const JsonValue* guard = value.find("guard")) {
    if (!guard->is_string()) reject("transition guard must be a name");
    tb.guard(guard->string);
  }
  if (const JsonValue* action = value.find("action")) {
    if (!action->is_array()) reject("transition action must be an array");
    for (const auto& act : action->array) {
      if (!act.is_string()) reject("transition action must be a string");
      tb.action(act.string);
    }
  }
  if (const JsonValue* internal = value.find("internal")) {
    if (!internal->is_bool()) reject("`internal` must be a boolean");
    if (internal->boolean) tb.internal();
  }
  sb.on_transition(event, std::move(tb));
}

inline StateBuilder build_state(const std::string& id, const JsonValue& def) {
  if (!def.is_object()) reject("state definition is not an object");
  const bool parallel = def.find("parallel") && def.find("parallel")->is_bool() && def.find("parallel")->boolean;
  const JsonValue* history = def.find("history");
  const JsonValue* initial = def.find("initial");
  StateBuilder sb = StateBuilder::atomic(id);
  if (parallel) {
    sb = StateBuilder::parallel(id);
  } else if (history != nullptr) {
    if (!history->is_string()) reject("`history` must be a spelling");
    sb = history->string == "deep" ? StateBuilder::history_deep(id) : StateBuilder::history_shallow(id);
    const JsonValue* fallback = def.find("default");
    if (fallback == nullptr || !fallback->is_string()) reject("a history pseudo-state needs a default target");
    sb.default_child(fallback->string);
  } else if (initial != nullptr) {
    if (!initial->is_string()) reject("`initial` must be a state id");
    sb = StateBuilder::compound(id, initial->string);
  }
  if (const JsonValue* parent = def.find("parent")) sb.parent(as_string(*parent, "parent"));
  if (const JsonValue* on = def.find("on")) {
    if (!on->is_object()) reject("`on` must be an event map");
    for (const auto& kv : on->object) add_transition(sb, kv.first, kv.second);
  }
  return sb;
}

inline ChartDef build_chart(const JsonValue& chart) {
  if (!chart.is_object()) reject("chart must be a statechart document");
  const JsonValue* states = chart.find("states");
  if (states == nullptr || !states->is_object()) reject("chart has no states object");
  ChartBuilder builder;
  for (const auto& kv : states->object) builder.state(build_state(kv.first, kv.second));
  auto def = builder.build();
  if (!def) reject("the chart does not build");
  return std::move(*def);
}

}  // namespace view_detail

inline void View::Impl::populate(Context& context, const JsonValue& doc) {
  using namespace view_detail;
  ctx = &context;
  allow(doc, {"id", "root", "signals", "nodes", "chart"});
  const std::string id = as_string(need(doc, "id"), "view id");
  if (id.empty()) reject("view id must be a non-empty string");
  root = as_string(need(doc, "root"), "view root");
  if (root.empty()) reject("view root must be a non-empty string");
  const JsonValue& signals_doc = need(doc, "signals");
  if (!signals_doc.is_object()) reject("view signals must be an object");
  const JsonValue& nodes_doc = need(doc, "nodes");
  if (!nodes_doc.is_object() || nodes_doc.object.empty()) reject("view nodes must contain at least one node");
  const bool has_chart = doc.find("chart") != nullptr;
  if (has_chart && !doc.find("chart")->is_object()) reject("chart must be a statechart document");

  std::unordered_map<std::string, Value> initials;
  for (const auto& kv : signals_doc.object) {
    if (kv.first.empty()) reject("signal id must be a non-empty string");
    allow(kv.second, {"initial"});
    initials.emplace(kv.first, literal(need(kv.second, "initial")));
  }
  struct ExprDoc {
    std::string key;
    JsonValue expr;
  };
  std::vector<ExprDoc> expr_docs;
  for (const auto& kv : nodes_doc.object) {
    if (kv.first.empty()) reject("node id must be a non-empty string");
    const JsonValue& node = kv.second;
    allow(node, {"role", "children", "layout", "text", "bind", "hidden", "disabled", "effects"});
    const std::string role = as_string(need(node, "role"), "role");
    if (!one_of(role, {"text", "button", "field", "list", "region"})) reject("unknown role " + role);
    Node stored;
    stored.role = role;
    if (const JsonValue* children = node.find("children")) stored.children = children_of(kv.first, *children);
    if (const JsonValue* layout = node.find("layout")) stored.flow = layout_of(kv.first, *layout);
    if (const JsonValue* bind = node.find("bind")) {
      const std::string name = as_string(*bind, "bind");
      if (name.empty() || !initials.count(name)) reject("bind names unknown signal " + name);
    }
    auto standing = [&](const char* field, const JsonValue& raw, bool text) {
      JsonValue expr;
      if (text && raw.is_string()) expr = lit_expr(raw);
      else if (!text && raw.is_bool()) expr = lit_expr(raw);
      else if (raw.is_object()) {
        validate_expr(raw, initials, false);
        expr = raw;
      } else {
        reject(std::string(field) + " must be a literal or an expression");
      }
      expr_docs.push_back({std::string(kv.first) + "." + field, std::move(expr)});
    };
    if (const JsonValue* text = node.find("text")) standing("text", *text, true);
    if (const JsonValue* hidden = node.find("hidden")) standing("hidden", *hidden, false);
    if (const JsonValue* disabled = node.find("disabled")) standing("disabled", *disabled, false);
    if (const JsonValue* effects_doc = node.find("effects")) {
      allow(*effects_doc, {"activate", "change", "submit"});
      for (const auto& ev : effects_doc->object) validate_effect(ev.second, initials, has_chart);
      if (!effects_doc->object.empty()) effects.emplace(kv.first, *effects_doc);
    }
    nodes.emplace(kv.first, std::move(stored));
  }
  if (!nodes.count(root)) reject("root " + root + " is not a node");
  check_tree(nodes, root);

  for (const auto& kv : initials) signals.emplace(kv.first, context.source(kv.second));
  for (const auto& doc_expr : expr_docs) (void)eval_expr(doc_expr.expr, nullptr, false);
  auto self = shared_from_this();
  for (const auto& doc_expr : expr_docs) {
    JsonValue expr = doc_expr.expr;
    exprs.emplace(doc_expr.key, context.computed<Value>([self, expr](Compute& cv) {
                    return self->eval_expr(expr, &cv, false);
                  }));
  }
  if (has_chart) chart.emplace(context, build_chart(*doc.find("chart")));
  context.effect_void([self](Compute& cv) {
    if (!self->exprs.empty()) {
      for (const auto& kv : self->exprs) (void)cv.get(kv.second);
    } else {
      for (const auto& kv : self->signals) (void)cv.get(kv.second);
    }
  });
}

inline View::Value View::Impl::eval_arg(const JsonValue& value, Compute* cx, bool in_effect_now) {
  if (value.is_object()) return eval_expr(value, cx, in_effect_now);
  return view_detail::literal(value);
}

inline View::Value View::Impl::eval_call(const JsonArray& args, Compute* cx, bool in_effect_now) {
  using namespace view_detail;
  if (args.empty() || !args[0].is_string()) reject("call takes a name");
  const std::string& name = args[0].string;
  if (name == "event") {
    if (args.size() != 1) reject("event takes no arguments");
    if (!in_effect_now) reject("event is illegal outside an effect");
    return payload;
  }
  if (name == "not") {
    if (args.size() != 2) reject("not takes one boolean");
    const Value value = eval_arg(args[1], cx, in_effect_now);
    if (value.kind != Value::Kind::Bool) reject("not expects a boolean");
    return Value::boolean(!value.b);
  }
  if (name == "add") {
    if (args.size() < 3) reject("add takes two or more integers");
    int64_t sum = 0;
    for (size_t i = 1; i < args.size(); ++i) {
      const Value value = eval_arg(args[i], cx, in_effect_now);
      if (value.kind != Value::Kind::Int) reject("add expects integers");
      if ((value.n > 0 && sum > INT64_MAX - value.n) || (value.n < 0 && sum < INT64_MIN - value.n))
        reject("add overflow");
      sum += value.n;
    }
    return Value::integer(sum);
  }
  if (name == "len") {
    if (args.size() != 2) reject("len takes one string");
    const Value value = eval_arg(args[1], cx, in_effect_now);
    if (value.kind != Value::Kind::Str) reject("len expects a string");
    return Value::integer(utf8_len(value.s));
  }
  reject("unknown call " + name);
  return {};
}

inline View::Value View::Impl::eval_expr(const JsonValue& expr, Compute* cx, bool in_effect_now) {
  using namespace view_detail;
  allow(expr, {"op", "args"});
  const std::string op = as_string(need(expr, "op"), "op");
  if (!need(expr, "args").is_array()) reject("expression args must be an array");
  const auto& args = expr.find("args")->array;
  if (op == "lit") {
    if (args.size() != 1) reject("lit takes one literal");
    return literal(args[0]);
  }
  if (op == "ref") {
    if (args.size() != 1 || !args[0].is_string()) reject("ref takes one signal id");
    const auto found = signals.find(args[0].string);
    if (found == signals.end()) reject("unknown signal " + args[0].string);
    return cx == nullptr ? ctx->get(found->second) : cx->get(found->second);
  }
  if (op == "call") return eval_call(args, cx, in_effect_now);
  if (op == "if") {
    if (args.size() != 3) reject("if takes three arguments");
    const Value cond = eval_arg(args[0], cx, in_effect_now);
    if (cond.kind != Value::Kind::Bool) reject("if condition must be a boolean");
    return eval_arg(cond.b ? args[1] : args[2], cx, in_effect_now);
  }
  if (op == "eq") {
    if (args.size() != 2) reject("eq takes two arguments");
    const Value left = eval_arg(args[0], cx, in_effect_now);
    const Value right = eval_arg(args[1], cx, in_effect_now);
    return Value::boolean(left == right);
  }
  if (op == "concat") {
    std::string out;
    for (const auto& arg : args) out += stringify(eval_arg(arg, cx, in_effect_now));
    return Value::str(std::move(out));
  }
  if (op == "trim") {
    if (args.size() != 1) reject("trim takes one value");
    return Value::str(trim_copy(stringify(eval_arg(args[0], cx, in_effect_now))));
  }
  reject("unknown expression op " + op);
  return {};
}

inline void View::Impl::exec(const JsonValue& effect, JsonValue& trace) {
  using namespace view_detail;
  const std::string op = as_string(need(effect, "op"), "op");
  if (!need(effect, "args").is_array()) reject("effect args must be an array");
  const auto& args = effect.find("args")->array;
  if (op == "set") {
    if (args.size() != 2 || !args[0].is_string()) reject("set takes a signal id and a value");
    const auto found = signals.find(args[0].string);
    if (found == signals.end()) reject("unknown signal " + args[0].string);
    const Value value = eval_arg(args[1], nullptr, true);
    ctx->set(found->second, value);
    JsonValue record = JsonValue::empty_object();
    record.set("op", JsonValue::of_string("set"));
    record.set("signal", JsonValue::of_string(args[0].string));
    record.set("value", value.json());
    trace.push(std::move(record));
    return;
  }
  if (op == "send") {
    if (!chart) reject("send requires a chart");
    if ((args.size() != 1 && args.size() != 2) || !args[0].is_string())
      reject("send takes an event name and an optional payload");
    JsonValue record = JsonValue::empty_object();
    record.set("op", JsonValue::of_string("send"));
    record.set("event", JsonValue::of_string(args[0].string));
    if (args.size() == 2) record.set("payload", eval_arg(args[1], nullptr, true).json());
    chart->send(*ctx, args[0].string, {});
    trace.push(std::move(record));
    return;
  }
  if (op == "batch") {
    if (args.empty()) reject("batch takes one or more effects");
    for (const auto& nested : args) exec(nested, trace);
    return;
  }
  reject("unknown effect op " + op);
}

inline void View::Impl::walk(JsonValue& out, const std::string& id) const {
  const Node& node = nodes.at(id);
  JsonValue kids = JsonValue::empty_array();
  for (const auto& child : node.children) kids.push(JsonValue::of_string(child));
  JsonValue row = JsonValue::empty_object();
  row.set("id", JsonValue::of_string(id));
  row.set("role", JsonValue::of_string(node.role));
  row.set("children", std::move(kids));
  row.set("flow", node.flow ? JsonValue::of_string(*node.flow) : JsonValue::null());
  out.push(std::move(row));
  for (const auto& child : node.children) walk(out, child);
}

inline View View::install(Context& ctx, const JsonValue& doc) {
  auto impl = std::make_shared<Impl>();
  impl->populate(ctx, doc);
  return View(std::move(impl));
}

inline JsonValue View::signals_json() const {
  JsonValue out = JsonValue::empty_object();
  for (const auto& kv : impl_->signals) out.set(kv.first, impl_->ctx->get(kv.second).json());
  return out;
}

inline JsonValue View::expressions_json() const {
  JsonValue out = JsonValue::empty_object();
  for (const auto& kv : impl_->exprs) out.set(kv.first, impl_->ctx->get(kv.second).json());
  return out;
}

inline JsonValue View::layout_json() const {
  JsonValue out = JsonValue::empty_array();
  impl_->walk(out, impl_->root);
  return out;
}

inline JsonValue View::chart_json() const {
  if (!impl_->chart) return JsonValue::null();
  JsonValue out = JsonValue::empty_array();
  for (const auto& leaf : impl_->chart->active_leaves(*impl_->ctx)) out.push(JsonValue::of_string(leaf));
  return out;
}

inline JsonValue View::dispatch(const std::string& node, const std::string& event, const JsonValue& value) {
  using namespace view_detail;
  if (!one_of(event, {"activate", "change", "submit"})) reject("unknown event " + event);
  if (!impl_->nodes.count(node)) reject("unknown node " + node);
  JsonValue trace = JsonValue::empty_array();
  const auto found = impl_->effects.find(node);
  if (found == impl_->effects.end() || found->second.find(event) == nullptr) return trace;
  impl_->payload = literal(value);
  impl_->in_effect = true;
  impl_->exec(*found->second.find(event), trace);
  impl_->in_effect = false;
  return trace;
}

}  // namespace lazily

#endif
