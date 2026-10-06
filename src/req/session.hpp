// The cookie store session and the flash (Rails: ActionDispatch::Session::CookieStore,
// ActionDispatch::Flash; config/initializers/session_store.rb; Rust: crates/kit/src/session.rs).
// The session writes its cookie only when it changes, and deletes the cookie when it is empty.
// Rails rewrites the cookie on each request (a known difference of the Rust port).
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "compat/json.hpp"
#include "core/error.hpp"
#include "core/timestamp.hpp"
#include "req/cookie.hpp"

namespace campfire::req {

inline constexpr std::string_view kSessionKey = "_campfire_session";
inline constexpr int kSessionExpireYears = 20;

// SecureRandom.hex(16)
[[nodiscard]] std::string generate_session_id();

class Session {
 public:
  Session() = default;

  [[nodiscard]] bool is_loaded() const { return loaded_; }
  // Reads the cookie once (load_for_read! and load_for_write!). A new session gets a session_id.
  Session& load(const CookieJar& jar);

  [[nodiscard]] std::optional<std::string_view> id() const;
  // session[key]: null if the value is nil.
  [[nodiscard]] const compat::json::Value* get(std::string_view key) const;
  [[nodiscard]] std::optional<std::string_view> get_str(std::string_view key) const;
  [[nodiscard]] bool contains_key(std::string_view key) const;
  // session[key] = value. It marks the session as changed only if the value is new.
  void insert(std::string_view key, compat::json::Value value);
  // session.delete(key)
  void remove(std::string_view key);
  // reset_session: a new session id and no other data.
  void reset();

  // Writes the cookie to `jar` if the session changed. Deletes it if only the id is left.
  [[nodiscard]] Status commit(CookieJar& jar, Timestamp now);

 private:
  bool loaded_ = false;
  bool changed_ = false;
  compat::json::Value data_ = compat::json::Value(compat::json::Value::Object{});
};

// ActionDispatch::Flash::FlashHash. The session keeps it under "flash" as
// { "discard" => [], "flashes" => { ... } }.
class Flash {
 public:
  // FlashHash.from_session_value: what the last request left is shown now and then discarded.
  [[nodiscard]] static Flash from_session_value(const compat::json::Value* value);
  // FlashHash#to_session_value: null if nothing stays.
  [[nodiscard]] std::optional<compat::json::Value> to_session_value() const;

  [[nodiscard]] const compat::json::Value* get(std::string_view key) const;
  [[nodiscard]] std::optional<std::string_view> get_str(std::string_view key) const;
  // flash[key] = value: shown in the next request.
  void set(std::string_view key, compat::json::Value value);
  // flash.now[key] = value: shown in this request only.
  void now(std::string_view key, compat::json::Value value);
  void keep(std::optional<std::string_view> key = std::nullopt);
  void discard(std::optional<std::string_view> key = std::nullopt);
  void remove(std::string_view key);
  [[nodiscard]] bool empty() const { return flashes_.empty(); }
  // The flashes in the order that they were set.
  [[nodiscard]] const std::vector<std::pair<std::string, compat::json::Value>>& entries() const { return flashes_; }
  [[nodiscard]] std::optional<std::string_view> notice() const { return get_str("notice"); }
  [[nodiscard]] std::optional<std::string_view> alert() const { return get_str("alert"); }

 private:
  std::vector<std::pair<std::string, compat::json::Value>> flashes_;
  std::vector<std::string> discard_;
};

}  // namespace campfire::req
