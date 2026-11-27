#include <lazily/view.hpp>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "test_spec_fixture.hpp"

namespace {

bool same(const lazily::JsonValue& a, const lazily::JsonValue& b) {
  using Type = lazily::JsonValue::Type;
  if (a.type == Type::Int && b.type == Type::Double) return static_cast<double>(a.integer) == b.real;
  if (a.type == Type::Double && b.type == Type::Int) return a.real == static_cast<double>(b.integer);
  if (a.type != b.type) return false;
  switch (a.type) {
    case Type::Null:
      return true;
    case Type::Bool:
      return a.boolean == b.boolean;
    case Type::Int:
      return a.integer == b.integer;
    case Type::Double:
      return a.real == b.real;
    case Type::String:
      return a.string == b.string;
    case Type::Array:
      if (a.array.size() != b.array.size()) return false;
      for (size_t i = 0; i < a.array.size(); ++i)
        if (!same(a.array[i], b.array[i])) return false;
      return true;
    case Type::Object:
      if (a.object.size() != b.object.size()) return false;
      for (const auto& kv : a.object) {
        const lazily::JsonValue* other = b.find(kv.first);
        if (other == nullptr || !same(kv.second, *other)) return false;
      }
      return true;
  }
  return false;
}

void expect_same(const std::string& label, const lazily::JsonValue& got, const lazily::JsonValue& want) {
  if (same(got, want)) return;
  std::cerr << label << " mismatch\n got  " << lazily::json_write(got) << "\n want " << lazily::json_write(want)
            << std::endl;
  std::exit(1);
}

void replay(const std::string& name, const lazily::JsonValue& fixture) {
  lazily::Context ctx;
  lazily::View view = lazily::View::install(ctx, *fixture.find("view"));
  expect_same(name + " signals", view.signals_json(), *fixture.find("signals"));
  expect_same(name + " expressions", view.expressions_json(), *fixture.find("expressions"));
  expect_same(name + " layout", view.layout_json(), *fixture.find("layout"));
  expect_same(name + " trace", lazily::JsonValue::empty_array(), *fixture.find("trace"));
  expect_same(name + " chart", view.chart_json(), *fixture.find("chart"));
  const lazily::JsonValue* steps = fixture.find("steps");
  if (steps == nullptr || !steps->is_array()) {
    std::cerr << name << " has no steps\n";
    std::exit(1);
  }
  for (size_t i = 0; i < steps->array.size(); ++i) {
    const lazily::JsonValue& step = steps->array[i];
    const lazily::JsonValue* value = step.find("value");
    lazily::JsonValue trace = view.dispatch(step.find("node")->string, step.find("event")->string,
                                            value == nullptr ? lazily::JsonValue::null() : *value);
    const std::string label = name + " step " + std::to_string(i);
    expect_same(label + " signals", view.signals_json(), *step.find("signals"));
    expect_same(label + " expressions", view.expressions_json(), *step.find("expressions"));
    expect_same(label + " trace", trace, *step.find("trace"));
    expect_same(label + " chart", view.chart_json(), *step.find("chart"));
  }
}

}  // namespace

int main() {
  const auto dir = lazily_test::spec_conformance_dir() / "view";
  if (!std::filesystem::is_directory(dir)) {
    std::cout << "NOTE: conformance/view is absent at " << dir << "; skipping\n";
    return 0;
  }
  std::vector<std::string> found;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.path().extension() == ".json") found.push_back(entry.path().filename().string());
  }
  std::sort(found.begin(), found.end());
  const std::vector<std::string> want = {
      "batch_send.json", "counter.json", "field_change.json", "malformed_rejected.json"};
  if (found != want) {
    std::cerr << "view fixture set drifted\n";
    return 1;
  }
  for (const char* name : {"counter.json", "field_change.json", "batch_send.json"}) {
    const lazily::JsonValue fixture =
        lazily::json_parse(lazily_test::spec_fixture_text("view", name));
    if (!fixture.find("kind") || fixture.find("kind")->string != "View") {
      std::cerr << name << " is not a View fixture\n";
      return 1;
    }
    try {
      replay(name, fixture);
    } catch (const std::exception& err) {
      std::cerr << name << " failed: " << err.what() << std::endl;
      return 1;
    }
  }
  const lazily::JsonValue malformed =
      lazily::json_parse(lazily_test::spec_fixture_text("view", "malformed_rejected.json"));
  const lazily::JsonValue* cases = malformed.find("cases");
  if (cases == nullptr || !cases->is_array() || cases->array.empty()) {
    std::cerr << "malformed corpus has no cases\n";
    return 1;
  }
  for (const auto& scenario : cases->array) {
    const std::string name = scenario.find("name")->string;
    bool rejected = false;
    try {
      lazily::Context ctx;
      (void)lazily::View::install(ctx, *scenario.find("view"));
    } catch (const std::exception&) {
      rejected = true;
    }
    if (!rejected) {
      std::cerr << name << " must be rejected\n";
      return 1;
    }
  }
  return 0;
}
