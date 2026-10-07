// The view model of the search page. Rails: the instance variables of app/views/searches/index.html.erb. Rust:
// crates/views/src/searches.rs (IndexView, search_path).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "compat/ruby.hpp"
#include "routes/routes.hpp"
#include "views/messages/types.hpp"

namespace campfire::views::searches {

struct IndexView {
  // `@query`: `params[:q]` with the characters that are not word characters made spaces, when it is present.
  std::optional<std::string> query;
  // `params[:q]` as sent: the value of the search field.
  std::optional<std::string> q;
  // `@messages`: `Current.user.reachable_messages.search(query).last(100)`.
  std::vector<messages::MessageItem> items;
  // `@recent_searches`: the queries, the newest first.
  std::vector<std::string> recent_searches;
  // `last_room_visited.id`: where the exit button goes.
  std::int64_t return_to_room_id = 0;
};

// `searches_path(q: query)`.
[[nodiscard]] inline std::string search_path(std::string_view query) {
  return campfire::routes::searches() + "?q=" + compat::cgi_escape(query);
}

}  // namespace campfire::views::searches
