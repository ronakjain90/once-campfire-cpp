// Test helpers for the storage tests: the app secrets, the fixture files and a clock.
#pragma once

#include <fstream>
#include <sstream>
#include <string>

#include "compat/secrets.hpp"
#include "compat/test/vectors.hpp"
#include "compat/time.hpp"
#include "compat/variation.hpp"

namespace storage_test {

inline const campfire::compat::Secrets& secrets() {
  static const campfire::compat::Secrets s(
      testing_support::at(testing_support::load_vectors("rails_compat.json"), "secret_key_base").as_string());
  return s;
}
inline const campfire::compat::MessageVerifier& verifier() { return secrets().active_storage_verifier(); }
inline campfire::compat::Timestamp now() { return *campfire::compat::parse_iso8601("2026-09-26T12:00:00Z"); }
inline const campfire::compat::json::Value& vectors() { return testing_support::load_vectors("storage.json"); }

inline std::string fixture_path(const std::string& name) { return std::string(CAMPFIRE_FIXTURES_DIR) + "/" + name; }

inline std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE_MESSAGE(in.good(), "cannot open " << path);
  std::stringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

inline std::string unhex(std::string_view s) {
  std::string out;
  for (size_t i = 0; i + 1 < s.size(); i += 2) out.push_back(char(std::stoi(std::string(s.substr(i, 2)), nullptr, 16)));
  return out;
}

inline std::string hex(std::string_view s) {
  static constexpr char d[] = "0123456789abcdef";
  std::string out;
  for (unsigned char c : s) {
    out.push_back(d[c >> 4]);
    out.push_back(d[c & 15]);
  }
  return out;
}

using testing_support::at;
using testing_support::items;
namespace json = campfire::compat::json;

inline campfire::compat::marshal::Value typed(const json::Value& v) {
  namespace m = campfire::compat::marshal;
  if (v.is_null()) return m::Value::nil();
  if (v.is_bool()) return m::Value::boolean(v.as_bool());
  if (auto n = v.to_int64()) return m::Value::integer(*n);
  if (v.is_array()) {
    m::Value::Array items;
    for (const auto& i : v.as_array()) items.push_back(typed(i));
    return m::Value::array(std::move(items));
  }
  if (const json::Value* s = v.find("sym")) return m::Value::symbol(s->as_string());
  if (const json::Value* s = v.find("str")) return m::Value::string(s->as_string());
  m::Value::Hash entries;
  for (const auto& pair : at(v, "hash").as_array()) entries.emplace_back(pair.as_array()[0].as_string(), typed(pair.as_array()[1]));
  return m::Value::hash(std::move(entries));
}

inline campfire::compat::Variation variation_of(const json::Value& v) {
  auto value = typed(v);
  const auto* hash = std::get_if<campfire::compat::marshal::Value::Hash>(&value.variant());
  REQUIRE(hash != nullptr);
  return campfire::compat::Variation(*hash);
}

}  // namespace storage_test
