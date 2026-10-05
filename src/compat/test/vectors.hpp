// Test helpers: load a golden vector file and count the cases of each group.
#pragma once

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "compat/json.hpp"
#include "compat/time.hpp"
#include "doctest.h"

namespace testing_support {

namespace json = campfire::compat::json;

inline const json::Value& null_value() {
  static const json::Value v;
  return v;
}

// Member `key` of an object, or null.
inline const json::Value& at(const json::Value& v, std::string_view key) {
  const json::Value* m = v.find(key);
  return m ? *m : null_value();
}

inline const json::Value::Array& items(const json::Value& v) { return v.as_array(); }

inline std::optional<std::string_view> opt_str(const json::Value& v) {
  if (const std::string* s = v.get_string()) return std::string_view(*s);
  return std::nullopt;
}

inline campfire::compat::Timestamp time_of(const json::Value& v) {
  auto t = campfire::compat::parse_iso8601(v.as_string());
  REQUIRE(t.has_value());
  return *t;
}

inline std::optional<campfire::compat::Timestamp> opt_time(const json::Value& v) {
  if (!v.is_string()) return std::nullopt;
  return time_of(v);
}

inline const json::Value& load_vectors(const char* file) {
  static std::vector<std::pair<std::string, json::Value>> cache;
  for (auto& [name, value] : cache) {
    if (name == file) return value;
  }
  std::string path = std::string(CAMPFIRE_VECTORS_DIR) + "/" + file;
  std::ifstream in(path, std::ios::binary);
  REQUIRE_MESSAGE(in.good(), "cannot open " << path);
  std::stringstream buffer;
  buffer << in.rdbuf();
  auto parsed = json::parse(buffer.str());
  REQUIRE_MESSAGE(parsed.has_value(), "invalid JSON in " << path);
  cache.emplace_back(file, std::move(*parsed));
  return cache.back().second;
}

// Counts the cases of one vector group. finish() prints a line for the report table.
class Group {
 public:
  Group(std::string file, std::string name) : file_(std::move(file)), name_(std::move(name)) {}

  void check(bool ok, const std::string& label) {
    ++total_;
    if (ok) ++pass_;
    else if (failures_.size() < 10) failures_.push_back(label);
  }

  void finish() {
    std::printf("VECTORS %-22s %-28s cases=%d pass=%d\n", file_.c_str(), name_.c_str(), total_, pass_);
    std::string detail;
    for (auto& f : failures_) detail += "\n  " + f;
    CHECK_MESSAGE(total_ > 0, "no cases in " << name_);
    CHECK_MESSAGE(pass_ == total_, name_ << ": " << (total_ - pass_) << " failures" << detail);
  }

 private:
  std::string file_, name_;
  int total_ = 0, pass_ = 0;
  std::vector<std::string> failures_;
};

}  // namespace testing_support
