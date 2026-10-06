// Query strings of URLs: `Hash#to_query` and `url_for(extra params)`. Rust: crates/views/src/helpers/url.rs.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace campfire::routes {

// A query value: one string, or an array (`key[]=a&key[]=b`).
using QueryValue = std::variant<std::string, std::vector<std::string>>;

// `path` and the parameters as `Hash#to_query` writes them: the pieces sorted, `CGI.escape` on keys
// and values, an array as `key%5B%5D=value` for each item. No parameters give `path` as it is.
[[nodiscard]] std::string with_query(std::string_view path,
                                     const std::vector<std::pair<std::string, QueryValue>>& params);

// `rooms_directs_path(user_ids: [ id, ... ])`.
[[nodiscard]] std::string rooms_directs_with_users(std::span<const std::int64_t> user_ids);

}  // namespace campfire::routes
