// Rails: app/models/account.rb (the singleton account, the logo attachment). Rust: crates/db/src/models/account.rs.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/arena.hpp"
#include "core/error.hpp"
#include "db/connection.hpp"
#include "db/database.hpp"

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
// `Account.any?`
[[nodiscard]] Result<bool> any(db::Connection& conn, Arena& arena);
// `Account.first&.join_code`
[[nodiscard]] Result<std::optional<std::string>> first_join_code(db::Connection& conn, Arena& arena);
// `Account.create!(name:)`: a new join code and the default settings. Returns the id. Records a change.
[[nodiscard]] Result<std::int64_t> create(db::Tx& tx, std::string_view name);
// `SecureRandom.alphanumeric(12).scan(/.{4}/).join("-")`
[[nodiscard]] std::string generate_join_code();

}  // namespace accounts
}  // namespace campfire::models
