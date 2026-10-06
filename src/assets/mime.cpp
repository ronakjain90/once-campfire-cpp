// Rack::Mime and the compressible types of ActionDispatch::Static (Rust: crates/assets/src/serve.rs).
#include "assets/mime.hpp"

#include <array>
#include <string>
#include <utility>

namespace campfire::assets {

namespace {

constexpr std::array<std::pair<std::string_view, std::string_view>, 30> kTypes{{
    {".avif", "image/avif"},
    {".css", "text/css"},
    {".csv", "text/csv"},
    {".gif", "image/gif"},
    {".gz", "application/x-gzip"},
    {".htm", "text/html"},
    {".html", "text/html"},
    {".ico", "image/vnd.microsoft.icon"},
    {".jpeg", "image/jpeg"},
    {".jpg", "image/jpeg"},
    {".js", "text/javascript"},
    {".json", "application/json"},
    {".m4a", "audio/mp4a-latm"},
    {".mjs", "text/javascript"},
    {".mp3", "audio/mpeg"},
    {".mp4", "video/mp4"},
    {".ogg", "application/ogg"},
    {".otf", "font/otf"},
    {".pdf", "application/pdf"},
    {".png", "image/png"},
    {".svg", "image/svg+xml"},
    {".ttf", "font/ttf"},
    {".txt", "text/plain"},
    {".wav", "audio/x-wav"},
    {".webm", "video/webm"},
    {".webp", "image/webp"},
    {".woff", "font/woff"},
    {".woff2", "font/woff2"},
    {".xml", "application/xml"},
    {".zip", "application/zip"},
}};

}  // namespace

std::optional<std::string_view> mime_type(std::string_view extension) noexcept {
  std::string lower(extension);
  for (char& c : lower) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  for (const auto& [ext, type] : kTypes) {
    if (ext == lower) {
      return type;
    }
  }
  return std::nullopt;
}

std::string_view file_extname(std::string_view path) noexcept {
  const auto slash = path.rfind('/');
  std::string_view base = slash == std::string_view::npos ? path : path.substr(slash + 1);
  const auto first = base.find_first_not_of('.');
  if (first == std::string_view::npos) {
    return {};
  }
  base.remove_prefix(first);
  const auto dot = base.rfind('.');
  return dot == std::string_view::npos ? std::string_view{} : base.substr(dot);
}

bool compressible(std::string_view content_type) noexcept {
  return content_type.starts_with("text/") || content_type.starts_with("application/javascript") ||
         content_type.starts_with("image/svg+xml");
}

}  // namespace campfire::assets
