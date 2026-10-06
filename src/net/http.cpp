// Request view and HTTP types. Rust: crates/kit/src/front/conn.rs.
#include "net/http.hpp"

namespace campfire::net {

Method parse_method(std::string_view text) noexcept {
  if (text == "GET") return Method::Get;
  if (text == "HEAD") return Method::Head;
  if (text == "POST") return Method::Post;
  if (text == "PUT") return Method::Put;
  if (text == "PATCH") return Method::Patch;
  if (text == "DELETE") return Method::Delete;
  if (text == "OPTIONS") return Method::Options;
  return Method::Other;
}

std::string_view method_name(Method method) noexcept {
  switch (method) {
    case Method::Get: return "GET";
    case Method::Head: return "HEAD";
    case Method::Post: return "POST";
    case Method::Put: return "PUT";
    case Method::Patch: return "PATCH";
    case Method::Delete: return "DELETE";
    case Method::Options: return "OPTIONS";
    case Method::Other: break;
  }
  return "";
}

bool iequals(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    char x = a[i];
    char y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

std::string_view Request::header(std::string_view lower_name) const noexcept {
  for (const Header& h : headers) {
    if (iequals(h.name, lower_name)) return h.value;
  }
  return {};
}

bool Request::has_header(std::string_view lower_name) const noexcept {
  for (const Header& h : headers) {
    if (iequals(h.name, lower_name)) return true;
  }
  return false;
}

}  // namespace campfire::net
