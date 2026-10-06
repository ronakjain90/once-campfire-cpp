// Ruby and Active Support string behavior that the rich text steps need. Rust: crates/richtext/src/ruby.rs
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "compat/json.hpp"

namespace campfire::richtext {

// Reads one UTF-8 code point at `pos` and moves `pos` past it. An invalid byte gives U+FFFD.
[[nodiscard]] char32_t next_code_point(std::string_view text, std::size_t& pos) noexcept;
void append_utf8(std::string& out, char32_t code_point);

// Active Support String#blank?: empty, or only Unicode white space.
[[nodiscard]] bool is_blank(std::string_view text) noexcept;

// String#chomp(""): removes every trailing "\n" or "\r\n", but not a lone "\r".
[[nodiscard]] std::string_view chomp_newlines(std::string_view text) noexcept;
// String#chomp: removes one trailing "\r\n", "\n" or "\r".
[[nodiscard]] std::string_view chomp(std::string_view text) noexcept;

// Action View truncate(text, length:, omission:) with the default separator, before escaping.
[[nodiscard]] std::string truncate(std::string_view text, std::size_t length, std::string_view omission);

// Object#to_s of a parsed JSON value, as Nokogiri's create_element applies it to attribute values.
[[nodiscard]] std::string json_value_to_s(const compat::json::Value& value);
// Object#inspect of a parsed JSON value, in Ruby 3.4 format (`{"a" => 1}`).
[[nodiscard]] std::string json_value_inspect(const compat::json::Value& value);

}  // namespace campfire::richtext
