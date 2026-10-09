// Rack::MethodOverride (Rails: the middleware before the router; Rust: method_override in crates/kit/src/adapter.rs).
#include "req/method_override.hpp"

#include <array>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "req/body.hpp"
#include "req/multipart.hpp"
#include "req/query.hpp"

namespace campfire::req {
namespace {

// Rack::MethodOverride::HTTP_METHODS
constexpr std::array<std::string_view, 9> kMethods = {"GET",     "HEAD",  "PUT",  "POST",  "DELETE",
                                                      "OPTIONS", "PATCH", "LINK", "UNLINK"};

std::optional<std::string_view> known_method(std::string_view text) {
  for (const std::string_view method : kMethods) {
    if (method.size() != text.size()) continue;
    bool same = true;
    for (std::size_t i = 0; i < text.size() && same; ++i) {
      same = std::toupper(static_cast<unsigned char>(text[i])) == method[i];
    }
    if (same) return method;
  }
  return std::nullopt;
}

bool iequals_ascii(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  }
  return true;
}

// The last `_method` pair of an urlencoded body, as `req.POST["_method"]` gives it.
std::optional<std::string> form_param(std::string_view body) {
  auto pairs = form_pairs(body);
  if (!pairs) return std::nullopt;
  std::optional<std::string> value;
  for (const RawPair& pair : *pairs) {
    if (pair.key == "_method") value = pair.has_value ? pair.value : std::string();
  }
  return value;
}

// The last text part named `_method` of a multipart body. The body is in memory, so a scan finds the part
// without the temp files of MultipartParser.
std::optional<std::string> multipart_param(std::string_view content_type, std::string_view body) {
  const auto boundary = parse_boundary(content_type);
  if (!boundary) return std::nullopt;
  const std::string open = "--" + *boundary;
  const std::string delimiter = "\r\n" + open;
  std::size_t at = 0;
  if (body.substr(0, open.size()) != open) {
    at = body.find(delimiter);
    if (at == std::string_view::npos) return std::nullopt;
    at += 2;
  }
  std::optional<std::string> value;
  while (true) {
    at += open.size();
    if (body.substr(at, 2) == "--") break;  // the closing delimiter
    const std::size_t head_start = body.find("\r\n", at);
    if (head_start == std::string_view::npos) break;
    const std::size_t head_end = body.find("\r\n\r\n", head_start);
    if (head_end == std::string_view::npos) break;
    const std::size_t data_start = head_end + 4;
    const std::size_t data_end = body.find(delimiter, data_start);
    if (data_end == std::string_view::npos) break;
    std::string_view head = body.substr(head_start + 2, head_end - head_start - 2);
    while (!head.empty()) {
      const std::size_t line_end = head.find("\r\n");
      const std::string_view line = head.substr(0, line_end);
      head = line_end == std::string_view::npos ? std::string_view() : head.substr(line_end + 2);
      const std::size_t colon = line.find(':');
      if (colon == std::string_view::npos || !iequals_ascii(line.substr(0, colon), "content-disposition")) continue;
      const PartHead part = parse_disposition(line.substr(colon + 1));
      if (part.name == "_method" && !part.filename) value = std::string(body.substr(data_start, data_end - data_start));
    }
    at = data_end + 2;
  }
  return value;
}

}  // namespace

std::optional<std::string_view> method_override(std::optional<std::string_view> content_type, std::string_view body,
                                                std::optional<std::string_view> override_header) {
  if (content_type && content_type->empty()) content_type = std::nullopt;
  const auto media = media_type(content_type);
  // `req.form_data? || req.parseable_data?`
  const bool multipart =
      media && (*media == "multipart/form-data" || *media == "multipart/related" || *media == "multipart/mixed");
  const bool urlencoded = !content_type || (media && *media == "application/x-www-form-urlencoded");

  std::optional<std::string> param;
  // Most posts carry no `_method`. A body without the bytes "method" cannot hold it (unless it %-encodes those
  // letters, which no browser does), so such a body is not parsed here.
  if ((multipart || urlencoded) && body.find("method") != std::string_view::npos) {
    param = multipart ? multipart_param(*content_type, body) : form_param(body);
  }
  if (param) return known_method(*param);
  if (override_header) return known_method(*override_header);
  return std::nullopt;
}

}  // namespace campfire::req
