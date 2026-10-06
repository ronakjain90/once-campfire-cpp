// Runtime of the generated template code (tools/ctc.py). Rails: ERB output rules (`<%= %>`).
#pragma once

#include <concepts>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "core/html.hpp"
#include "core/out.hpp"
#include "views/fragment_cache.hpp"

namespace campfire::views {

// `{{ expr }}`. Escape rules are the rules of ERB:
//   SafeHtml      written as it is (ERB does not escape a SafeBuffer)
//   text          escaped
//   integer       decimal text (Ruby Integer#to_s)
//   bool          "true" or "false"
//   optional<T>   nothing if empty (Ruby nil), else the value
// Any other type is a compile error.
inline void write(Out& out, SafeHtml html) { out.append(html); }
inline void write(Out& out, std::string_view text) { html_escape(out, text); }
inline void write(Out& out, bool value) { out.append_raw(value ? "true" : "false"); }

template <std::integral T>
  requires(!std::same_as<T, bool> && !std::same_as<T, char>)
void write(Out& out, T value) {
  if constexpr (std::is_signed_v<T>) {
    out.append_int(value);
  } else {
    out.append_uint(value);
  }
}

template <class T>
void write(Out& out, const std::optional<T>& value) {
  if (value.has_value()) {
    write(out, *value);
  }
}

template <class T>
  requires(std::convertible_to<const T&, std::string_view> && !std::same_as<std::remove_cvref_t<T>, std::string_view> &&
           !std::same_as<std::remove_cvref_t<T>, SafeHtml>)
void write(Out& out, const T& text) {
  html_escape(out, std::string_view(text));
}

// `{{= expr }}`: the value must be a SafeHtml.
template <class T>
void write_safe(Out& out, const T& html) {
  static_assert(std::is_same_v<std::remove_cvref_t<T>, SafeHtml>,
                "{{= expr }} needs a SafeHtml value. Use {{ expr }} to write text with HTML escape.");
  if constexpr (std::is_same_v<std::remove_cvref_t<T>, SafeHtml>) {
    out.append(html);
  }
}

// `{% cache key %} ... {% end %}`. With the null cache the body writes straight to `out`.
template <class Key, class Body>
void cached(Out& out, const Key& key, Body&& body) {
  FragmentCache& cache = fragment_cache();
  if (!cache.enabled()) {
    body(out);
    return;
  }
  const std::string_view key_view(key);
  if (cache.read(key_view, out)) {
    return;
  }
  Out inner;
  body(inner);
  const std::string html = inner.to_string();
  cache.write(key_view, html);
  out.append(SafeHtml::trusted(html));
}

}  // namespace campfire::views
