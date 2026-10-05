// Error type and std::expected aliases. Matches the "Errors" rules in plans/architecture.md
// section 4 and section 14.
#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace campfire {

// The kind of an error. Add a value when a caller must act on it. Use `Internal` otherwise.
enum class Errc : std::uint8_t {
  Internal,
  InvalidArgument,
  NotFound,
  Config,
  Parse,
  Io,
  Timeout,
};

[[nodiscard]] constexpr std::string_view to_string(Errc code) noexcept {
  switch (code) {
    case Errc::Internal: return "internal";
    case Errc::InvalidArgument: return "invalid_argument";
    case Errc::NotFound: return "not_found";
    case Errc::Config: return "config";
    case Errc::Parse: return "parse";
    case Errc::Io: return "io";
    case Errc::Timeout: return "timeout";
  }
  return "unknown";
}

// An error: a code that a program can test, and a message for a person.
struct Error {
  Errc code = Errc::Internal;
  std::string message;

  [[nodiscard]] bool operator==(const Error&) const = default;
};

// `Result<T>` holds a `T` or an `Error`. `Status` holds nothing or an `Error`.
template <class T>
using Result = std::expected<T, Error>;
using Status = Result<void>;

// `return fail(Errc::Parse, "bad input");` makes a failed Result of any type.
[[nodiscard]] inline std::unexpected<Error> fail(Errc code, std::string message) {
  return std::unexpected<Error>(Error{code, std::move(message)});
}

}  // namespace campfire
