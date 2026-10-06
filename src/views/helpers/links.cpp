// `link_to`, `link_to_if`, `mail_to` (Rust: crates/views/src/helpers/links.rs).
#include "views/helpers/links.hpp"

#include "compat/ruby.hpp"

namespace campfire::views::helpers {

void link_to(Out& out, std::string_view url, Attrs attrs, SafeHtml content) {
  link_to(out, url, std::move(attrs), [&](Out& o) { o.append(content); });
}

void link_to_text(Out& out, std::string_view text, std::string_view url, Attrs attrs) {
  link_to(out, url, std::move(attrs), [&](Out& o) { html_escape(o, text); });
}

void link_to_if(Out& out, bool condition, std::string_view text, std::string_view url, Attrs attrs) {
  if (condition) {
    link_to_text(out, text, url, std::move(attrs));
  } else {
    html_escape(out, text);
  }
}

void mail_to(Out& out, std::string_view email) {
  std::string encoded = compat::url_encode(email);
  for (std::size_t at = encoded.find("%40"); at != std::string::npos; at = encoded.find("%40", at + 1)) {
    encoded.replace(at, 3, "@");
  }
  link_to_text(out, email, "mailto:" + encoded, Attrs{});
}

}  // namespace campfire::views::helpers
