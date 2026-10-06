// `link_to`, `link_to_if`, `mail_to` (ActionView UrlHelper). Rust: crates/views/src/helpers/links.rs.
#pragma once

#include <string_view>

#include "views/helpers/tag.hpp"

namespace campfire::views::helpers {

// `link_to(url, options) do ... end`: `href` goes after the given options.
template <BodyFn Body>
void link_to(Out& out, std::string_view url, Attrs attrs, Body&& body) {
  attrs.set("href", Value(url));
  content_tag(out, "a", attrs, std::forward<Body>(body));
}

// `link_to(url, options) { safe content }`.
void link_to(Out& out, std::string_view url, Attrs attrs, SafeHtml content);

// `link_to(text, url, options)` with plain text.
void link_to_text(Out& out, std::string_view text, std::string_view url, Attrs attrs);

// `link_to_if(condition, name, url, options)`: only the escaped name when the condition is false.
void link_to_if(Out& out, bool condition, std::string_view text, std::string_view url, Attrs attrs);

// `mail_to(email)`: `ERB::Util.url_encode` of the address (keeping "@") in the href.
void mail_to(Out& out, std::string_view email);

}  // namespace campfire::views::helpers
