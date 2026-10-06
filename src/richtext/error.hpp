// What Ruby would have raised while a rich text step ran. Rust: crates/richtext/src/lib.rs (Error)
#pragma once

#include <expected>
#include <string>
#include <utility>

#include "richtext/dom.hpp"

namespace campfire::richtext {

// Callers copy what the Rails code does with the exception: `message_presentation` rescues each
// exception and renders "", unless the log line of the rescue raises too (Unrenderable).
struct Error {
  enum class Kind : std::uint8_t {
    Parse,         // Nokogiri raised ArgumentError: the depth or attribute limit of Gumbo
    Raised,        // Ruby raised an exception: `message` names it
    Unrenderable,  // Ruby raised an exception with a message that is not valid UTF-8
  };
  Kind kind = Kind::Raised;
  ParseError parse = ParseError::TreeTooDeep;  // Only for Kind::Parse
  std::string message;

  static Error from_parse(ParseError error) { return {Kind::Parse, error, std::string(to_string(error))}; }
  static Error raised(std::string message) { return {Kind::Raised, ParseError::TreeTooDeep, std::move(message)}; }
  static Error unrenderable(std::string message) {
    return {Kind::Unrenderable, ParseError::TreeTooDeep, std::move(message)};
  }
};

template <class T>
using Result = std::expected<T, Error>;

// For `std::unexpected(...)` on a parse error: `return fail(dom.error())`.
inline std::unexpected<Error> fail(ParseError error) {
  return std::unexpected(Error::from_parse(error));
}

}  // namespace campfire::richtext
