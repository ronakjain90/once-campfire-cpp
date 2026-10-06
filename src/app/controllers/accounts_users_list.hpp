// The people list of the account pages. Rails: app/controllers/accounts_controller.rb (`account_users`),
// accounts/users_controller.rb. Rust: crates/campfire/src/controllers/presenters/{accounts,pagination}.rs.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app/rq.hpp"
#include "models/user.hpp"
#include "views/accounts/types.hpp"

namespace campfire::app::controllers {

// geared_pagination's `set_page_and_extract_portion_from records, per_page: 500` for a relation without a cursor.
struct Page {
  std::int64_t number = 1;
  std::int64_t records_count = 0;
  static constexpr std::int64_t kPerPage = 500;

  // `param.to_i > 0 ? param.to_i : 1`, capped so that the arithmetic cannot overflow.
  [[nodiscard]] static Page from(std::optional<std::string_view> page_param, std::int64_t records_count);
  [[nodiscard]] std::int64_t offset() const { return (number - 1) * kPerPage; }
  [[nodiscard]] std::int64_t page_count() const;
  [[nodiscard]] bool is_last() const { return number == page_count(); }
  [[nodiscard]] std::int64_t next_param() const { return number + 1; }
};

// A row of the people list.
[[nodiscard]] views::AccountUser account_user(const Rq& rq, const models::User& user);

}  // namespace campfire::app::controllers
