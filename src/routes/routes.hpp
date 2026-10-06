// Path helpers of config/routes.rb (Rust: crates/routes/src/lib.rs). The names are the Rails
// `*_path` helper names without `_path`: `routes::room_message(room_id, id)`.
#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace campfire::routes {

// A route pattern as a template argument: "/rooms/:room_id/messages/:id".
template <std::size_t N>
struct FixedString {
  char text[N]{};
  // NOLINTNEXTLINE(google-explicit-constructor): a string literal makes a pattern.
  constexpr FixedString(const char (&s)[N]) noexcept { std::copy_n(s, N, text); }
  [[nodiscard]] constexpr std::string_view view() const noexcept { return {text, N - 1}; }
};

// Appends one path segment value as Rails escapes it (ActionDispatch::Journey::Router::Utils
// .escape_segment): letters, digits and `-._~!$&'()*+,;=:@` stay, other bytes become %XX.
void append_segment(std::string& out, std::string_view value);
void append_segment(std::string& out, std::int64_t value);
void append_segment(std::string& out, std::uint64_t value);

namespace detail {

template <class T>
concept SegmentText = std::convertible_to<const T&, std::string_view> && !std::integral<T>;

// A pattern split at its `:param` tokens: literal[0] param literal[1] param ... literal[n].
template <FixedString P>
struct Pattern {
  static consteval std::size_t count_params() {
    std::size_t n = 0;
    for (char c : P.view()) {
      n += c == ':' ? 1 : 0;
    }
    return n;
  }
  static constexpr std::size_t kParams = count_params();

  static consteval std::array<std::string_view, kParams + 1> split() {
    std::array<std::string_view, kParams + 1> parts{};
    const std::string_view text = P.view();
    std::size_t start = 0;
    std::size_t index = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (text[i] != ':') {
        continue;
      }
      parts[index++] = text.substr(start, i - start);
      std::size_t j = i + 1;
      while (j < text.size() &&
             (text[j] == '_' || (text[j] >= 'a' && text[j] <= 'z') || (text[j] >= '0' && text[j] <= '9'))) {
        ++j;
      }
      start = j;
      i = j - 1;
    }
    parts[index] = text.substr(start);
    return parts;
  }
  static constexpr std::array<std::string_view, kParams + 1> kLiterals = split();
};

template <class T>
void append_arg(std::string& out, const T& value) {
  if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
    append_segment(out, static_cast<std::int64_t>(value));
  } else if constexpr (std::is_integral_v<T>) {
    append_segment(out, static_cast<std::uint64_t>(value));
  } else {
    static_assert(SegmentText<T>, "a route argument must be an integer or text");
    append_segment(out, std::string_view(value));
  }
}

}  // namespace detail

// Fills a pattern with the arguments, in order. The count is checked at compile time.
template <FixedString P, class... Args>
[[nodiscard]] std::string path(const Args&... args) {
  using Pat = detail::Pattern<P>;
  static_assert(sizeof...(Args) == Pat::kParams, "wrong number of arguments for the route");
  std::string out;
  out.reserve(P.view().size() + 12 * sizeof...(Args));
  out.append(Pat::kLiterals[0]);
  std::size_t index = 0;
  ((detail::append_arg(out, args), out.append(Pat::kLiterals[++index])), ...);
  return out;
}

// A route whose first param (`user_id`) has the default "me".
template <FixedString P, class... Args>
[[nodiscard]] std::string path_with_default_user(const Args&... args) {
  if constexpr (sizeof...(Args) == detail::Pattern<P>::kParams) {
    return path<P>(args...);
  } else {
    return path<P>("me", args...);
  }
}

#define CF_ROUTE(name, pattern, endpoint)               \
  template <class... Args>                              \
  [[nodiscard]] std::string name(const Args&... args) { \
    return path<pattern>(args...);                      \
  }
#define CF_ROUTE_ME(name, pattern, endpoint)            \
  template <class... Args>                              \
  [[nodiscard]] std::string name(const Args&... args) { \
    return path_with_default_user<pattern>(args...);    \
  }
#include "routes/routes.def"
#undef CF_ROUTE_ME
#undef CF_ROUTE

// One row of the route list: used by tests and by tools.
struct NamedRoute {
  std::string_view name;
  std::string_view pattern;
  std::string_view endpoint;
};

inline constexpr NamedRoute kNamedRoutes[] = {
#define CF_ROUTE(name, pattern, endpoint) {#name, pattern, endpoint},
#define CF_ROUTE_ME(name, pattern, endpoint) {#name, pattern, endpoint},
#include "routes/routes.def"
#undef CF_ROUTE_ME
#undef CF_ROUTE
};

// `direct :fresh_user_avatar`: the avatar path with the `v` cache buster
// (`user.updated_at.to_fs(:number)`). `avatar_token` is the signed token of the user.
[[nodiscard]] std::string fresh_user_avatar(std::string_view avatar_token, std::string_view updated_at_number);

// `direct :fresh_account_logo`: `account_logo_path(v:, size:)`. A missing option is left out,
// as Rails does for nil. The keys are sorted, as `Hash#to_query` sorts them: `size`, then `v`.
[[nodiscard]] std::string fresh_account_logo(std::optional<std::string_view> updated_at_number = std::nullopt,
                                             std::optional<std::string_view> size = std::nullopt);

}  // namespace campfire::routes
