// Rails: app/models/session.rb. Rust: crates/db/src/models/session.rs.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/arena.hpp"
#include "core/error.hpp"
#include "core/timestamp.hpp"
#include "db/connection.hpp"
#include "db/database.hpp"

namespace campfire::models {

// `Session::ACTIVITY_REFRESH_RATE`: one hour.
inline constexpr std::int64_t kActivityRefreshSeconds = 3600;

// A session row that owns its text. The session cache keeps these.
struct Session {
  std::int64_t id = 0;
  std::int64_t user_id = 0;
  std::string token;
  std::optional<std::string> ip_address;
  std::optional<std::string> user_agent;
  std::string last_active_at;  // as the database keeps it
  std::string created_at;
  std::string updated_at;

  [[nodiscard]] static Session from_row(const db::schema::SessionRow& row);
  // True if the last activity is more than one hour before `now` (`Session#resume`).
  [[nodiscard]] bool needs_resume(Timestamp now) const;
};

namespace sessions {

// `Session.find_by(token:)`.
[[nodiscard]] Result<std::optional<Session>> find_by_token(db::Connection& conn, Arena& arena,
                                                           std::string_view token);

// `user.sessions.start!(user_agent:, ip_address:)`. Records a change of the table `sessions`.
[[nodiscard]] Result<Session> start(db::Tx& tx, std::int64_t user_id, std::optional<std::string_view> user_agent,
                                    std::optional<std::string_view> ip_address);

// `Session#resume`: refreshes the activity, the user agent and the address. Does nothing if the
// session was active less than one hour ago. Records a change of the table `sessions`.
[[nodiscard]] Status resume(db::Tx& tx, Session& session, std::optional<std::string_view> user_agent,
                            std::optional<std::string_view> ip_address);

// `Session#destroy!`. Records a change of the table `sessions`.
[[nodiscard]] Status destroy(db::Tx& tx, const Session& session);

// `SecureRandom.base58(24)` (`has_secure_token`).
[[nodiscard]] std::string generate_token();

}  // namespace sessions
}  // namespace campfire::models
