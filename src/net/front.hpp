// Headers of the front server. Rust: crates/kit/src/front/handler.rs, compression.rs (add_vary), conn.rs (Date).
#pragma once

#include <ctime>
#include <string_view>

#include "net/http.hpp"
#include "net/response.hpp"

namespace campfire::net {

// The text of an HTTP date: "Mon, 05 Oct 2026 16:31:10 GMT" (RFC 9110). `buffer` holds the text.
[[nodiscard]] std::string_view format_http_date(std::time_t when, char (&buffer)[32]) noexcept;

// The date of now, on the real clock (the frozen clock does not change it).
[[nodiscard]] std::string_view http_date_now(char (&buffer)[32]) noexcept;

// `shouldCacheRequest`: GET or HEAD, no upgrade, no range, a short URI.
[[nodiscard]] bool should_cache_request(const Request& request) noexcept;

// Adds what the front adds to a response from the app, in the order that the Rust port uses:
// "vary: Accept-Encoding", "x-cache" and "date". Removes the headers that a status cannot carry
// (Go's `suppressedHeaders`). The front cache and the compression are not part of it.
void apply_front_headers(const Request& request, Response& response);

// What Go's server does to a response with no body: drop the headers that the status forbids.
void suppress_bodiless_headers(Response& response);

}  // namespace campfire::net
