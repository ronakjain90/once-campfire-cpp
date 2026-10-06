// rails_autolink 1.1.8 `auto_link(text, html: {target: "_blank"}, sanitize_options: ...)`, as
// MessagesHelper#message_presentation calls it. Rust: crates/richtext/src/autolink.rs
#pragma once

#include <expected>
#include <string>
#include <string_view>

#include "richtext/dom.hpp"
#include "richtext/sanitizer.hpp"

namespace campfire::richtext {

// Ruby's `\p{Word}` (Letter, Mark, Decimal_Number, Connector_Punctuation).
[[nodiscard]] bool is_word_char(char32_t code_point) noexcept;

// Works on the serialized HTML with the same patterns as the Ruby code, so the exact text of the
// earlier steps matters. One deliberate difference closes a stored XSS in rails_autolink (README
// "Known differences"): the sanitized HTML is serialized with `<` and `>` escaped in attribute
// values. Nokogiri writes them raw, so a URL after a `>` in a `title` looked like text to
// `auto_linked?`, and the `<a href="...">` put there closed the attribute and turned the rest of its
// value into markup. With them escaped, each `<` and `>` in the text belongs to a tag, and the links
// go only between tags.
[[nodiscard]] std::expected<std::string, ParseError> auto_link(std::string_view text, const SafeList& sanitize_options);

}  // namespace campfire::richtext
