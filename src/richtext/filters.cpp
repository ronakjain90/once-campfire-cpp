// Rails: app/helpers/content_filters/sanitize_tags.rb, sanitize_attributes.rb. Rust: crates/richtext/src/filters.rs
#include "richtext/filters.hpp"

#include "richtext/sanitizer.hpp"

namespace campfire::richtext {

std::string_view ruby_strip(std::string_view text) noexcept {
  const auto is_space = [](char c) { return c == '\0' || c == ' ' || (c >= '\t' && c <= '\r'); };
  while (!text.empty() && is_space(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && is_space(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

namespace {

void remove_disallowed(Dom& dom, Node* parent, const NameSet& allowed) {
  for (Node* child = parent->first_child; child != nullptr;) {
    Node* next = child->next;
    if (child->is_element() && !allowed.contains(child->name)) {
      dom.detach(child);
    } else {
      remove_disallowed(dom, child, allowed);
    }
    child = next;
  }
}

}  // namespace

void sanitize_tags(Dom& dom) {
  remove_disallowed(dom, dom.root(), sanitize_tags_allowed_tags());
}

std::expected<std::string, ParseError> filter_message_html(std::string_view body) {
  auto dom = parse_fragment(ruby_strip(body));
  if (!dom) {
    return std::unexpected(dom.error());
  }
  sanitize_tags(*dom);
  auto sanitized = sanitize(to_html(dom->root()), SafeList::content_filter());
  if (!sanitized) {
    return std::unexpected(sanitized.error());
  }
  // The filter returns a string, and Action Text wraps it in a new fragment.
  auto wrapped = parse_fragment(ruby_strip(*sanitized));
  if (!wrapped) {
    return std::unexpected(wrapped.error());
  }
  return to_html(wrapped->root());
}

}  // namespace campfire::richtext
