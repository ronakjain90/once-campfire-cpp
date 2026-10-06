// Rails: app/models/ban.rb. Rust: crates/db/src/models/ban.rs.
#pragma once

#include <string_view>

#include "core/arena.hpp"
#include "core/error.hpp"
#include "db/connection.hpp"

namespace campfire::models::bans {

// `Ban.banned?(ip_address)`
[[nodiscard]] Result<bool> banned(db::Connection& conn, Arena& arena, std::string_view ip_address);

}  // namespace campfire::models::bans
