// Rails::HTML5::SafeListSanitizer with Rails::HTML::PermitScrubber over Loofah. Rust: crates/richtext/src/sanitizer.rs
#pragma once

#include <algorithm>
#include <expected>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "richtext/dom.hpp"

namespace campfire::richtext {

// A set of names with fast lookup. Built once, read many times.
class NameSet {
 public:
  NameSet() = default;
  NameSet(std::initializer_list<std::string_view> names) : names_(names) { finish(); }
  void add(std::initializer_list<std::string_view> names) {
    names_.insert(names_.end(), names.begin(), names.end());
    finish();
  }
  void add(const NameSet& other) {
    names_.insert(names_.end(), other.names_.begin(), other.names_.end());
    finish();
  }
  [[nodiscard]] bool contains(std::string_view name) const {
    return std::binary_search(names_.begin(), names_.end(), name);
  }

 private:
  void finish() {
    std::sort(names_.begin(), names_.end());
    names_.erase(std::unique(names_.begin(), names_.end()), names_.end());
  }
  std::vector<std::string_view> names_;
};

// A tag and attribute allowlist, as passed to `sanitize(html, tags:, attributes:)`.
struct SafeList {
  NameSet tags;
  NameSet attributes;

  // Action View `sanitize(html)` with no options: the class-level defaults.
  static const SafeList& defaults();
  // ActionText::ContentHelper allowed tags and attributes as Campfire configures them.
  static const SafeList& action_text();
  // ContentFilters::SanitizeAttributes: SanitizeTags' tags, Action Text's attributes plus class.
  static const SafeList& content_filter();
  // MessagesHelper::AUTO_LINK_ALLOWED_TAGS and AUTO_LINK_ALLOWED_ATTRIBUTES.
  static const SafeList& auto_link();
};

// ContentFilters::SanitizeTags::ALLOWED_TAGS
const NameSet& sanitize_tags_allowed_tags();

// Scrubs the fragment in place, bottom up, as Loofah::Scrubber#traverse_conditionally_bottom_up.
void scrub(Dom& dom, const SafeList& list);

// SafeListSanitizer#sanitize(html, tags:, attributes:). An empty input gives an empty output
// without a parse.
[[nodiscard]] std::expected<std::string, ParseError> sanitize(std::string_view html, const SafeList& list);

// `sanitize`, serialized with `<` and `>` escaped in attribute values (Rust: the deliberate difference
// for autolink). The DOM is the same as the one `sanitize` writes.
[[nodiscard]] std::expected<std::string, ParseError> sanitize_with_escaped_attribute_brackets(std::string_view html,
                                                                                              const SafeList& list);

// Loofah::HTML5::Scrub.allowed_uri?
[[nodiscard]] bool allowed_uri(std::string_view uri);

// CGI.unescapeHTML
[[nodiscard]] std::string cgi_unescape_html(std::string_view text);

// Unicode White_Space, as Ruby's [[:space:]] and Rust's char::is_whitespace.
[[nodiscard]] bool is_unicode_space(char32_t code_point) noexcept;

}  // namespace campfire::richtext
