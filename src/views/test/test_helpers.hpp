// Helpers that only the ctc tests use. They show the call shapes of `{% call %}` and `{% wrap %}`.
#pragma once

#include <string_view>

#include "core/html.hpp"
#include "core/out.hpp"

namespace campfire::views::helpers {

// `{% call badge(name) %}`
inline void badge(Out& out, std::string_view name) {
  out.append(SafeHtml::literal("<b>"));
  html_escape(out, name);
  out.append(SafeHtml::literal("</b>"));
}

// `{% wrap box(class_name) |kind| %} ... {% end %}`
template <class Body>
void box(Out& out, std::string_view class_name, Body&& body) {
  out.append(SafeHtml::literal("<div class=\""));
  html_escape(out, class_name);
  out.append(SafeHtml::literal("\">"));
  body(out);
  out.append(SafeHtml::literal("</div>"));
}

// `{% wrap with_kind(class_name) |kind| %} ... {% end %}`: the body gets one argument.
template <class Body>
void with_kind(Out& out, std::string_view class_name, Body&& body) {
  body(out, class_name);
}

}  // namespace campfire::views::helpers
