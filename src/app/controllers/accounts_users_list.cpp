// Rust: crates/campfire/src/controllers/presenters/{accounts,pagination}.rs.
#include "app/controllers/accounts_users_list.hpp"

#include <algorithm>

#include "app/controllers/sidebars.hpp"
#include "compat/ruby.hpp"
#include "models/user_admin.hpp"

namespace campfire::app::controllers {

Page Page::from(std::optional<std::string_view> page_param, std::int64_t records_count) {
  Page page;
  page.number = std::clamp<std::int64_t>(page_param ? compat::to_i(*page_param) : 0, 1, 1'000'000'000);
  page.records_count = records_count;
  return page;
}

std::int64_t Page::page_count() const {
  std::int64_t count = 0;
  std::int64_t residual = records_count;
  while (residual > 0) {
    ++count;
    residual -= kPerPage;
  }
  return std::max<std::int64_t>(count, 1);
}

views::AccountUser account_user(const Rq& rq, const models::User& user) {
  views::AccountUser row;
  row.user = user_summary(rq, user);
  row.is_current = rq.current_user() != nullptr && rq.current_user()->id == user.id;
  row.banned = user.status == models::kStatusBanned;
  row.active = user.status == models::kStatusActive;
  row.bot = user.is_bot();
  row.administrator = user.is_administrator();
  return row;
}

}  // namespace campfire::app::controllers
