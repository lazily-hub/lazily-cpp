#ifndef LAZILY_GRAPH_IR_HPP
#define LAZILY_GRAPH_IR_HPP

// Graph IR interpreter. A document parses into sources, computeds, and effects.
// Canonical source is emitted from the document, not from live values.

#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lazily {

class GraphIrError : public std::runtime_error {
 public:
  explicit GraphIrError(const std::string& message) : std::runtime_error(message) {}
};

struct Edge {
  std::string from;
  std::string kind;
  std::string to;

  bool operator==(const Edge& other) const {
    return from == other.from && kind == other.kind && to == other.to;
  }
};

inline constexpr const char* kPolicyKeepLatest = "keep-latest";
inline constexpr const char* kPolicySum = "sum";
inline constexpr const char* kPolicyMax = "max";
inline constexpr const char* kPolicyMin = "min";
inline constexpr const char* kPolicyAnd = "and";
inline constexpr const char* kPolicyOr = "or";
inline constexpr const char* kEdgeRead = "read";
inline constexpr const char* kEdgeWrite = "write";
inline constexpr int kGraphDrainBudget = 100000;

inline bool graph_json_eq(const JsonValue& left, const JsonValue& right);
inline std::string graph_canonical_json(const JsonValue& value);

inline JsonValue graph_canon(const JsonValue& value) {
  if (value.type == JsonValue::Type::Array) {
    JsonArray array;
    array.reserve(value.array.size());
    for (const auto& item : value.array) array.push_back(graph_canon(item));
    return JsonValue::of_array(std::move(array));
  }
  if (value.type == JsonValue::Type::Object) {
    auto entries = value.object;
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    JsonObject object;
    object.reserve(entries.size());
    for (auto& entry : entries) object.emplace_back(entry.first, graph_canon(entry.second));
    return JsonValue::of_object(std::move(object));
  }
  return value;
}

inline std::string graph_canonical_json(const JsonValue& value) { return json_write(graph_canon(value)); }

inline bool graph_json_eq(const JsonValue& left, const JsonValue& right) {
  if (left.type != right.type) return false;
  switch (left.type) {
  case JsonValue::Type::Null:
    return true;
  case JsonValue::Type::Bool:
    return left.boolean == right.boolean;
  case JsonValue::Type::Int:
    return left.integer == right.integer;
  case JsonValue::Type::Double:
    return left.real == right.real;
  case JsonValue::Type::String:
    return left.string == right.string;
  case JsonValue::Type::Array:
    if (left.array.size() != right.array.size()) return false;
    for (size_t i = 0; i < left.array.size(); ++i)
      if (!graph_json_eq(left.array[i], right.array[i])) return false;
    return true;
  case JsonValue::Type::Object: {
    if (left.object.size() != right.object.size()) return false;
    for (const auto& entry : left.object) {
      const JsonValue* other = right.find(entry.first);
      if (other == nullptr || !graph_json_eq(entry.second, *other)) return false;
    }
    return true;
  }
  }
  return false;
}

inline bool graph_valid_id(const std::string& text) {
  if (text.empty() || text.size() > 128) return false;
  const auto head = static_cast<unsigned char>(text[0]);
  if (!((head >= 'A' && head <= 'Z') || (head >= 'a' && head <= 'z') || head == '_')) return false;
  for (unsigned char c : text) {
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
  }
  return true;
}

inline const JsonValue* graph_field(const JsonValue& object, const std::string& key) { return object.find(key); }

inline void graph_allow(const JsonValue& object, const std::string& what, std::initializer_list<const char*> keys) {
  for (const auto& entry : object.object) {
    bool allowed = false;
    for (const char* key : keys)
      if (entry.first == key) allowed = true;
    if (!allowed) throw GraphIrError(what + ": unknown field `" + entry.first + "`");
  }
}

inline const JsonValue& graph_require(const JsonValue& object, const std::string& field, const std::string& what) {
  const JsonValue* value = object.find(field);
  if (value == nullptr) throw GraphIrError(what + " requires `" + field + "`");
  return *value;
}

inline const std::string& graph_string(const JsonValue& value, const std::string& what) {
  if (!value.is_string()) throw GraphIrError(what + " must be a string, got " + graph_canonical_json(value));
  return value.string;
}

inline const JsonValue& graph_object(const JsonValue& value, const std::string& what) {
  if (!value.is_object()) throw GraphIrError(what + " must be an object");
  return value;
}

inline bool graph_expect_bool(const JsonValue& value) {
  if (!value.is_bool()) throw GraphIrError("expected a boolean, got " + graph_canonical_json(value));
  return value.boolean;
}

inline JsonValue graph_canonical_f64(double number) {
  if (!std::isfinite(number)) throw GraphIrError("non-finite number");
  const double min_int = static_cast<double>(std::numeric_limits<int64_t>::min());
  if (number == std::trunc(number) && number >= min_int && number < 9223372036854775808.0) {
    const auto integer = static_cast<int64_t>(number);
    if (static_cast<double>(integer) == number) return JsonValue::of_int(integer);
  }
  return JsonValue::of_double(number);
}

struct GraphNum {
  bool integer = false;
  int64_t as_int = 0;
  double as_real = 0;
};

inline GraphNum graph_as_num(const JsonValue& value) {
  if (value.type == JsonValue::Type::Int) return GraphNum{true, value.integer, 0};
  if (value.type == JsonValue::Type::Double) {
    if (!std::isfinite(value.real)) throw GraphIrError("non-finite number");
    return GraphNum{false, 0, value.real};
  }
  throw GraphIrError("expected a number, got " + graph_canonical_json(value));
}

inline int64_t graph_add_int(int64_t a, int64_t b) {
  int64_t result = 0;
  if (__builtin_add_overflow(a, b, &result)) throw GraphIrError("integer overflow");
  return result;
}

inline int64_t graph_sub_int(int64_t a, int64_t b) {
  int64_t result = 0;
  if (__builtin_sub_overflow(a, b, &result)) throw GraphIrError("integer overflow");
  return result;
}

inline int64_t graph_mul_int(int64_t a, int64_t b) {
  int64_t result = 0;
  if (__builtin_mul_overflow(a, b, &result)) throw GraphIrError("integer overflow");
  return result;
}

inline int graph_ordering(const JsonValue& left, const JsonValue& right) {
  const GraphNum a = graph_as_num(left);
  const GraphNum b = graph_as_num(right);
  if (a.integer && b.integer) return (a.as_int > b.as_int) - (a.as_int < b.as_int);
  const double lf = a.integer ? static_cast<double>(a.as_int) : a.as_real;
  const double rf = b.integer ? static_cast<double>(b.as_int) : b.as_real;
  return (lf > rf) - (lf < rf);
}

inline JsonValue graph_num_bin(const std::string& op, const JsonValue& left, const JsonValue& right) {
  const GraphNum a = graph_as_num(left);
  const GraphNum b = graph_as_num(right);
  if (op != "div" && a.integer && b.integer) {
    if (op == "add") return JsonValue::of_int(graph_add_int(a.as_int, b.as_int));
    if (op == "sub") return JsonValue::of_int(graph_sub_int(a.as_int, b.as_int));
    if (op == "mul") return JsonValue::of_int(graph_mul_int(a.as_int, b.as_int));
    throw GraphIrError("op `" + op + "` is not an integer operator");
  }
  const double lf = a.integer ? static_cast<double>(a.as_int) : a.as_real;
  const double rf = b.integer ? static_cast<double>(b.as_int) : b.as_real;
  if (op == "div" && rf == 0) throw GraphIrError("division by zero");
  double value = 0;
  if (op == "add") value = lf + rf;
  else if (op == "sub") value = lf - rf;
  else if (op == "mul") value = lf * rf;
  else if (op == "div") value = lf / rf;
  else throw GraphIrError("op `" + op + "` is not a numeric operator");
  return graph_canonical_f64(value);
}

inline void graph_check_policy_init(const std::string& policy, const JsonValue& init) {
  if (policy == kPolicySum || policy == kPolicyMax || policy == kPolicyMin) graph_as_num(init);
  else if (policy == kPolicyAnd || policy == kPolicyOr) graph_expect_bool(init);
}

inline std::string graph_parse_id(const std::string& text, const std::string& what) {
  if (!graph_valid_id(text)) throw GraphIrError(what + " `" + text + "` is not a graph IR id");
  return text;
}

inline bool graph_is_unary(const std::string& op) { return op == "neg" || op == "not" || op == "len"; }

inline bool graph_is_binary(const std::string& op) {
  return op == "add" || op == "sub" || op == "mul" || op == "div" || op == "mod" || op == "min" || op == "max" ||
         op == "eq" || op == "ne" || op == "lt" || op == "le" || op == "gt" || op == "ge" || op == "concat" ||
         op == "get" || op == "and" || op == "or";
}

inline void graph_parse_expr(const JsonValue& value);
inline void graph_parse_stmt(const JsonValue& value);

inline void graph_parse_expr(const JsonValue& value) {
  graph_object(value, "expression");
  graph_allow(value, "expression", {"lit", "ref", "op", "args", "object"});
  int forms = 0;
  for (const char* key : {"lit", "ref", "op", "object"})
    if (value.find(key) != nullptr) ++forms;
  if (forms != 1) throw GraphIrError("expression must be exactly one of lit, ref, op, or object");
  if (value.find("args") != nullptr && value.find("op") == nullptr)
    throw GraphIrError("args is only valid on an op expression");
  if (value.find("lit") != nullptr) return;
  if (const JsonValue* ref = value.find("ref")) {
    graph_parse_id(graph_string(*ref, "expression `ref`"), "ref");
    return;
  }
  if (const JsonValue* object = value.find("object")) {
    if (!object->is_array()) throw GraphIrError("object expression must be an array");
    std::set<std::string> seen;
    for (const auto& item : object->array) {
      graph_object(item, "object field");
      graph_allow(item, "object field", {"key", "value"});
      const std::string key = graph_string(graph_require(item, "key", "object field"), "object field `key`");
      if (!seen.insert(key).second) throw GraphIrError("duplicate object key `" + key + "`");
      graph_parse_expr(graph_require(item, "value", "object field"));
    }
    return;
  }
  const std::string op = graph_string(graph_require(value, "op", "expression"), "expression `op`");
  if (!graph_is_unary(op) && !graph_is_binary(op) && op != "if" && op != "array")
    throw GraphIrError("unknown op `" + op + "`");
  const JsonValue& args = graph_require(value, "args", "op expression");
  if (!args.is_array()) throw GraphIrError("op args must be an array");
  const bool ok = graph_is_unary(op) ? args.array.size() == 1 : op == "if" ? args.array.size() == 3
                                                          : op == "array"   ? true
                                                                            : args.array.size() == 2;
  if (!ok) throw GraphIrError("op `" + op + "` has the wrong number of arguments");
  for (const auto& arg : args.array) graph_parse_expr(arg);
}

inline void graph_parse_stmt(const JsonValue& value) {
  graph_object(value, "statement");
  const std::string verb = graph_string(graph_require(value, "do", "statement"), "statement `do`");
  if (verb == "eval") {
    graph_allow(value, "eval", {"do", "expr"});
    graph_parse_expr(graph_require(value, "expr", "eval"));
  } else if (verb == "set" || verb == "merge") {
    graph_allow(value, verb, {"do", "target", "expr"});
    graph_parse_id(graph_string(graph_require(value, "target", verb), verb + " `target`"), "write target");
    graph_parse_expr(graph_require(value, "expr", verb));
  } else if (verb == "seq") {
    graph_allow(value, "seq", {"do", "steps"});
    const JsonValue& steps = graph_require(value, "steps", "seq");
    if (!steps.is_array()) throw GraphIrError("seq steps must be an array");
    if (steps.array.empty()) throw GraphIrError("seq steps must not be empty");
    for (const auto& step : steps.array) graph_parse_stmt(step);
  } else if (verb == "when") {
    graph_allow(value, "when", {"do", "cond", "then", "else"});
    graph_parse_expr(graph_require(value, "cond", "when"));
    graph_parse_stmt(graph_require(value, "then", "when"));
    if (const JsonValue* otherwise = value.find("else")) graph_parse_stmt(*otherwise);
  } else {
    throw GraphIrError("unknown statement `" + verb + "`");
  }
}

inline std::string graph_parse_policy(const JsonValue& object, const std::string& id) {
  const JsonValue* policy = object.find("policy");
  if (policy == nullptr) return kPolicyKeepLatest;
  if (!policy->is_string()) throw GraphIrError("source `" + id + "` policy must be a string");
  const std::string& value = policy->string;
  if (value != kPolicyKeepLatest && value != kPolicySum && value != kPolicyMax && value != kPolicyMin &&
      value != kPolicyAnd && value != kPolicyOr)
    throw GraphIrError("unknown source policy `" + value + "`");
  return value;
}

inline void graph_parse_node(const JsonValue& value) {
  graph_object(value, "node");
  const std::string kind = graph_string(graph_require(value, "kind", "node"), "node `kind`");
  const std::string id = graph_parse_id(graph_string(graph_require(value, "id", "node"), "node `id`"), "node id");
  if (kind == "source") {
    graph_allow(value, "source", {"id", "kind", "init", "policy"});
    const JsonValue& init = graph_require(value, "init", "source `" + id + "`");
    graph_check_policy_init(graph_parse_policy(value, id), init);
  } else if (kind == "computed") {
    graph_allow(value, "computed", {"id", "kind", "expr", "eager"});
    if (const JsonValue* eager = value.find("eager"))
      if (!eager->is_bool()) throw GraphIrError("computed `" + id + "` eager must be a boolean");
    graph_parse_expr(graph_require(value, "expr", "computed"));
  } else if (kind == "effect") {
    graph_allow(value, "effect", {"id", "kind", "body"});
    graph_parse_stmt(graph_require(value, "body", "effect"));
  } else {
    throw GraphIrError("unknown node kind `" + kind + "`");
  }
}

inline void graph_collect_expr(const JsonValue& expr, std::set<std::string>& reads) {
  if (const JsonValue* ref = expr.find("ref")) reads.insert(ref->string);
  if (const JsonValue* args = expr.find("args"))
    for (const auto& arg : args->array) graph_collect_expr(arg, reads);
  if (const JsonValue* object = expr.find("object"))
    for (const auto& field : object->array) graph_collect_expr(*field.find("value"), reads);
}

inline void graph_collect_stmt(const JsonValue& stmt, std::set<std::string>& reads, std::set<std::string>& writes) {
  const std::string verb = stmt.find("do")->string;
  if (verb == "eval") graph_collect_expr(*stmt.find("expr"), reads);
  else if (verb == "set" || verb == "merge") {
    writes.insert(stmt.find("target")->string);
    graph_collect_expr(*stmt.find("expr"), reads);
  } else if (verb == "seq") {
    for (const auto& step : stmt.find("steps")->array) graph_collect_stmt(step, reads, writes);
  } else if (verb == "when") {
    graph_collect_expr(*stmt.find("cond"), reads);
    graph_collect_stmt(*stmt.find("then"), reads, writes);
    if (const JsonValue* otherwise = stmt.find("else")) graph_collect_stmt(*otherwise, reads, writes);
  }
}

inline std::pair<std::vector<std::string>, std::vector<std::string>> graph_edge_sets(const JsonValue& node) {
  std::set<std::string> reads;
  std::set<std::string> writes;
  const std::string kind = node.find("kind")->string;
  if (kind == "computed") graph_collect_expr(*node.find("expr"), reads);
  if (kind == "effect") graph_collect_stmt(*node.find("body"), reads, writes);
  return {std::vector<std::string>(reads.begin(), reads.end()), std::vector<std::string>(writes.begin(), writes.end())};
}

inline std::string graph_emit_expr(const JsonValue& expr);
inline std::string graph_emit_stmt(const JsonValue& stmt);

inline std::string graph_emit_expr(const JsonValue& expr) {
  if (const JsonValue* lit = expr.find("lit")) return "lit(" + graph_canonical_json(*lit) + ")";
  if (const JsonValue* ref = expr.find("ref")) return "ref(" + ref->string + ")";
  if (const JsonValue* object = expr.find("object")) {
    std::string out = "object(";
    for (size_t i = 0; i < object->array.size(); ++i) {
      if (i != 0) out += ", ";
      out += graph_canonical_json(*object->array[i].find("key"));
      out += ": ";
      out += graph_emit_expr(*object->array[i].find("value"));
    }
    return out + ")";
  }
  std::string out = expr.find("op")->string + "(";
  const auto& args = expr.find("args")->array;
  for (size_t i = 0; i < args.size(); ++i) {
    if (i != 0) out += ", ";
    out += graph_emit_expr(args[i]);
  }
  return out + ")";
}

inline std::string graph_emit_stmt(const JsonValue& stmt) {
  const std::string verb = stmt.find("do")->string;
  if (verb == "eval") return "eval(" + graph_emit_expr(*stmt.find("expr")) + ")";
  if (verb == "set" || verb == "merge")
    return verb + "(" + stmt.find("target")->string + ", " + graph_emit_expr(*stmt.find("expr")) + ")";
  if (verb == "seq") {
    std::string out = "seq(";
    const auto& steps = stmt.find("steps")->array;
    for (size_t i = 0; i < steps.size(); ++i) {
      if (i != 0) out += ", ";
      out += graph_emit_stmt(steps[i]);
    }
    return out + ")";
  }
  std::string out = "when(" + graph_emit_expr(*stmt.find("cond")) + ", " + graph_emit_stmt(*stmt.find("then"));
  if (const JsonValue* otherwise = stmt.find("else")) out += ", " + graph_emit_stmt(*otherwise);
  return out + ")";
}

inline std::string graph_emit_node(const JsonValue& node) {
  const std::string kind = node.find("kind")->string;
  const std::string id = node.find("id")->string;
  if (kind == "source") {
    std::string out = "source " + id + " = " + graph_canonical_json(*node.find("init"));
    if (const JsonValue* policy = node.find("policy"))
      if (policy->string != kPolicyKeepLatest) out += " policy " + policy->string;
    return out;
  }
  if (kind == "computed") {
    std::string out = "computed " + id;
    if (const JsonValue* eager = node.find("eager"))
      if (eager->boolean) out += " eager";
    return out + " = " + graph_emit_expr(*node.find("expr"));
  }
  return "effect " + id + " = " + graph_emit_stmt(*node.find("body"));
}

inline size_t graph_rune_count(const std::string& text) {
  size_t count = 0;
  for (unsigned char c : text)
    if ((c & 0xC0) != 0x80) ++count;
  return count;
}

class GraphIr {
 public:
  static GraphIr from_json_str(std::string_view text) {
    JsonValue root;
    try {
      root = json_parse(text);
    } catch (const std::runtime_error& err) {
      throw GraphIrError(std::string("graph document is not JSON: ") + err.what());
    }
    return from_json(root);
  }

  const std::string* id() const { return has_id_ ? &id_ : nullptr; }

  const JsonValue* node(const std::string& node_id) const {
    const auto found = index_.find(node_id);
    return found == index_.end() ? nullptr : &nodes_[found->second];
  }

  bool node_eager(const std::string& node_id) const {
    const JsonValue* found = node(node_id);
    if (found == nullptr || found->find("kind")->string != "computed") return false;
    const JsonValue* eager = found->find("eager");
    return eager != nullptr && eager->boolean;
  }

  std::string node_expr_op(const std::string& node_id) const {
    const JsonValue* found = node(node_id);
    if (found == nullptr) return "";
    const JsonValue* expr = found->find("expr");
    if (expr == nullptr || expr->find("op") == nullptr) return "";
    return expr->find("op")->string;
  }

  std::vector<Edge> static_edges() const {
    std::vector<Edge> edges;
    for (const auto& node : nodes_) {
      const auto sets = graph_edge_sets(node);
      for (const auto& to : sets.first) edges.push_back(Edge{node.find("id")->string, kEdgeRead, to});
      for (const auto& to : sets.second) edges.push_back(Edge{node.find("id")->string, kEdgeWrite, to});
    }
    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) {
      if (a.from != b.from) return a.from < b.from;
      if (a.kind != b.kind) return a.kind < b.kind;
      return a.to < b.to;
    });
    return edges;
  }

  std::string emit_source() const {
    std::string out = has_id_ ? "graph " + id_ + " ir 1\n" : "graph ir 1\n";
    for (const auto& node : nodes_) out += graph_emit_node(node) + "\n";
    return out;
  }

  std::string to_json_text() const {
    JsonValue object = JsonValue::empty_object();
    object.set("kind", JsonValue::of_string("graph"));
    object.set("ir", JsonValue::of_string("1"));
    if (has_id_) object.set("id", JsonValue::of_string(id_));
    object.set("nodes", JsonValue::of_array(nodes_));
    return graph_canonical_json(object);
  }

 private:
  friend class GraphRuntime;
  friend class GraphEdit;

  std::string id_;
  bool has_id_ = false;
  std::vector<JsonValue> nodes_;
  std::map<std::string, size_t> index_;

  static GraphIr from_json(const JsonValue& value) {
    graph_object(value, "graph document");
    graph_allow(value, "graph document", {"kind", "ir", "id", "nodes"});
    const std::string kind = graph_string(graph_require(value, "kind", "graph document"), "graph document `kind`");
    if (kind != "graph") throw GraphIrError("graph document kind must be \"graph\", got `" + kind + "`");
    const std::string version = graph_string(graph_require(value, "ir", "graph document"), "graph document `ir`");
    if (version != "1") throw GraphIrError("unsupported graph IR version `" + version + "`");
    GraphIr doc;
    if (const JsonValue* id = value.find("id")) {
      doc.id_ = graph_parse_id(graph_string(*id, "graph id"), "graph id");
      doc.has_id_ = true;
    }
    const JsonValue& nodes = graph_require(value, "nodes", "graph document");
    if (!nodes.is_array()) throw GraphIrError("graph nodes must be an array");
    std::set<std::string> seen;
    for (const auto& item : nodes.array) {
      graph_parse_node(item);
      const std::string id = item.find("id")->string;
      if (!seen.insert(id).second) throw GraphIrError("duplicate node id `" + id + "`");
      doc.nodes_.push_back(item);
    }
    doc.reindex();
    doc.check();
    return doc;
  }

  void reindex() {
    index_.clear();
    for (size_t slot = 0; slot < nodes_.size(); ++slot) {
      const std::string id = nodes_[slot].find("id")->string;
      if (index_.count(id) != 0) throw GraphIrError("duplicate node id `" + id + "`");
      index_[id] = slot;
    }
  }

  void check() const {
    for (const auto& node : nodes_) {
      const auto sets = graph_edge_sets(node);
      const std::string id = node.find("id")->string;
      for (const auto& to : sets.first) {
        const JsonValue* target = this->node(to);
        if (target == nullptr) throw GraphIrError("node `" + id + "` reads unknown node `" + to + "`");
        if (target->find("kind")->string == "effect")
          throw GraphIrError("node `" + id + "` reads effect `" + to + "`, which has no value");
      }
      for (const auto& to : sets.second) {
        const JsonValue* target = this->node(to);
        if (target == nullptr) throw GraphIrError("node `" + id + "` writes unknown node `" + to + "`");
        if (target->find("kind")->string != "source")
          throw GraphIrError("node `" + id + "` writes non-source `" + to + "`");
      }
    }
    std::map<std::string, int> color;
    std::vector<std::string> stack;
    const auto visit = [&](auto&& self, const std::string& id) -> void {
      if (color[id] == 1) {
        auto start = std::find(stack.begin(), stack.end(), id);
        std::string path;
        for (auto it = start; it != stack.end(); ++it) {
          if (!path.empty()) path += " -> ";
          path += *it;
        }
        if (!path.empty()) path += " -> ";
        path += id;
        throw GraphIrError("cycle in read edges: " + path);
      }
      if (color[id] == 2) return;
      color[id] = 1;
      stack.push_back(id);
      if (const JsonValue* target = this->node(id))
        for (const auto& dep : graph_edge_sets(*target).first)
          if (this->node(dep) != nullptr) self(self, dep);
      stack.pop_back();
      color[id] = 2;
    };
    for (const auto& node : nodes_) visit(visit, node.find("id")->string);
  }

  void apply_op(const JsonValue& op) {
    const std::string name = op.find("op")->string;
    if (name == "add" || name == "replace") {
      const JsonValue& node = *op.find("node");
      const std::string id = node.find("id")->string;
      if (name == "add") {
        if (index_.count(id) != 0) throw GraphIrError("add requires a new id, `" + id + "` already exists");
        nodes_.push_back(node);
      } else {
        const auto found = index_.find(id);
        if (found == index_.end()) throw GraphIrError("replace requires an existing node `" + id + "`");
        nodes_[found->second] = node;
      }
    } else {
      const std::string id = op.find("id")->string;
      const auto found = index_.find(id);
      if (found == index_.end()) throw GraphIrError("remove requires an existing node `" + id + "`");
      nodes_.erase(nodes_.begin() + static_cast<std::ptrdiff_t>(found->second));
    }
    reindex();
    check();
  }
};

class GraphEdit {
 public:
  static GraphEdit from_json_str(std::string_view text) {
    JsonValue root;
    try {
      root = json_parse(text);
    } catch (const std::runtime_error& err) {
      throw GraphIrError(std::string("graph edit is not JSON: ") + err.what());
    }
    graph_object(root, "graph edit");
    graph_allow(root, "graph edit", {"kind", "ir", "ops"});
    const std::string kind = graph_string(graph_require(root, "kind", "graph edit"), "graph edit `kind`");
    if (kind != "edit") throw GraphIrError("graph edit kind must be \"edit\", got `" + kind + "`");
    const std::string version = graph_string(graph_require(root, "ir", "graph edit"), "graph edit `ir`");
    if (version != "1") throw GraphIrError("unsupported graph IR version `" + version + "`");
    const JsonValue& ops = graph_require(root, "ops", "graph edit");
    if (!ops.is_array()) throw GraphIrError("graph edit ops must be an array");
    if (ops.array.empty()) throw GraphIrError("graph edit ops must not be empty");
    GraphEdit edit;
    for (const auto& item : ops.array) {
      graph_object(item, "edit op");
      const std::string name = graph_string(graph_require(item, "op", "edit op"), "edit op `op`");
      if (name == "add" || name == "replace") {
        graph_allow(item, name, {"op", "node"});
        graph_parse_node(graph_require(item, "node", name));
      } else if (name == "remove") {
        graph_allow(item, "remove", {"op", "id"});
        graph_parse_id(graph_string(graph_require(item, "id", "remove"), "remove `id`"), "remove id");
      } else {
        throw GraphIrError("unknown edit op `" + name + "`");
      }
      edit.ops_.push_back(item);
    }
    return edit;
  }

 private:
  friend class GraphRuntime;
  std::vector<JsonValue> ops_;
};

struct GraphEffectRun {
  std::string id;
  JsonValue result;
};

class GraphRuntime {
 public:
  static GraphRuntime load_json(std::string_view text, int drain_budget = 0) {
    return lower(GraphIr::from_json_str(text), drain_budget);
  }

  static GraphRuntime lower(const GraphIr& doc, int drain_budget = 0) {
    doc.check();
    GraphRuntime runtime(doc, drain_budget > 0 ? drain_budget : kGraphDrainBudget);
    runtime.install_document();
    return runtime;
  }

  bool contains(const std::string& id) const { return doc_.node(id) != nullptr; }

  bool is_eager(const std::string& id) const {
    const JsonValue* node = require_node(id);
    if (node->find("kind")->string != "computed") throw GraphIrError("`" + id + "` is not a computed");
    const JsonValue* eager = node->find("eager");
    return eager != nullptr && eager->boolean;
  }

  std::string source_policy(const std::string& id) const {
    const JsonValue* node = require_node(id);
    if (node->find("kind")->string != "source") throw GraphIrError("`" + id + "` is not a source");
    return graph_parse_policy(*node, id);
  }

  std::vector<Edge> static_edges() const { return doc_.static_edges(); }
  std::string emit_source() const { return doc_.emit_source(); }

  std::vector<std::string> observed_reads(const std::string& id) const {
    const auto found = observed_.find(id);
    if (found == observed_.end()) return {};
    return {found->second.begin(), found->second.end()};
  }

  const std::vector<GraphEffectRun>& effect_trace() const { return trace_; }

  JsonValue get(const std::string& id) {
    if (effects_.count(id) != 0 && sources_.count(id) == 0 && computeds_.count(id) == 0)
      throw GraphIrError("`" + id + "` is an effect and has no value");
    const auto source = sources_.find(id);
    if (source != sources_.end()) return source->second.value;
    const auto computed = computeds_.find(id);
    if (computed == computeds_.end()) throw GraphIrError("unknown node `" + id + "`");
    if (computed->second.dirty || !computed->second.has_value) recompute(id);
    return computeds_.at(id).cache;
  }

  void write_source(const std::string& id, JsonValue value) {
    set_source(id, std::move(value));
    drain();
    raise_fault();
  }

  void apply(const GraphEdit& edit) {
    for (size_t index = 0; index < edit.ops_.size(); ++index) {
      GraphIr next = doc_;
      try {
        next.apply_op(edit.ops_[index]);
      } catch (const GraphIrError& err) {
        throw GraphIrError("edit op " + std::to_string(index) + ": " + err.what());
      }
      try {
        install_op(edit.ops_[index]);
      } catch (const GraphIrError& err) {
        throw GraphIrError("edit op " + std::to_string(index) + ": " + err.what());
      }
      doc_ = std::move(next);
    }
  }

  std::optional<int> last_drain_exhaustion() const { return last_drain_exhaustion_; }
  int drain_budget() const { return drain_budget_; }

 private:
  struct SourceState {
    JsonValue value;
    std::string policy;
  };
  struct ComputedState {
    JsonValue expr;
    bool eager = false;
    JsonValue cache;
    bool has_value = false;
    bool dirty = true;
  };
  struct EffectState {
    JsonValue body;
  };
  struct Snap {
    std::string kind;
    SourceState source;
    ComputedState computed;
    EffectState effect;
    std::set<std::string> deps;
    bool present = false;
  };

  GraphIr doc_;
  int drain_budget_ = kGraphDrainBudget;
  std::optional<int> last_drain_exhaustion_;
  std::map<std::string, SourceState> sources_;
  std::map<std::string, ComputedState> computeds_;
  std::map<std::string, EffectState> effects_;
  std::map<std::string, std::set<std::string>> deps_;
  std::map<std::string, std::set<std::string>> subs_;
  std::map<std::string, std::set<std::string>> observed_;
  std::vector<GraphEffectRun> trace_;
  std::vector<std::set<std::string>> frames_;
  std::vector<std::optional<std::string>> stack_;
  std::set<std::string> computing_;
  std::deque<std::string> scheduled_;
  std::set<std::string> scheduled_set_;
  std::optional<std::string> current_;
  bool draining_ = false;
  std::optional<std::string> fault_;

  GraphRuntime(GraphIr doc, int drain_budget) : doc_(std::move(doc)), drain_budget_(drain_budget) {}

  const JsonValue* require_node(const std::string& id) const {
    const JsonValue* node = doc_.node(id);
    if (node == nullptr) throw GraphIrError("unknown node `" + id + "`");
    return node;
  }

  void install_document() {
    for (const auto& node : doc_.nodes_)
      if (node.find("kind")->string == "source") insert_source(node);
    for (const auto& node : doc_.nodes_)
      if (node.find("kind")->string == "computed") insert_computed(node);
    for (const auto& node : doc_.nodes_)
      if (node.find("kind")->string == "computed" && node.find("eager") != nullptr && node.find("eager")->boolean)
        recompute(node.find("id")->string);
    for (const auto& node : doc_.nodes_)
      if (node.find("kind")->string == "effect") insert_effect(node);
    raise_fault();
  }

  void install_op(const JsonValue& op) {
    const std::string name = op.find("op")->string;
    if (name == "add") add_node(*op.find("node"));
    else if (name == "replace") replace_node(*op.find("node"));
    else remove_node(op.find("id")->string);
    drain();
    raise_fault();
  }

  void add_node(const JsonValue& node) {
    const std::string kind = node.find("kind")->string;
    const std::string id = node.find("id")->string;
    if (kind == "source") insert_source(node);
    else if (kind == "computed") {
      insert_computed(node);
      if (node.find("eager") != nullptr && node.find("eager")->boolean) {
        try {
          recompute(id);
        } catch (const GraphIrError&) {
          drop_binding(id);
          throw;
        }
      }
    } else {
      insert_effect(node);
    }
  }

  void replace_node(const JsonValue& node) {
    const std::string id = node.find("id")->string;
    Snap snap = snapshot(id);
    drop_binding(id);
    try {
      add_node(node);
      notify(id);
    } catch (const GraphIrError&) {
      drop_binding(id);
      restore(id, snap);
      throw;
    }
  }

  void remove_node(const std::string& id) {
    if (sources_.count(id) == 0 && computeds_.count(id) == 0 && effects_.count(id) == 0)
      throw GraphIrError("unknown node `" + id + "`");
    drop_binding(id);
    subs_.erase(id);
  }

  void insert_source(const JsonValue& node) {
    const std::string id = node.find("id")->string;
    const std::string policy = graph_parse_policy(node, id);
    graph_check_policy_init(policy, *node.find("init"));
    sources_[id] = SourceState{*node.find("init"), policy};
  }

  void insert_computed(const JsonValue& node) {
    const std::string id = node.find("id")->string;
    ComputedState state;
    state.expr = *node.find("expr");
    state.eager = node.find("eager") != nullptr && node.find("eager")->boolean;
    computeds_[id] = std::move(state);
    deps_[id] = {};
  }

  void insert_effect(const JsonValue& node) {
    const std::string id = node.find("id")->string;
    effects_[id] = EffectState{*node.find("body")};
    deps_[id] = {};
    try {
      run_effect(id);
    } catch (const GraphIrError&) {
      drop_binding(id);
      throw;
    }
  }

  Snap snapshot(const std::string& id) const {
    Snap snap;
    const auto source = sources_.find(id);
    if (source != sources_.end()) {
      snap.present = true;
      snap.kind = "source";
      snap.source = source->second;
      return snap;
    }
    const auto computed = computeds_.find(id);
    if (computed != computeds_.end()) {
      snap.present = true;
      snap.kind = "computed";
      snap.computed = computed->second;
      const auto deps = deps_.find(id);
      if (deps != deps_.end()) snap.deps = deps->second;
      return snap;
    }
    const auto effect = effects_.find(id);
    if (effect != effects_.end()) {
      snap.present = true;
      snap.kind = "effect";
      snap.effect = effect->second;
      const auto deps = deps_.find(id);
      if (deps != deps_.end()) snap.deps = deps->second;
    }
    return snap;
  }

  void restore(const std::string& id, const Snap& snap) {
    if (!snap.present) return;
    if (snap.kind == "source") sources_[id] = snap.source;
    else if (snap.kind == "computed") {
      computeds_[id] = snap.computed;
      deps_[id] = {};
      for (const auto& dep : snap.deps) {
        subs_[dep].insert(id);
        deps_[id].insert(dep);
      }
    } else if (snap.kind == "effect") {
      effects_[id] = snap.effect;
      deps_[id] = {};
      for (const auto& dep : snap.deps) {
        subs_[dep].insert(id);
        deps_[id].insert(dep);
      }
    }
  }

  void drop_binding(const std::string& id) {
    sources_.erase(id);
    computeds_.erase(id);
    effects_.erase(id);
    const auto deps = deps_.find(id);
    if (deps != deps_.end())
      for (const auto& dep : deps->second) {
        const auto readers = subs_.find(dep);
        if (readers != subs_.end()) readers->second.erase(id);
      }
    deps_.erase(id);
    observed_.erase(id);
    scheduled_set_.erase(id);
  }

  JsonValue set_source(const std::string& id, JsonValue value) {
    const auto found = sources_.find(id);
    if (found == sources_.end()) {
      if (computeds_.count(id) != 0 || effects_.count(id) != 0) throw GraphIrError("`" + id + "` is not a source");
      throw GraphIrError("unknown node `" + id + "`");
    }
    const bool changed = !graph_json_eq(found->second.value, value);
    found->second.value = std::move(value);
    if (changed) notify(id);
    return found->second.value;
  }

  JsonValue merge_source(const std::string& id, const JsonValue& value) {
    const auto found = sources_.find(id);
    if (found == sources_.end()) {
      if (computeds_.count(id) != 0 || effects_.count(id) != 0) throw GraphIrError("`" + id + "` is not a source");
      throw GraphIrError("unknown node `" + id + "`");
    }
    return set_source(id, fold_policy(found->second.policy, found->second.value, value));
  }

  void notify(const std::string& id) {
    const auto found = subs_.find(id);
    if (found == subs_.end()) return;
    const std::vector<std::string> readers(found->second.begin(), found->second.end());
    for (const auto& dep : readers) {
      const auto computed = computeds_.find(dep);
      if (computed != computeds_.end()) {
        const auto down = subs_.find(dep);
        if (computed->second.eager || (down != subs_.end() && !down->second.empty())) {
          if (computing_.count(dep) != 0) {
            computed->second.dirty = true;
            continue;
          }
          recompute(dep);
        } else {
          computed->second.dirty = true;
        }
      } else if (effects_.count(dep) != 0) {
        schedule(dep);
      }
    }
  }

  void schedule(const std::string& id) {
    if (effects_.count(id) != 0 && scheduled_set_.insert(id).second) scheduled_.push_back(id);
  }

  void drain() {
    if (draining_) return;
    draining_ = true;
    struct Reset {
      bool& bit;
      ~Reset() { bit = false; }
    } reset{draining_};
    int iterations = 0;
    while (!scheduled_.empty()) {
      const std::string id = scheduled_.front();
      scheduled_.pop_front();
      scheduled_set_.erase(id);
      ++iterations;
      if (iterations >= drain_budget_) {
        last_drain_exhaustion_ = iterations;
        return;
      }
      if (effects_.count(id) == 0) continue;
      try {
        run_effect(id);
      } catch (const GraphIrError& err) {
        fault_ = err.what();
        return;
      }
    }
  }

  void raise_fault() {
    if (!fault_) return;
    const std::string message = *fault_;
    fault_.reset();
    throw GraphIrError(message);
  }

  void run_effect(const std::string& id) {
    begin(id);
    JsonValue result;
    try {
      result = eval_stmt(effects_.at(id).body);
    } catch (...) {
      end(id);
      throw;
    }
    end(id);
    trace_.push_back(GraphEffectRun{id, result});
    drain();
  }

  JsonValue recompute(const std::string& id) {
    ComputedState& state = computeds_.at(id);
    if (!computing_.insert(id).second) throw GraphIrError("cycle in read edges: " + id);
    try {
      begin(id);
      JsonValue value;
      try {
        value = eval_expr(state.expr);
      } catch (...) {
        end(id);
        throw;
      }
      end(id);
      const bool changed = !state.has_value || !graph_json_eq(state.cache, value);
      state.cache = std::move(value);
      state.has_value = true;
      state.dirty = false;
      if (changed) notify(id);
      computing_.erase(id);
      return state.cache;
    } catch (const GraphIrError&) {
      state.dirty = true;
      state.has_value = false;
      state.cache = JsonValue::null();
      computing_.erase(id);
      throw;
    } catch (...) {
      computing_.erase(id);
      throw;
    }
  }

  void begin(const std::string& id) {
    const auto previous = deps_.find(id);
    if (previous != deps_.end())
      for (const auto& dep : previous->second) {
        const auto readers = subs_.find(dep);
        if (readers != subs_.end()) readers->second.erase(id);
      }
    deps_[id] = {};
    frames_.push_back({});
    stack_.push_back(current_);
    current_ = id;
  }

  void end(const std::string& id) {
    observed_[id] = frames_.back();
    frames_.pop_back();
    current_ = stack_.back();
    stack_.pop_back();
  }

  void note(const std::string& id) {
    if (frames_.empty() || !current_) throw GraphIrError("read outside a node evaluation");
    frames_.back().insert(id);
    subs_[id].insert(*current_);
    deps_[*current_].insert(id);
  }

  JsonValue read_ref(const std::string& id) {
    note(id);
    const auto source = sources_.find(id);
    if (source != sources_.end()) return source->second.value;
    const auto computed = computeds_.find(id);
    if (computed != computeds_.end()) {
      if (computed->second.dirty || !computed->second.has_value) return recompute(id);
      return computed->second.cache;
    }
    if (effects_.count(id) != 0) throw GraphIrError("`" + id + "` is an effect and has no value");
    throw GraphIrError("unknown node `" + id + "`");
  }

  JsonValue eval_expr(const JsonValue& expr) {
    if (const JsonValue* lit = expr.find("lit")) return *lit;
    if (const JsonValue* ref = expr.find("ref")) return read_ref(ref->string);
    if (const JsonValue* object = expr.find("object")) {
      JsonValue result = JsonValue::empty_object();
      std::set<std::string> seen;
      for (const auto& field : object->array) {
        const std::string key = field.find("key")->string;
        if (!seen.insert(key).second) throw GraphIrError("duplicate object key `" + key + "`");
        result.set(key, eval_expr(*field.find("value")));
      }
      return result;
    }
    const std::string op = expr.find("op")->string;
    const auto& args = expr.find("args")->array;
    if (op == "and") {
      if (!graph_expect_bool(eval_expr(args[0]))) return JsonValue::of_bool(false);
      return JsonValue::of_bool(graph_expect_bool(eval_expr(args[1])));
    }
    if (op == "or") {
      if (graph_expect_bool(eval_expr(args[0]))) return JsonValue::of_bool(true);
      return JsonValue::of_bool(graph_expect_bool(eval_expr(args[1])));
    }
    if (op == "if") return eval_expr(graph_expect_bool(eval_expr(args[0])) ? args[1] : args[2]);
    if (op == "array") {
      JsonArray array;
      for (const auto& arg : args) array.push_back(eval_expr(arg));
      return JsonValue::of_array(std::move(array));
    }
    std::vector<JsonValue> values;
    values.reserve(args.size());
    for (const auto& arg : args) values.push_back(eval_expr(arg));
    return eval_values(op, values);
  }

  JsonValue eval_values(const std::string& op, const std::vector<JsonValue>& values) {
    const auto one = [&]() -> const JsonValue& {
      if (values.size() != 1) throw GraphIrError("op `" + op + "` has the wrong number of arguments");
      return values[0];
    };
    const auto pair = [&]() -> std::pair<const JsonValue&, const JsonValue&> {
      if (values.size() != 2) throw GraphIrError("op `" + op + "` has the wrong number of arguments");
      return {values[0], values[1]};
    };
    if (op == "neg") {
      const GraphNum num = graph_as_num(one());
      if (num.integer) {
        if (num.as_int == std::numeric_limits<int64_t>::min()) throw GraphIrError("integer overflow");
        return JsonValue::of_int(-num.as_int);
      }
      return graph_canonical_f64(-num.as_real);
    }
    if (op == "not") return JsonValue::of_bool(!graph_expect_bool(one()));
    if (op == "len") return JsonValue::of_int(static_cast<int64_t>(len_value(one())));
    if (op == "add" || op == "sub" || op == "mul" || op == "div") {
      const auto sides = pair();
      return graph_num_bin(op, sides.first, sides.second);
    }
    if (op == "mod") {
      const auto sides = pair();
      const GraphNum a = graph_as_num(sides.first);
      const GraphNum b = graph_as_num(sides.second);
      if (!a.integer || !b.integer) throw GraphIrError("mod requires integers");
      if (b.as_int == 0) throw GraphIrError("division by zero");
      return JsonValue::of_int(a.as_int % b.as_int);
    }
    if (op == "min" || op == "max") {
      const auto sides = pair();
      const int order = graph_ordering(sides.first, sides.second);
      if (op == "max") return order >= 0 ? sides.first : sides.second;
      return order <= 0 ? sides.first : sides.second;
    }
    if (op == "eq" || op == "ne") {
      const auto sides = pair();
      const bool equal = graph_json_eq(sides.first, sides.second);
      return JsonValue::of_bool(op == "ne" ? !equal : equal);
    }
    if (op == "lt" || op == "le" || op == "gt" || op == "ge") {
      const auto sides = pair();
      const int order = graph_ordering(sides.first, sides.second);
      const bool bit = op == "lt" ? order < 0 : op == "le" ? order <= 0 : op == "gt" ? order > 0 : order >= 0;
      return JsonValue::of_bool(bit);
    }
    if (op == "concat") {
      const auto sides = pair();
      if (!sides.first.is_string() || !sides.second.is_string())
        throw GraphIrError("concat requires strings, got " + graph_canonical_json(sides.first) + " and " +
                           graph_canonical_json(sides.second));
      return JsonValue::of_string(sides.first.string + sides.second.string);
    }
    if (op == "get") {
      const auto sides = pair();
      return eval_get(sides.first, sides.second);
    }
    throw GraphIrError("op `" + op + "` is not an eager operator");
  }

  static size_t len_value(const JsonValue& value) {
    if (value.is_string()) return graph_rune_count(value.string);
    if (value.is_array()) return value.array.size();
    if (value.is_object()) return value.object.size();
    throw GraphIrError("len requires a string, array, or object, got " + graph_canonical_json(value));
  }

  static JsonValue eval_get(const JsonValue& target, const JsonValue& key) {
    if (target.is_array()) {
      const GraphNum index = graph_as_num(key);
      if (!index.integer) throw GraphIrError("array index must be an integer");
      if (index.as_int < 0) throw GraphIrError("array index must be >= 0");
      if (static_cast<uint64_t>(index.as_int) >= target.array.size())
        throw GraphIrError("index " + std::to_string(index.as_int) + " out of range");
      return target.array[static_cast<size_t>(index.as_int)];
    }
    if (target.is_object()) {
      if (!key.is_string()) throw GraphIrError("object key must be a string, got " + graph_canonical_json(key));
      const JsonValue* value = target.find(key.string);
      if (value == nullptr) throw GraphIrError("missing key `" + key.string + "`");
      return *value;
    }
    throw GraphIrError("get requires an array or object, got " + graph_canonical_json(target));
  }

  static JsonValue fold_policy(const std::string& policy, const JsonValue& old_value, const JsonValue& op) {
    if (policy == kPolicyKeepLatest) return op;
    if (policy == kPolicySum) return graph_num_bin("add", old_value, op);
    if (policy == kPolicyMax) return graph_ordering(old_value, op) >= 0 ? old_value : op;
    if (policy == kPolicyMin) return graph_ordering(old_value, op) <= 0 ? old_value : op;
    if (policy == kPolicyAnd) return JsonValue::of_bool(graph_expect_bool(old_value) && graph_expect_bool(op));
    if (policy == kPolicyOr) return JsonValue::of_bool(graph_expect_bool(old_value) || graph_expect_bool(op));
    throw GraphIrError("unknown source policy `" + policy + "`");
  }

  JsonValue eval_stmt(const JsonValue& stmt) {
    const std::string verb = stmt.find("do")->string;
    if (verb == "eval") return eval_expr(*stmt.find("expr"));
    if (verb == "set") return set_source(stmt.find("target")->string, eval_expr(*stmt.find("expr")));
    if (verb == "merge") return merge_source(stmt.find("target")->string, eval_expr(*stmt.find("expr")));
    if (verb == "seq") {
      JsonArray steps;
      for (const auto& step : stmt.find("steps")->array) steps.push_back(eval_stmt(step));
      return JsonValue::of_array(std::move(steps));
    }
    if (verb == "when") {
      if (!graph_expect_bool(eval_expr(*stmt.find("cond")))) {
        if (const JsonValue* otherwise = stmt.find("else")) return eval_stmt(*otherwise);
        return JsonValue::null();
      }
      return eval_stmt(*stmt.find("then"));
    }
    throw GraphIrError("unknown statement `" + verb + "`");
  }
};

}  // namespace lazily

#endif  // LAZILY_GRAPH_IR_HPP
