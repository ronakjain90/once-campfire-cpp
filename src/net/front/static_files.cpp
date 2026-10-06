// ActionDispatch::Static and Rack::Deflater for the asset table. Rust: crates/campfire/src/app.rs,
// crates/kit/src/deflater.rs.
#include "net/front/static_files.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

#include "assets/assets.hpp"
#include "assets/static_files.hpp"
#include "net/front/compress.hpp"
#include "net/front/headers.hpp"

namespace campfire::net::front {

namespace {

struct Accepted {
  std::string_view coding;
  double quality;
};

std::string_view trim(std::string_view text) noexcept {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) text.remove_prefix(1);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) text.remove_suffix(1);
  return text;
}

// Rack::Request#accept_encoding (parse_http_accept_header).
std::vector<Accepted> parse_accept_encoding(std::string_view header) {
  std::vector<Accepted> items;
  std::size_t begin = 0;
  while (begin <= header.size()) {
    std::size_t end = header.find(',', begin);
    if (end == std::string_view::npos) end = header.size();
    const std::string_view part = trim(header.substr(begin, end - begin));
    begin = end + 1;
    if (part.empty()) continue;
    const std::size_t semicolon = part.find(';');
    const std::string_view attribute = trim(part.substr(0, semicolon));
    double q = 1.0;
    if (semicolon != std::string_view::npos) {
      const std::string_view parameters = trim(part.substr(semicolon + 1));
      if (parameters.starts_with("q=")) {
        std::string_view digits = parameters.substr(2);
        std::size_t n = 0;
        while (n < digits.size() && ((digits[n] >= '0' && digits[n] <= '9') || digits[n] == '.')) ++n;
        digits = digits.substr(0, n);
        if (!digits.empty()) q = std::strtod(std::string(digits).c_str(), nullptr);
      }
    }
    items.push_back({attribute, q});
  }
  return items;
}

// Rack::Utils.select_best_encoding over ["gzip", "identity"]. Returns "" for none.
std::string_view select_best_encoding(const std::vector<Accepted>& all) {
  static constexpr std::string_view kAvailable[] = {"gzip", "identity"};
  const std::size_t count = std::min<std::size_t>(all.size(), 16);
  struct Item {
    std::string_view coding;
    double quality;
    std::size_t preference;
  };
  std::vector<Item> expanded;
  bool wildcard_seen = false;
  for (std::size_t i = 0; i < count; ++i) {
    const auto& [m, q] = all[i];
    std::size_t preference = 2;
    for (std::size_t k = 0; k < 2; ++k) {
      if (kAvailable[k] == m) preference = k;
    }
    if (m == "*") {
      if (!wildcard_seen) {
        for (const std::string_view a : kAvailable) {
          const bool listed = std::any_of(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(count),
                                          [&](const Accepted& x) { return x.coding == a; });
          if (!listed) expanded.push_back({a, q, preference});
        }
        wildcard_seen = true;
      }
    } else {
      expanded.push_back({m, q, preference});
    }
  }
  std::vector<Item> sorted = expanded;
  std::stable_sort(sorted.begin(), sorted.end(), [](const Item& a, const Item& b) {
    if (a.quality != b.quality) return a.quality > b.quality;
    return a.preference < b.preference;
  });
  std::vector<std::string_view> candidates;
  for (const Item& item : sorted) candidates.push_back(item.coding);
  if (std::find(candidates.begin(), candidates.end(), "identity") == candidates.end()) candidates.push_back("identity");
  for (const Item& item : expanded) {
    if (item.quality == 0.0) std::erase(candidates, item.coding);
  }
  for (const std::string_view candidate : candidates) {
    for (const std::string_view a : kAvailable) {
      if (a == candidate) return a;
    }
  }
  return {};
}

bool has_word(std::string_view haystack, std::string_view word) {
  const auto is_word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; };
  for (std::size_t at = haystack.find(word); at != std::string_view::npos; at = haystack.find(word, at + 1)) {
    const bool before = at > 0 && is_word(haystack[at - 1]);
    const bool after = at + word.size() < haystack.size() && is_word(haystack[at + word.size()]);
    if (!before && !after) return true;
  }
  return false;
}

// Rack::Deflater#should_deflate? for a static response (the length is set by the app).
bool should_deflate(const Response& response) {
  const int status = response.status;
  if ((status >= 100 && status < 200) || status == 204 || status == 304) return false;
  if (has_word(response.get("cache-control"), "no-transform")) return false;
  const std::string_view encoding = response.get("content-encoding");
  if (!encoding.empty() && !has_word(encoding, "identity")) return false;
  return response.get("content-length") != "0";
}

std::string_view lower_copy(Arena& arena, std::string_view name) {
  if (std::none_of(name.begin(), name.end(), [](char c) { return c >= 'A' && c <= 'Z'; })) return name;
  std::string lowered(name);
  for (char& c : lowered) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return arena.copy(lowered);
}

}  // namespace

std::optional<Response> serve_static(Ctx& ctx) {
  const Request& request = ctx.request();
  if (request.method != Method::Get && request.method != Method::Head) return std::nullopt;
  assets::StaticRequest query;
  query.method = request.method_text;
  query.path = request.path;
  if (request.has_header("range")) query.range = request.header("range");
  if (request.has_header("if-modified-since")) query.if_modified_since = request.header("if-modified-since");
  std::optional<assets::StaticResponse> served = assets::serve(query);
  if (!served) return std::nullopt;
  Arena& arena = ctx.arena();

  Response response = ctx.response(served->status);
  for (const auto& [name, value] : served->headers) response.add(lower_copy(arena, name), arena.copy(value));
  // The Rust app builds a 304 with a length header that the front drops at the end: the drop
  // moves the last header into its place, which gives the order of the headers on the wire.
  if (served->status == 304) response.add("content-length", "0");
  std::string_view body = served->body();
  const bool head = request.method == Method::Head;
  const std::string_view full_gzip = served->file && served->status == 200 ? served->file->gzip : std::string_view{};

  if (!should_deflate(response)) {
    if (!head) response.body_view(served->owned_ ? arena.copy(body) : body);
    return response;
  }

  const std::string_view encoding = select_best_encoding(parse_accept_encoding(request.header("accept-encoding")));
  if (encoding.empty()) {
    // Rack::Deflater: 406 with the message below. The path is the request target.
    const std::string message =
        "An acceptable encoding for the requested resource " + std::string(request.target) + " could not be found.";
    Response refused = ctx.response(406);
    refused.add("content-type", "text/plain");
    refused.add_copy("content-length", std::to_string(message.size()));
    refused.body_view(arena.copy(message));
    return refused;
  }

  // Vary: the tokens that the app set, then "Accept-Encoding", unless one of them has it.
  {
    std::string vary;
    bool seen = false;
    const std::string_view existing = response.get("vary");
    std::size_t begin = 0;
    while (!existing.empty() && begin <= existing.size()) {
      std::size_t end = existing.find(',', begin);
      if (end == std::string_view::npos) end = existing.size();
      const std::string_view token = trim(existing.substr(begin, end - begin));
      if (token == "*" || iequals(token, "accept-encoding")) seen = true;
      begin = end + 1;
    }
    if (!seen) {
      vary = existing.empty() ? std::string() : std::string(existing) + ",";
      vary += "Accept-Encoding";
      insert_header(response, "vary", arena.copy(vary));
    }
  }
  if (encoding == "identity") {
    if (!head) response.body_view(served->owned_ ? arena.copy(body) : body);
    return response;
  }
  response.add("content-encoding", "gzip");
  remove_header(response, "content-length");
  if (head) {
    response.unsized = true;
    return response;
  }
  response.chunked = true;
  if (!full_gzip.empty() && body.data() == served->file->identity.data() &&
      body.size() == served->file->identity.size()) {
    response.body_view(full_gzip);
  } else {
    const auto mtime = static_cast<std::uint32_t>(assets::generated_data().built_at);
    response.body_view(arena.copy(gzip_member(body, mtime)));
  }
  return response;
}

}  // namespace campfire::net::front
