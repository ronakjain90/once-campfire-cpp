// Rails: app/models/first_run.rb. Rust: crates/db/src/models/first_run.rs.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "core/error.hpp"
#include "db/database.hpp"
#include "models/user.hpp"

namespace campfire::models::first_run {

inline constexpr std::string_view kAccountName = "Campfire";
inline constexpr std::string_view kFirstRoomName = "All Talk";

// `FirstRun.create!(user_params)`: the account, an administrator, and the first open room. The administrator is a
// member of the room. A duplicate email address gives a constraint error with "UNIQUE" in its message.
[[nodiscard]] Result<User> create(db::Tx& tx, std::string_view name, std::string_view email_address,
                                  std::optional<std::string> password_digest);

}  // namespace campfire::models::first_run
