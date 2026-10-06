// The account writes. Rails: app/models/account.rb, account/joinable.rb. Rust: crates/db/src/models/account.rs.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/arena.hpp"
#include "core/error.hpp"
#include "db/connection.hpp"
#include "db/database.hpp"

namespace campfire::models::accounts {

// `Account.first.join_code`, with the name, for the edit page.
struct Edit {
  std::int64_t id = 0;
  std::string name;
  std::string join_code;
};
[[nodiscard]] Result<std::optional<Edit>> first_edit(db::Connection& conn, Arena& arena);

// What `account.update!` gets. A member that is not set leaves the attribute alone.
struct Changes {
  std::optional<std::string> name;
  std::optional<std::optional<std::string>> custom_styles;
  // `settings: {}`: the key and the value as text, in the order of the params.
  std::optional<std::vector<std::pair<std::string, std::string>>> settings;
};
// `account.update!(changes)`. It reads the row again on the writer. It writes the changed columns and
// `updated_at`, and nothing if no value changed. A settings key that the schema does not know is an error.
[[nodiscard]] Status update(db::Tx& tx, std::int64_t id, const Changes& changes);
// `account.reset_join_code`
[[nodiscard]] Status reset_join_code(db::Tx& tx, std::int64_t id);
// `record.touch` of an account or a user: sets `updated_at`.
[[nodiscard]] Status touch_account(db::Tx& tx, std::int64_t id);
[[nodiscard]] Status touch_user(db::Tx& tx, std::int64_t id);

}  // namespace campfire::models::accounts
