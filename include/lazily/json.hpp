#ifndef LAZILY_JSON_HPP
#define LAZILY_JSON_HPP

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lazily {

// Minimal, dependency-free JSON DOM for the `json` REFERENCE codec
// (`#lzcppjsoncodec`).
//
// protocol.md § Frame codecs makes `json` the reference codec: the required,
// dependency-free, human-inspectable interop floor every binding MUST speak,
// what the FFI baseline re-encodes to, and what conformance fixtures are
// written in. "Dependency-free" is the substance of that role, so this lives in
// the library rather than pulling in a third-party parser — lazily-cpp ships
// header-only, and a floor that needs an external JSON dependency is not a
// floor.
//
// tests/test_json.hpp is a fixture READER of the same shape. It is deliberately
// not reused: a test helper cannot be part of the shipped codec, and the
// library has to ENCODE as well as parse — reading a frame you cannot produce
// is the exact gap this header closes.
//
// Scope is what an `IpcMessage` frame needs: objects, arrays, strings,
// integers, doubles, booleans, null. `json_write` is byte-canonical for a given
// DOM (no whitespace, key order = insertion order), which is how protocol.md's
// "json is byte-canonical" holds here: the codec drives insertion order
// deterministically, so one message has exactly one byte form.

struct JsonValue;

// Insertion-ordered, so encoding is deterministic. An IpcMessage frame carries
// a handful of keys per object; a linear scan beats a map at this size.
using JsonObject = std::vector<std::pair<std::string, JsonValue>>;
using JsonArray = std::vector<JsonValue>;

struct JsonValue {
  enum class Type { Null, Bool, Int, Double, String, Array, Object };

  Type type = Type::Null;
  bool boolean = false;
  int64_t integer = 0;
  double real = 0;
  std::string string;
  JsonArray array;
  JsonObject object;

  JsonValue() = default;

  static JsonValue null() { return JsonValue{}; }
  // Deliberately NOT overloads of one `of`: `of(3)` would be ambiguous between
  // the bool and int64 forms (both are rank-"conversion"), and a literal
  // silently landing on `bool` is the kind of encoder bug a round-trip test
  // catches late.
  static JsonValue of_bool(bool v) {
    JsonValue j;
    j.type = Type::Bool;
    j.boolean = v;
    return j;
  }
  static JsonValue of_int(int64_t v) {
    JsonValue j;
    j.type = Type::Int;
    j.integer = v;
    return j;
  }
  static JsonValue of_double(double v) {
    JsonValue j;
    j.type = Type::Double;
    j.real = v;
    return j;
  }
  static JsonValue of_string(std::string v) {
    JsonValue j;
    j.type = Type::String;
    j.string = std::move(v);
    return j;
  }
  static JsonValue of_string(const char* v) { return of_string(std::string(v)); }
  static JsonValue of_array(JsonArray v) {
    JsonValue j;
    j.type = Type::Array;
    j.array = std::move(v);
    return j;
  }
  static JsonValue of_object(JsonObject v) {
    JsonValue j;
    j.type = Type::Object;
    j.object = std::move(v);
    return j;
  }
  static JsonValue empty_array() { return of_array(JsonArray{}); }
  static JsonValue empty_object() { return of_object(JsonObject{}); }

  bool is_null() const { return type == Type::Null; }
  bool is_bool() const { return type == Type::Bool; }
  bool is_number() const { return type == Type::Int || type == Type::Double; }
  bool is_string() const { return type == Type::String; }
  bool is_array() const { return type == Type::Array; }
  bool is_object() const { return type == Type::Object; }

  const JsonValue* find(std::string_view key) const {
    for (const auto& kv : object) {
      if (kv.first == key) return &kv.second;
    }
    return nullptr;
  }

  // Append to an object. Callers drive key order, which is what makes the
  // encoded form canonical.
  void set(std::string key, JsonValue value) {
    type = Type::Object;
    object.emplace_back(std::move(key), std::move(value));
  }
  void push(JsonValue value) {
    type = Type::Array;
    array.push_back(std::move(value));
  }

  int64_t as_int() const {
    if (type == Type::Int) return integer;
    if (type == Type::Double) return static_cast<int64_t>(real);
    throw std::runtime_error("json: expected a number");
  }
  double as_double() const {
    if (type == Type::Double) return real;
    if (type == Type::Int) return static_cast<double>(integer);
    throw std::runtime_error("json: expected a number");
  }
  bool as_bool() const {
    if (type != Type::Bool) throw std::runtime_error("json: expected a bool");
    return boolean;
  }
  const std::string& as_string() const {
    if (type != Type::String) throw std::runtime_error("json: expected a string");
    return string;
  }
};

// -- writing ------------------------------------------------------------------

inline void json_write_string(std::string& out, std::string_view s) {
  out += '"';
  for (const char c : s) {
    const auto byte = static_cast<unsigned char>(c);
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    default:
      if (byte < 0x20) {
        // Control bytes are the only ones that MUST be escaped beyond the named
        // set above; multibyte UTF-8 passes through verbatim so the output
        // carries the same bytes the input did.
        static const char kHex[] = "0123456789abcdef";
        out += "\\u00";
        out += kHex[(byte >> 4) & 0xF];
        out += kHex[byte & 0xF];
      } else {
        out += c;
      }
    }
  }
  out += '"';
}

inline void json_write_double(std::string& out, double v) {
  // `%.17g` round-trips every finite double exactly. The IpcMessage codec never
  // emits one (every wire field is an integer, string, or byte array); this
  // exists so the DOM is a complete JSON writer rather than a codec-shaped
  // subset that surprises the next caller.
  char buf[40];
  const int n = std::snprintf(buf, sizeof(buf), "%.17g", v);
  out.append(buf, n > 0 ? static_cast<size_t>(n) : 0);
}

inline void json_write_to(std::string& out, const JsonValue& value) {
  switch (value.type) {
  case JsonValue::Type::Null:
    out += "null";
    return;
  case JsonValue::Type::Bool:
    out += value.boolean ? "true" : "false";
    return;
  case JsonValue::Type::Int:
    out += std::to_string(value.integer);
    return;
  case JsonValue::Type::Double:
    json_write_double(out, value.real);
    return;
  case JsonValue::Type::String:
    json_write_string(out, value.string);
    return;
  case JsonValue::Type::Array:
    out += '[';
    for (size_t i = 0; i < value.array.size(); ++i) {
      if (i != 0) out += ',';
      json_write_to(out, value.array[i]);
    }
    out += ']';
    return;
  case JsonValue::Type::Object:
    out += '{';
    for (size_t i = 0; i < value.object.size(); ++i) {
      if (i != 0) out += ',';
      json_write_string(out, value.object[i].first);
      out += ':';
      json_write_to(out, value.object[i].second);
    }
    out += '}';
    return;
  }
}

// Byte-canonical rendering: no whitespace, keys in the order the encoder wrote
// them.
inline std::string json_write(const JsonValue& value) {
  std::string out;
  json_write_to(out, value);
  return out;
}

// -- parsing ------------------------------------------------------------------

// Parsing is an implementation detail behind `json_parse`, so the scanner lives
// in a nested namespace: tests/test_json.hpp has carried its own
// `lazily_test::JsonParser` since #lzspecconf, and a TU that pulls in both
// namespaces (every conformance runner does) would find the name ambiguous.
namespace json_detail {

// Throws std::runtime_error on malformed input, matching the msgpack codec's
// failure mode (msgpack.hpp). A frame that does not parse is not a frame.
class JsonParser {
public:
  explicit JsonParser(std::string_view src) : src_(src) {}

  JsonValue parse_document() {
    JsonValue value = parse_value();
    skip_ws();
    if (pos_ != src_.size()) fail("trailing bytes after the top-level value");
    return value;
  }

private:
  std::string_view src_;
  size_t pos_ = 0;

  [[noreturn]] void fail(const char* what) const {
    throw std::runtime_error("json: " + std::string(what) + " at offset " + std::to_string(pos_));
  }

  void skip_ws() {
    while (pos_ < src_.size()) {
      const char c = src_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
        ++pos_;
      else
        break;
    }
  }

  char peek() const {
    if (pos_ >= src_.size()) fail("unexpected end of input");
    return src_[pos_];
  }

  void expect(char c) {
    if (peek() != c) fail("unexpected character");
    ++pos_;
  }

  bool literal(std::string_view word) {
    if (src_.size() - pos_ < word.size()) return false;
    if (src_.compare(pos_, word.size(), word) != 0) return false;
    pos_ += word.size();
    return true;
  }

  JsonValue parse_value() {
    skip_ws();
    switch (peek()) {
    case '{':
      return parse_object();
    case '[':
      return parse_array();
    case '"':
      return JsonValue::of_string(parse_string());
    case 't':
      if (!literal("true")) fail("malformed literal");
      return JsonValue::of_bool(true);
    case 'f':
      if (!literal("false")) fail("malformed literal");
      return JsonValue::of_bool(false);
    case 'n':
      if (!literal("null")) fail("malformed literal");
      return JsonValue::null();
    default:
      return parse_number();
    }
  }

  JsonValue parse_object() {
    expect('{');
    JsonValue node = JsonValue::empty_object();
    skip_ws();
    if (peek() == '}') {
      ++pos_;
      return node;
    }
    for (;;) {
      skip_ws();
      std::string key = parse_string();
      skip_ws();
      expect(':');
      node.object.emplace_back(std::move(key), parse_value());
      skip_ws();
      const char c = peek();
      ++pos_;
      if (c == '}') return node;
      if (c != ',') fail("expected ',' or '}' in object");
    }
  }

  JsonValue parse_array() {
    expect('[');
    JsonValue node = JsonValue::empty_array();
    skip_ws();
    if (peek() == ']') {
      ++pos_;
      return node;
    }
    for (;;) {
      node.array.push_back(parse_value());
      skip_ws();
      const char c = peek();
      ++pos_;
      if (c == ']') return node;
      if (c != ',') fail("expected ',' or ']' in array");
    }
  }

  uint32_t hex4() {
    if (src_.size() - pos_ < 4) fail("truncated \\u escape");
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = src_[pos_++];
      v <<= 4;
      if (c >= '0' && c <= '9')
        v |= static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f')
        v |= static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        v |= static_cast<uint32_t>(c - 'A' + 10);
      else
        fail("bad hex digit in a \\u escape");
    }
    return v;
  }

  static void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }

  std::string parse_string() {
    expect('"');
    std::string out;
    for (;;) {
      if (pos_ >= src_.size()) fail("unterminated string");
      const char c = src_[pos_++];
      if (c == '"') return out;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (pos_ >= src_.size()) fail("unterminated escape");
      const char esc = src_[pos_++];
      switch (esc) {
      case '"':
        out += '"';
        break;
      case '\\':
        out += '\\';
        break;
      case '/':
        out += '/';
        break;
      case 'b':
        out += '\b';
        break;
      case 'f':
        out += '\f';
        break;
      case 'n':
        out += '\n';
        break;
      case 'r':
        out += '\r';
        break;
      case 't':
        out += '\t';
        break;
      case 'u': {
        uint32_t cp = hex4();
        // Surrogate pair: the low half arrives as a separate \u escape.
        if (cp >= 0xD800 && cp <= 0xDBFF && src_.size() - pos_ >= 6 && src_[pos_] == '\\' &&
            src_[pos_ + 1] == 'u') {
          pos_ += 2;
          const uint32_t low = hex4();
          if (low >= 0xDC00 && low <= 0xDFFF)
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          else
            pos_ -= 6; // not a pair after all — leave the escape for the caller
        }
        append_utf8(out, cp);
        break;
      }
      default:
        fail("unknown escape");
      }
    }
  }

  JsonValue parse_number() {
    const size_t start = pos_;
    bool floating = false;
    if (pos_ < src_.size() && (src_[pos_] == '-' || src_[pos_] == '+')) ++pos_;
    while (pos_ < src_.size()) {
      const char c = src_[pos_];
      if (c >= '0' && c <= '9') {
        ++pos_;
      } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
        floating = floating || c == '.' || c == 'e' || c == 'E';
        ++pos_;
      } else {
        break;
      }
    }
    if (pos_ == start) fail("expected a value");
    const std::string token(src_.substr(start, pos_ - start));
    // An integer wider than `int64_t` — a `NodeId` in the top half of the u64
    // wire range, say — MUST be refused, never rounded or truncated
    // (protocol.md § NodeId / PeerId, `#lzspecdecoderbound`). `std::stoll`
    // already refuses it, but by throwing `std::out_of_range`, which derives
    // from `std::logic_error` and NOT from the `std::runtime_error` this parser
    // raises for every other malformed frame. A caller guarding decode with
    // `catch (const std::runtime_error&)` — the documented error type — missed
    // it entirely and terminated. Route it back through `fail()` so an
    // out-of-range identifier is an ordinary, catchable decode error.
    try {
      if (floating) return JsonValue::of_double(std::stod(token));
      return JsonValue::of_int(static_cast<int64_t>(std::stoll(token)));
    } catch (const std::out_of_range&) {
      fail("number is outside the range this decoder represents exactly");
    }
  }
};

} // namespace json_detail

inline JsonValue json_parse(std::string_view text) {
  return json_detail::JsonParser(text).parse_document();
}

} // namespace lazily

#endif // LAZILY_JSON_HPP
