// Search and the search of messages. Rails: app/models/search.rb, app/models/message/searchable.rb. Rust:
// crates/db/src/models/search.rs, message.rs.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/arena.hpp"
#include "core/error.hpp"
#include "db/connection.hpp"
#include "db/database.hpp"
#include "models/message.hpp"

namespace campfire::models {

// `Search::RECENT`: how many recent searches the trim keeps.
inline constexpr std::int64_t kRecentSearches = 10;

namespace searches {

// `Current.user.searches.ordered.pluck(:query)`: the newest first.
[[nodiscard]] Result<std::vector<std::string>> recent_queries(db::Connection& conn, Arena& arena, std::int64_t user_id);

// `Current.user.searches.record(query)`: `find_or_create_by(query:).touch`. A new search trims the user's searches to
// the 10 newest.
[[nodiscard]] Status record(db::Tx& tx, std::int64_t user_id, std::string_view query);

// `Current.user.searches.destroy_all`
[[nodiscard]] Status destroy_all(db::Tx& tx, std::int64_t user_id);

}  // namespace searches

namespace messages {

// `Current.user.reachable_messages.search(query).last(100)`: the oldest first. `terms` is `search_query::match_terms`
// of the query; empty terms give no message.
[[nodiscard]] Result<std::vector<Message>> search_reachable(db::Connection& conn, Arena& arena, std::int64_t user_id,
                                                            std::string_view terms);

}  // namespace messages
}  // namespace campfire::models
