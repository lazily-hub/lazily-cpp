// Minimal standalone JSON reader for conformance replays (`#lzspecconf`).
//
// Extracted from the reactive-graph conformance runner so the fixture-driven
// suites (topic / seqcrdt / lossless-tree / command) can replay the canonical
// `lazily-spec/conformance/**` corpora generically — reading the ACTUAL fixture
// bytes rather than a hand transcription. Each conformance test is its own
// executable/TU, so this header carries no cross-TU linkage concerns.

#ifndef LAZILY_TESTS_TEST_JSON_HPP
#define LAZILY_TESTS_TEST_JSON_HPP

#include <cctype>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "test_require.hpp"

namespace lazily_test {

struct Json;
using JsonPtr = std::shared_ptr<Json>;

struct Json {
  enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
  bool boolean = false;
  double number = 0;
  std::string str;
  std::vector<JsonPtr> array;
  std::vector<std::pair<std::string, JsonPtr>> object;  // ordered for determinism

  const Json* find(const std::string& key) const {
    for (const auto& kv : object)
      if (kv.first == key) return kv.second.get();
    return nullptr;
  }
  bool has(const std::string& key) const { return find(key) != nullptr; }
  bool is_null() const { return type == Type::Null; }
  bool is_array() const { return type == Type::Array; }
  bool is_object() const { return type == Type::Object; }
  long long as_int() const { return static_cast<long long>(number); }
  double as_double() const { return number; }
  bool as_bool() const { return boolean; }
  const std::string& as_str() const { return str; }
};

struct JsonParser {
  const std::string& src;
  std::size_t pos = 0;

  explicit JsonParser(const std::string& s) : src(s) {}

  void skip_ws() {
    while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t' ||
                                src[pos] == '\n' || src[pos] == '\r'))
      ++pos;
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
    if (pos < src.size() && src[pos] == '}') { ++pos; return node; }
    while (true) {
      skip_ws();
      auto key = parse_string();
      skip_ws();
      REQUIRE(pos < src.size() && src[pos] == ':', "expected ':' in JSON object");
      ++pos;
      node->object.emplace_back(key->str, parse());
      skip_ws();
      REQUIRE(pos < src.size(), "unterminated JSON object");
      if (src[pos] == ',') { ++pos; continue; }
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
    if (pos < src.size() && src[pos] == ']') { ++pos; return node; }
    while (true) {
      node->array.push_back(parse());
      skip_ws();
      REQUIRE(pos < src.size(), "unterminated JSON array");
      if (src[pos] == ',') { ++pos; continue; }
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
    if (src.compare(pos, 4, "true") == 0) { node->boolean = true; pos += 4; }
    else {
      REQUIRE(src.compare(pos, 5, "false") == 0, "malformed JSON boolean");
      node->boolean = false; pos += 5;
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
           (std::isdigit(static_cast<unsigned char>(src[pos])) || src[pos] == '.' ||
            src[pos] == 'e' || src[pos] == 'E' || src[pos] == '-' || src[pos] == '+'))
      ++pos;
    REQUIRE(pos > start, "malformed JSON number");
    node->number = std::stod(src.substr(start, pos - start));
    return node;
  }
};

// Parse a whole document.
inline JsonPtr parse_json(const std::string& text) {
  JsonParser parser(text);
  return parser.parse();
}

}  // namespace lazily_test

#endif  // LAZILY_TESTS_TEST_JSON_HPP
