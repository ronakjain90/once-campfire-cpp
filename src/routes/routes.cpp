// Path helpers of config/routes.rb (Rust: crates/routes/src/lib.rs, crates/views/src/helpers/url.rs).
#include "routes/routes.hpp"

#include <algorithm>
#include <charconv>

#include "compat/ruby.hpp"
#include "routes/query.hpp"

namespace campfire::routes {
namespace {

constexpr bool segment_safe(unsigned char c) {
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
    return true;
  }
  constexpr std::string_view kOther = "-._~!$&'()*+,;=:@";
  return kOther.find(static_cast<char>(c)) != std::string_view::npos;
}

}  // namespace

void append_segment(std::string& out, std::string_view value) {
  constexpr char kHex[] = "0123456789ABCDEF";
  for (const char ch : value) {
    const auto c = static_cast<unsigned char>(ch);
    if (segment_safe(c)) {
      out.push_back(ch);
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 15]);
    }
  }
}

void append_segment(std::string& out, std::int64_t value) {
  std::array<char, 24> buffer{};
  const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  out.append(buffer.data(), result.ptr);
}

void append_segment(std::string& out, std::uint64_t value) {
  std::array<char, 24> buffer{};
  const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  out.append(buffer.data(), result.ptr);
}

std::string fresh_user_avatar(std::string_view avatar_token, std::string_view updated_at_number) {
  std::string out = user_avatar(avatar_token);
  out += "?v=";
  out += compat::cgi_escape(updated_at_number);
  return out;
}

std::string fresh_account_logo(std::optional<std::string_view> updated_at_number,
                               std::optional<std::string_view> size) {
  std::string out = account_logo();
  char separator = '?';
  if (size) {
    out += separator;
    out += "size=";
    out += compat::cgi_escape(*size);
    separator = '&';
  }
  if (updated_at_number) {
    out += separator;
    out += "v=";
    out += compat::cgi_escape(*updated_at_number);
  }
  return out;
}

std::string with_query(std::string_view path, const std::vector<std::pair<std::string, QueryValue>>& params) {
  // Hash#to_query joins the items of an array first, then sorts the pieces of the keys.
  std::vector<std::string> pieces;
  pieces.reserve(params.size());
  for (const auto& [key, value] : params) {
    std::string piece;
    if (const auto* one = std::get_if<std::string>(&value)) {
      piece = compat::cgi_escape(key) + "=" + compat::cgi_escape(*one);
    } else {
      const std::string array_key = compat::cgi_escape(key + "[]");
      for (const std::string& item : std::get<std::vector<std::string>>(value)) {
        if (!piece.empty()) {
          piece += '&';
        }
        piece += array_key + "=" + compat::cgi_escape(item);
      }
    }
    if (!piece.empty()) {
      pieces.push_back(std::move(piece));
    }
  }
  std::ranges::sort(pieces);
  std::string out(path);
  char separator = '?';
  for (const std::string& piece : pieces) {
    out += separator;
    out += piece;
    separator = '&';
  }
  return out;
}

std::string rooms_directs_with_users(std::span<const std::int64_t> user_ids) {
  std::vector<std::string> ids;
  ids.reserve(user_ids.size());
  for (const std::int64_t id : user_ids) {
    ids.push_back(std::to_string(id));
  }
  std::vector<std::pair<std::string, QueryValue>> params;
  params.emplace_back("user_ids", std::move(ids));
  return with_query(rooms_directs(), params);
}

}  // namespace campfire::routes
