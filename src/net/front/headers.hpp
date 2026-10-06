// Header map rules of the Rust port. Rust: the `http` crate's HeaderMap, as hyper writes it.
#pragma once

#include <string_view>

#include "net/response.hpp"

namespace campfire::net::front {

// `HeaderMap::remove`. The map keeps one entry for each name. Removing an entry moves the last
// entry into its place, so the order of the other headers changes. The wire order depends on
// this, and the diff sweep compares it. The values of one name stay together, at the place of
// the first one. Returns true if a header was removed.
bool remove_header(Response& response, std::string_view name);

// `HeaderMap::insert`: the first header with this name gets the value, the other headers with
// this name go, and a new header goes last.
void insert_header(Response& response, std::string_view name, std::string_view value);

}  // namespace campfire::net::front
