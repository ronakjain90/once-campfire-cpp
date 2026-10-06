// ActionDispatch::Static over Rack::Files (Rails: actionpack static.rb; Rust: crates/assets/src/serve.rs).
#include "assets/static_files.hpp"

#include <algorithm>
#include <cctype>

#include "assets/mime.hpp"
#include "compat/ruby.hpp"

namespace campfire::assets {

namespace {

// The last `config.public_file_server.headers` assignment of production.rb wins.
constexpr std::string_view kCacheControl = "public, max-age=2592000";
// Rack::Files::MULTIPART_BOUNDARY
constexpr std::string_view kBoundary = "AaB03x";

int hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

// URI::RFC2396_Parser#unescape: a %XX sequence becomes a byte. All other bytes stay.
std::string unescape(std::string_view path) {
  std::string out;
  for (std::size_t i = 0; i < path.size(); ++i) {
    // Ruby needs a byte after the two hex digits (the Rust port copies this).
    if (path[i] == '%' && i + 2 < path.size() && hex_value(path[i + 1]) >= 0 && hex_value(path[i + 2]) >= 0) {
      out += static_cast<char>(hex_value(path[i + 1]) * 16 + hex_value(path[i + 2]));
      i += 2;
    } else {
      out += path[i];
    }
  }
  return out;
}

// FileHandler#clean_path: chomp("/"), percent-decode, reject NUL, then Rack::Utils.clean_path_info.
std::optional<std::string> clean_path(std::string_view path_info) {
  if (path_info.ends_with('/')) {
    path_info.remove_suffix(1);
  }
  const std::string path = unescape(path_info);
  if (path.find('\0') != std::string::npos) {
    return std::nullopt;
  }
  std::vector<std::string_view> clean;
  bool first_empty = false;
  std::size_t index = 0;
  for (std::size_t pos = 0; pos <= path.size(); ++index) {
    const std::size_t slash = std::min(path.find('/', pos), path.size());
    const std::string_view part = std::string_view(path).substr(pos, slash - pos);
    if (index == 0) {
      first_empty = part.empty();
    }
    if (part == "..") {
      if (!clean.empty()) {
        clean.pop_back();
      }
    } else if (!part.empty() && part != ".") {
      clean.push_back(part);
    }
    pos = slash + 1;
  }
  std::string out = first_empty ? "/" : "";
  for (std::size_t i = 0; i < clean.size(); ++i) {
    if (i != 0) {
      out += '/';
    }
    out += clean[i];
  }
  return out;
}

}  // namespace

std::optional<std::string_view> StaticResponse::header(std::string_view name) const {
  for (const auto& [n, v] : headers) {
    if (n.size() == name.size() && std::equal(n.begin(), n.end(), name.begin(), [](char a, char b) {
          return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
        })) {
      return v;
    }
  }
  return std::nullopt;
}

namespace {

struct Found {
  StaticFile file;
  std::string content_type;
};

// FileHandler#find_file. A path with no known extension also tries ".html" and "/index.html".
std::optional<Found> find_static(std::string_view path_info) {
  const auto path = clean_path(path_info);
  if (!path) {
    return std::nullopt;
  }
  const std::string_view extension = file_extname(*path);
  const auto type = mime_type(extension);
  std::vector<std::pair<std::string, std::string>> candidates{{*path, std::string(type.value_or("text/plain"))}};
  if (!type && extension != ".html") {
    candidates.emplace_back(*path + ".html", "text/html");
    candidates.emplace_back(*path + "/index.html", "text/html");
  }
  for (auto& [candidate, content_type] : candidates) {
    if (auto file = find_file(candidate)) {
      return Found{*file, std::move(content_type)};
    }
  }
  return std::nullopt;
}

}  // namespace

std::optional<StaticResponse> serve(const StaticRequest& request) {
  if (request.method != "GET" && request.method != "HEAD") {
    return std::nullopt;
  }
  auto found = find_static(request.path);
  if (!found) {
    return std::nullopt;
  }
  const std::string_view file = found->file.identity;
  const std::string& last_modified = built_at_http_date();

  StaticResponse response;
  if (request.if_modified_since == last_modified) {
    response.status = 304;
    return response;
  }
  response.file = found->file;
  response.borrowed_ = file;
  const std::size_t size = file.size();
  response.headers = {
      {"last-modified", last_modified}, {"content-type", ""}, {"Cache-Control", std::string(kCacheControl)}};

  const auto ranges = compat::byte_ranges(request.range, size);
  if (!ranges) {
    // Serve the whole file.
  } else if (ranges->empty()) {
    const std::string_view message = "Byte range unsatisfiable\n";
    response.headers = {{"content-type", ""},
                        {"content-length", std::to_string(message.size())},
                        {"x-cascade", "pass"},
                        {"content-range", "bytes */" + std::to_string(size)}};
    response.status = 416;
    response.file.reset();
    response.owned_ = std::string(message);
  } else if (ranges->size() == 1) {
    const auto [first, last] = (*ranges)[0];
    response.headers.emplace_back(
        "content-range", "bytes " + std::to_string(first) + "-" + std::to_string(last) + "/" + std::to_string(size));
    response.status = 206;
    response.borrowed_ = file.substr(first, last - first + 1);
    response.file.reset();
  } else {
    std::string multipart;
    for (const auto& r : *ranges) {
      multipart += "\r\n--" + std::string(kBoundary) + "\r\ncontent-type: " + found->content_type +
                   "\r\ncontent-range: bytes " + std::to_string(r.first) + "-" + std::to_string(r.last) + "/" +
                   std::to_string(size) + "\r\n\r\n";
      multipart.append(file.substr(r.first, r.last - r.first + 1));
    }
    multipart += "\r\n--" + std::string(kBoundary) + "--\r\n";
    response.headers[1].second = "multipart/byteranges; boundary=" + std::string(kBoundary);
    response.status = 206;
    response.owned_ = std::move(multipart);
    response.file.reset();
  }

  if (response.status != 416) {
    response.headers.emplace_back("content-length", std::to_string(response.body().size()));
  }
  // FileHandler#serve: `headers.update(content_headers)`. Static overrides the multipart type.
  for (auto& header : response.headers) {
    if (header.first == "content-type") {
      header.second = found->content_type;
    }
  }
  if (request.method == "HEAD") {
    response.borrowed_ = {};
    response.owned_.reset();
  }
  return response;
}

}  // namespace campfire::assets
