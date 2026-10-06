// Rails: app/models/account.rb (the singleton account, the logo attachment). Rust: crates/db/src/models/account.rs.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "core/arena.hpp"
#include "core/error.hpp"
#include "db/connection.hpp"

namespace campfire::models {

struct Account {
  std::int64_t id = 0;
  std::string name;
  std::optional<std::string> custom_styles;
  std::string updated_at;
  bool has_logo = false;  // `account.logo.attached?`
};

namespace accounts {

// `Account.first` with the logo flag. Nothing before the first run.
[[nodiscard]] Result<std::optional<Account>> first(db::Connection& conn, Arena& arena);

}  // namespace accounts
}  // namespace campfire::models
