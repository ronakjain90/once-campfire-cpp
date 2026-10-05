// Timestamps for message expiry (Rust: crates/rails_compat/src/metadata.rs, cookies.rs).
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace campfire::compat {

// An instant in UTC with nanosecond resolution.
using Timestamp = std::chrono::sys_time<std::chrono::nanoseconds>;

// Time#iso8601(3) in UTC: "2046-01-01T12:00:00.000Z". The fraction is truncated, not rounded.
std::string iso8601_millis(Timestamp time);

// Parses "YYYY-MM-DDTHH:MM:SS[.fraction](Z|+HH:MM|-HH:MM)". Digits of the fraction after the
// ninth are dropped. Returns nullopt for any other text.
std::optional<Timestamp> parse_iso8601(std::string_view text);

// `20.years.from_now` for cookies.permanent: 20 calendar years, with the day clamped to the
// end of the month (29 February becomes 28 February).
Timestamp permanent_expires_at(Timestamp now);

}  // namespace campfire::compat
