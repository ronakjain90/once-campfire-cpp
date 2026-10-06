// Rails: app/models/user.rb (roles, status, has_secure_password, authenticate_bot). Rust: crates/db/src/models/user.rs.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/arena.hpp"
#include "core/error.hpp"
#include "db/connection.hpp"
#include "db/database.hpp"
#include "db/schema.gen.hpp"

namespace campfire::models {

// `enum :role, %i[ member administrator bot ]`
enum class Role : std::int64_t { Member = 0, Administrator = 1, Bot = 2 };
// `enum :status, %i[ active deactivated banned ]`
inline constexpr std::int64_t kStatusActive = 0;

// A user row that owns its text. The session cache keeps these.
struct User {
  std::int64_t id = 0;
  std::optional<std::string> bio;
  std::optional<std::string> bot_token;
  std::string created_at;
  std::optional<std::string> email_address;
  std::string name;
  std::optional<std::string> password_digest;
  std::int64_t role = 0;
  std::int64_t status = 0;
  std::string updated_at;

  [[nodiscard]] static User from_row(const db::schema::UserRow& row);
  [[nodiscard]] bool is_administrator() const noexcept {
    return role == static_cast<std::int64_t>(Role::Administrator);
  }
  [[nodiscard]] bool is_bot() const noexcept { return role == static_cast<std::int64_t>(Role::Bot); }
  // `can_administer?` with no record: administrators only.
  [[nodiscard]] bool can_administer() const noexcept { return is_administrator(); }
  // `has_secure_password#authenticate`. Blocks about 250 ms: call it on a job thread.
  [[nodiscard]] bool authenticate(std::string_view password) const;
};

// The attributes of `User.create!`.
struct NewUser {
  std::string name;
  std::optional<std::string> email_address;
  std::optional<std::string> password_digest;  // `has_secure_password`: nil for a blank password
  Role role = Role::Member;
};

namespace users {

// `User.create!`: the row, then `grant_membership_to_open_rooms` (`after_create_commit`: the writer runs it in the
// same transaction). A duplicate email address gives a constraint error with "UNIQUE" in its message
// (`ActiveRecord::RecordNotUnique`). Records changes of `users` and `memberships`.
[[nodiscard]] Result<User> create(db::Tx& tx, const NewUser& attributes);

[[nodiscard]] Result<std::optional<User>> find_by_id(db::Connection& conn, Arena& arena, std::int64_t id);
// `User.active.find_by(email_address:)`: the lookup half of `authenticate_by`.
[[nodiscard]] Result<std::optional<User>> find_active_by_email_address(db::Connection& conn, Arena& arena,
                                                                       std::string_view email_address);
// `User.authenticate_bot(bot_key)`: `"#{id}-#{bot_token}"`.
[[nodiscard]] Result<std::optional<User>> authenticate_bot(db::Connection& conn, Arena& arena,
                                                           std::string_view bot_key);
// The password half of `User.active.authenticate_by`. A missing user costs the same time as a
// wrong password. Blocks: call it on a job thread. A blank password gives nothing.
[[nodiscard]] std::optional<User> authenticated(std::optional<User> candidate, std::string_view password);
// `User.none?`
[[nodiscard]] Result<bool> none(db::Connection& conn, Arena& arena);

// `User.administrator.first`, for `accounts/_help_contact`: the name and the email address.
struct Owner {
  std::string name;
  std::string email_address;
};
[[nodiscard]] Result<std::optional<Owner>> first_administrator(db::Connection& conn, Arena& arena);

}  // namespace users
}  // namespace campfire::models
