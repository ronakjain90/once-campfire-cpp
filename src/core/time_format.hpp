// Rails time formats used by the app. Rust: crates/db/src/time.rs, crates/kit/src/clock.rs,
// crates/rails_compat/src/metadata.rs, crates/views/src/messages/support.rs.
// Rails: config/initializers/time_formats.rb and ActiveSupport Time#to_fs, #iso8601, #httpdate.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/error.hpp"
#include "core/timestamp.hpp"

namespace campfire {

// `Time#to_fs(:number)`: `%Y%m%d%H%M%S` in UTC. Example: 20240601120000.
[[nodiscard]] std::string format_to_fs_number(Timestamp t);

// `Time#to_fs(:usec)`, the `cache_version` of a record: `%Y%m%d%H%M%S` and 6 digits of microseconds.
[[nodiscard]] std::string format_cache_version(Timestamp t);

// `Time#iso8601` of a UTC time: seconds, `Z` suffix. Example: 2024-06-01T12:00:00Z.
[[nodiscard]] std::string format_iso8601(Timestamp t);

// `Time#iso8601(3)` of a UTC time. The fraction is cut, not rounded. Also the JSON form of an
// ActiveSupport::TimeWithZone. Example: 2024-06-01T12:00:00.123Z.
[[nodiscard]] std::string format_iso8601_millis(Timestamp t);

// The text that Active Record writes to SQLite: `YYYY-MM-DD HH:MM:SS`, with `.dddddd`
// (microseconds) only when the microseconds are not 0. Nanoseconds are cut.
[[nodiscard]] std::string format_db(Timestamp t);

// Reads what Rails or SQLite wrote: `YYYY-MM-DD HH:MM:SS[.fraction]`. A `T` separator, a
// trailing `Z` and a trailing ` UTC` are accepted. A fraction keeps at most 6 digits.
[[nodiscard]] std::optional<Timestamp> parse_db(std::string_view text);

// `Time#httpdate`: `Thu, 01 Jan 1970 00:00:00 GMT`.
[[nodiscard]] std::string format_httpdate(Timestamp t);

// `Time.httpdate` / `Time.rfc2822`: also accepts an optional weekday, 2 digit years, `UT`,
// `GMT`, the US zone names and numeric offsets. Returns nothing for text that is not a date.
[[nodiscard]] std::optional<Timestamp> parse_httpdate(std::string_view text);

// `time.to_fs(:epoch)`, `(time.to_f * 1000).to_i` (config/initializers/time_formats.rb). The
// conversion goes through a double, as Ruby does. It cuts some values down by 1 ms. This is
// deliberate: the client compares these numbers.
[[nodiscard]] std::int64_t epoch_ms(Timestamp t);

// Reads an RFC 3339 timestamp: `2024-06-01T12:00:00Z`, with an optional fraction (up to 9
// digits) and an offset (`Z` or `+hh:mm`). The offset is required.
[[nodiscard]] Result<Timestamp> parse_rfc3339(std::string_view text);

// `n.years.from_now`: calendar years in UTC. Feb 29 becomes Feb 28 in a year that is not a
// leap year. Returns `t` if the result is out of range.
[[nodiscard]] Timestamp years_from(Timestamp t, int years);

}  // namespace campfire
