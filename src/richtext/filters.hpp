// ContentFilters::SanitizeTags and SanitizeAttributes for bodies without attachments. Rails: app/helpers/content_filters/*.rb. Rust: crates/richtext/src/filters.rs
#pragma once

#include <expected>
#include <string>
#include <string_view>

#include "richtext/dom.hpp"

namespace campfire::richtext {

// Ruby's String#strip: removes NUL and ASCII whitespace at both ends.
[[nodiscard]] std::string_view ruby_strip(std::string_view text) noexcept;

// ContentFilters::SanitizeTags: removes every element that is not in its list, with its contents.
void sanitize_tags(Dom& dom);

// `TextMessagePresentationFilters.apply(content).to_html` for a body that has no Action Text
// attachments: parse (the body is stripped first), SanitizeTags, SanitizeAttributes, serialize.
// The attachment steps (RemoveSoloUnfurledLinkText, canonicalization) belong to task T10.
[[nodiscard]] std::expected<std::string, ParseError> filter_message_html(std::string_view body);

}  // namespace campfire::richtext
