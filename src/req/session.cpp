// The cookie store session and the flash (Rails: ActionDispatch::Session::CookieStore,
// ActionDispatch::Flash; Rust: crates/kit/src/session.rs).
#include "req/session.hpp"

#include <algorithm>

#include "compat/crypto.hpp"
#include "core/time_format.hpp"

namespace campfire::req {

using compat::json::Value;

std::string generate_session_id() {
  std::string bytes;
  compat::crypto::random_bytes(bytes, 16);
  return compat::crypto::hex_encode(bytes);
}

Session& Session::load(const CookieJar& jar) {
  if (loaded_) return *this;
  auto cookie = jar.encrypted_value(kSessionKey);
  if (cookie && cookie->is_object()) data_ = std::move(*cookie);
  const Value* sid = data_.find("session_id");
  if (sid == nullptr || sid->is_null()) data_.set("session_id", Value(generate_session_id()));
  loaded_ = true;
  return *this;
}

std::optional<std::string_view> Session::id() const { return get_str("session_id"); }

const Value* Session::get(std::string_view key) const {
  const Value* v = data_.find(key);
  return (v == nullptr || v->is_null()) ? nullptr : v;
}

std::optional<std::string_view> Session::get_str(std::string_view key) const {
  const Value* v = get(key);
  if (v == nullptr || !v->is_string()) return std::nullopt;
  return std::string_view(v->as_string());
}

bool Session::contains_key(std::string_view key) const { return data_.find(key) != nullptr; }

void Session::insert(std::string_view key, Value value) {
  const Value* old = data_.find(key);
  if (old != nullptr && *old == value) return;
  data_.set(std::string(key), std::move(value));
  changed_ = true;
}

void Session::remove(std::string_view key) {
  auto& members = data_.as_object();
  const auto it = std::ranges::find_if(members, [&](const auto& m) { return m.first == key; });
  if (it == members.end()) return;
  members.erase(it);
  changed_ = true;
}

void Session::reset() {
  data_ = Value(Value::Object{});
  data_.set("session_id", Value(generate_session_id()));
  loaded_ = true;
  changed_ = true;
}

Status Session::commit(CookieJar& jar, Timestamp now) {
  if (!changed_) return {};
  Value::Object kept;
  for (const auto& [key, value] : data_.as_object()) {
    if (!value.is_null()) kept.emplace_back(key, value);
  }
  if (std::ranges::all_of(kept, [](const auto& m) { return m.first == "session_id"; })) {
    jar.remove(kSessionKey);
    return {};
  }
  Cookie cookie;
  cookie.httponly = true;
  cookie.expires = campfire::years_from(now, kSessionExpireYears);
  return jar.set_encrypted(kSessionKey, Value(std::move(kept)), std::move(cookie));
}

Flash Flash::from_session_value(const Value* value) {
  Flash flash;
  if (value == nullptr || !value->is_object()) return flash;
  std::vector<std::string_view> discarded;
  if (const Value* d = value->find("discard"); d != nullptr && d->is_array()) {
    for (const Value& item : d->as_array()) {
      if (item.is_string()) discarded.emplace_back(item.as_string());
    }
  }
  const Value* flashes = value->find("flashes");
  if (flashes == nullptr || !flashes->is_object()) return flash;
  for (const auto& [key, v] : flashes->as_object()) {
    if (std::ranges::find(discarded, std::string_view(key)) != discarded.end()) continue;
    flash.flashes_.emplace_back(key, v);
    flash.discard_.push_back(key);
  }
  return flash;
}

std::optional<Value> Flash::to_session_value() const {
  Value::Object keep;
  for (const auto& [key, v] : flashes_) {
    if (std::ranges::find(discard_, key) == discard_.end()) keep.emplace_back(key, v);
  }
  if (keep.empty()) return std::nullopt;
  Value::Object out;
  out.emplace_back("discard", Value(Value::Array{}));
  out.emplace_back("flashes", Value(std::move(keep)));
  return Value(std::move(out));
}

const Value* Flash::get(std::string_view key) const {
  const auto it = std::ranges::find_if(flashes_, [&](const auto& e) { return e.first == key; });
  return it == flashes_.end() ? nullptr : &it->second;
}

std::optional<std::string_view> Flash::get_str(std::string_view key) const {
  const Value* v = get(key);
  if (v == nullptr || !v->is_string()) return std::nullopt;
  return std::string_view(v->as_string());
}

void Flash::set(std::string_view key, Value value) {
  std::erase(discard_, std::string(key));
  const auto it = std::ranges::find_if(flashes_, [&](const auto& e) { return e.first == key; });
  if (it != flashes_.end()) {
    it->second = std::move(value);
  } else {
    flashes_.emplace_back(std::string(key), std::move(value));
  }
}

void Flash::now(std::string_view key, Value value) {
  set(key, std::move(value));
  discard(key);
}

void Flash::keep(std::optional<std::string_view> key) {
  if (key) {
    std::erase(discard_, std::string(*key));
  } else {
    discard_.clear();
  }
}

void Flash::discard(std::optional<std::string_view> key) {
  std::vector<std::string> keys;
  if (key) {
    keys.emplace_back(*key);
  } else {
    for (const auto& e : flashes_) keys.push_back(e.first);
  }
  for (auto& k : keys) {
    if (std::ranges::find(discard_, k) == discard_.end()) discard_.push_back(std::move(k));
  }
}

void Flash::remove(std::string_view key) {
  std::erase(discard_, std::string(key));
  std::erase_if(flashes_, [&](const auto& e) { return e.first == key; });
}

}  // namespace campfire::req
