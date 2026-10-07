// Opengraph::{Metadata, Location, Fetch, Document}. Rust: crates/campfire/src/integrations/opengraph/*.rs.
#include "app/opengraph/opengraph.hpp"

#include <algorithm>
#include <array>

#include "app/network_guard.hpp"
#include "app/opengraph/html.hpp"
#include "compat/json.hpp"
#include "core/log.hpp"
#include "richtext/dom.hpp"
#include "richtext/sanitizer.hpp"
#include "richtext/text_util.hpp"
#include "richtext/uri.hpp"

namespace campfire::app::opengraph {

namespace {

using richtext::Uri;
using unfurl::Clock;

constexpr std::array<std::string_view, 4> kAttributes = {"title", "url", "image", "description"};
constexpr std::array<std::string_view, 4> kTwitterHosts = {"twitter.com", "www.twitter.com", "x.com", "www.x.com"};
constexpr std::string_view kFxTwitterHost = "fxtwitter.com";
constexpr std::array<std::string_view, 4> kAllowedImageContentTypes = {"image/jpeg", "image/png", "image/gif",
                                                                       "image/webp"};
constexpr std::string_view kAllowedDocumentContentType = "text/html";

bool is_blank(std::string_view text) {
  return richtext::is_blank(text);
}

struct Context {
  const unfurl::Network& network;
  Clock::time_point deadline;
};

bool word_char(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
         static_cast<unsigned char>(c) >= 0x80;
}

// `url.match(FILES_AND_MEDIA_URL_REGEX)`: /\bhttps?:\/\/\S+\.(?:zip|tar|...|mpeg)\b/
bool files_and_media_url(std::string_view text) {
  static constexpr std::array<std::string_view, 37> kExtensions = {
      "zip", "tar",  "gz",  "bz2", "xz",  "rar",  "7z",  "dmg", "exe",  "msi",    "pkg",    "deb",  "iso",
      "jpg", "jpeg", "png", "gif", "bmp", "mp4",  "mov", "avi", "mkv",  "wmv",    "flv",    "heic", "heif",
      "mp3", "wav",  "ogg", "aac", "wma", "webm", "ogv", "mpg", "mpeg", "tar.gz", "tar.bz2"};
  const auto space = [](char c) { return c == ' ' || (c >= '\t' && c <= '\r'); };
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (i > 0 && word_char(text[i - 1])) continue;
    std::size_t rest;
    if (text.compare(i, 8, "https://") == 0)
      rest = i + 8;
    else if (text.compare(i, 7, "http://") == 0)
      rest = i + 7;
    else
      continue;
    std::size_t end = rest;
    while (end < text.size() && !space(text[end])) ++end;
    const std::string_view run = text.substr(rest, end - rest);
    for (std::size_t k = 1; k < run.size(); ++k) {
      if (run[k] != '.') continue;
      for (const std::string_view ext : kExtensions) {
        if (run.compare(k + 1, ext.size(), ext) != 0) continue;
        const std::size_t after = k + 1 + ext.size();
        const char next = rest + after < text.size() ? text[rest + after] : '\0';
        if (rest + after >= text.size() || !word_char(next)) return true;
      }
    }
  }
  return false;
}

// `Opengraph::Location`
class Location {
 public:
  Location(const Context& context, std::optional<std::string_view> url) : context_(context) {
    if (url) {
      url_ = std::string(*url);
      if (auto parsed = richtext::parse_uri(*url)) parsed_ = std::move(*parsed);
    }
  }

  // Both validations run, so the host is resolved even for a URL that is not http.
  bool is_valid() {
    const bool http = parsed_ && parsed_->is_http();
    const bool is_public = resolved_ip().has_value();
    return http && is_public;
  }

  const std::optional<std::string>& resolved_ip() {
    if (resolved_) return *resolved_;
    std::optional<std::string> ip;
    if (parsed_ && parsed_->host) {
      ip = context_.network.lookup ? resolve_public_address(*parsed_->host, context_.network.lookup)
                                   : resolve_public_address(*parsed_->host);
    }
    resolved_ = ip;
    return *resolved_;
  }

  [[nodiscard]] const std::optional<Uri>& parsed() const { return parsed_; }
  [[nodiscard]] const std::string& url() const { return url_; }
  [[nodiscard]] const Context& context() const { return context_; }

 private:
  Context context_;
  std::string url_;
  std::optional<Uri> parsed_;
  std::optional<std::optional<std::string>> resolved_;
};

// `Opengraph::Fetch`: GET or HEAD against a pinned address, following up to 10 responses (any 3xx is a redirect).
// Each redirect target is parsed, must be http(s), and goes through the guard again.
std::string request_uri(const Uri& url) {
  std::string out = url.path && !url.path->empty() ? *url.path : "/";
  if (url.query) out += "?" + *url.query;
  return out;
}

// `Net::HTTPGenericRequest#initialize`: `uri.hostname`, plus the port unless it is the default of the scheme.
std::string host_header(const std::string& host, std::uint16_t port, bool https) {
  std::string name = host;
  if (name.size() > 1 && name.front() == '[' && name.back() == ']') name = name.substr(1, name.size() - 2);
  const std::uint16_t default_port = https ? 443 : 80;
  return port == default_port ? name : name + ":" + std::to_string(port);
}

bool iequals(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
         });
}

// `Net::HTTP.start(url.host, url.port, ipaddr: ip, use_ssl: url.scheme == "https")` and `http.request(...)`.
Result<unfurl::Response> send(const Context& context, const Uri& url, const std::string& ip, std::string_view method) {
  if (!url.host || url.host->empty()) return fail(Errc::Parse, "bad URI");
  const bool https = url.scheme && iequals(*url.scheme, "https");
  if (!url.port || *url.port > 65535) return fail(Errc::Parse, "bad URI");
  const auto port = static_cast<std::uint16_t>(*url.port);
  const unfurl::Endpoint endpoint{https, *url.host, port, ip};
  const unfurl::Request request{std::string(method), request_uri(url), host_header(*url.host, port, https)};
  return unfurl::exchange(context.network, endpoint, request, context.network.timeouts, context.deadline);
}

struct Target {
  Uri url;
  std::string ip;
};

Result<Target> resolve_redirect(const Context& context, std::optional<std::string> location) {
  if (!location) return fail(Errc::Parse, "bad URI");
  auto url = richtext::parse_uri(*location);
  if (!url) return fail(Errc::Parse, "bad URI");
  if (!url->is_http()) return fail(Errc::Parse, "Opengraph::Fetch::RedirectDeniedError");
  std::optional<std::string> ip;
  if (context.network.lookup) {
    ip = resolve_public_address(url->host.value_or(""), context.network.lookup);
  } else {
    ip = resolve_public_address(url->host.value_or(""));
  }
  if (!ip) return fail(Errc::Parse, "Attempt to access private IP via " + url->host.value_or(""));
  return Target{std::move(*url), std::move(*ip)};
}

Result<unfurl::Response> request(const Context& context, Uri url, std::string ip, std::string_view method) {
  for (std::size_t i = 0; i < kMaxRedirects; ++i) {
    auto response = send(context, url, ip, method);
    if (!response) return response;
    if (response->status >= 300 && response->status < 400) {
      auto next = resolve_redirect(context, response->header("location"));
      if (!next) return std::unexpected(next.error());
      url = std::move(next->url);
      ip = std::move(next->ip);
    } else {
      return response;
    }
  }
  return fail(Errc::Parse, "Opengraph::Fetch::TooManyRedirectsError");
}

// `fetch_document(url, ip:)`: the body, or nothing when the response is not acceptable.
Result<std::optional<std::string>> fetch_document(const Context& context, const Uri& url, const std::string& ip) {
  auto response = request(context, url, ip, "GET");
  if (!response) return std::unexpected(response.error());
  if (response->status != 200 || response->content_type() != std::optional<std::string>(kAllowedDocumentContentType)) {
    return std::optional<std::string>{};
  }
  auto length = response->content_length();
  if (!length) return std::unexpected(length.error());
  if (length->value_or(0) > kMaxBodySize) return std::optional<std::string>{};
  auto body = response->read_body(kMaxBodySize);
  if (!body) return std::unexpected(body.error());
  if (body->too_large) return std::optional<std::string>{};
  return std::optional<std::string>(std::move(body->bytes));
}

// `fetch_content_type(url, ip:)`: the `Content-Type` of the final response, whatever its status.
Result<std::optional<std::string>> fetch_content_type(const Context& context, const Uri& url, const std::string& ip) {
  auto response = request(context, url, ip, "HEAD");
  if (!response) return std::unexpected(response.error());
  return response->header("content-type");
}

// `read_html`: nothing for an invalid URL or one that looks like a file or media.
std::optional<std::string> read_html(Location& location) {
  if (!location.is_valid() || files_and_media_url(location.url())) return std::nullopt;
  const Uri& url = *location.parsed();
  const std::string ip = *location.resolved_ip();
  auto html = fetch_document(location.context(), url, ip);
  if (!html) {
    log_warn("Failed to fetch {} at {} ({})", url.to_s(), ip, html.error().message);
    return std::nullopt;
  }
  return std::move(*html);
}

std::optional<std::string> read_content_type(Location& location) {
  if (!location.is_valid()) return std::nullopt;
  const Uri& url = *location.parsed();
  const std::string ip = *location.resolved_ip();
  auto type = fetch_content_type(location.context(), url, ip);
  if (!type) {
    log_warn("Failed to fetch {} at {} ({})", url.to_s(), ip, type.error().message);
    return std::nullopt;
  }
  return std::move(*type);
}

}  // namespace

std::vector<std::pair<std::string, std::string>> opengraph_attributes(std::optional<std::string_view> body) {
  const std::string html = decode(body.value_or(std::string_view{}));
  const std::vector<Element> metas = meta_elements(html);
  const bool has_meta_encoding = meta_encoding(metas).has_value();
  std::array<std::optional<std::string>, kAttributes.size()> found;
  for (const Element& meta : metas) {
    const std::string* property = meta.attr("property");
    const std::string* name_attr = meta.attr("name");
    const bool tag = (property && property->starts_with("og:")) || (name_attr && name_attr->starts_with("og:"));
    if (!tag) continue;
    std::string name = property ? *property : (name_attr ? *name_attr : std::string());
    for (std::size_t at; (at = name.find("og:")) != std::string::npos;) name.erase(at, 3);
    const auto index = std::find(kAttributes.begin(), kAttributes.end(), name) - kAttributes.begin();
    if (static_cast<std::size_t>(index) >= kAttributes.size()) continue;
    const std::string* content = meta.attr("content");
    if (content == nullptr || is_blank(*content)) continue;
    std::string value;
    for (const char c : *content) {
      if (has_meta_encoding || static_cast<unsigned char>(c) < 0x80) value += c;
    }
    found[static_cast<std::size_t>(index)] = std::move(value);
  }
  std::vector<std::pair<std::string, std::string>> out;
  for (std::size_t i = 0; i < kAttributes.size(); ++i) {
    if (found[i]) out.emplace_back(std::string(kAttributes[i]), std::move(*found[i]));
  }
  return out;
}

namespace {

// The attributes of the model as instance variables, in the order they were first assigned (the order of `render
// json:`).
struct Metadata {
  std::vector<std::pair<std::string, std::optional<std::string>>> attributes;

  [[nodiscard]] const std::string* get(std::string_view key) const {
    for (const auto& [name, value] : attributes) {
      if (name == key) return value ? &*value : nullptr;
    }
    return nullptr;
  }
  void assign(std::string_view key, std::optional<std::string> value) {
    for (auto& [name, existing] : attributes) {
      if (name == key) {
        existing = std::move(value);
        return;
      }
    }
    attributes.emplace_back(std::string(key), std::move(value));
  }
  [[nodiscard]] bool present(std::string_view key) const {
    const std::string* value = get(key);
    return value != nullptr && !is_blank(*value);
  }
};

bool is_twitter_host(const std::optional<std::string>& host) {
  return host && std::find(kTwitterHosts.begin(), kTwitterHosts.end(), *host) != kTwitterHosts.end();
}

// `tweet_url?`. A URL that `URI.parse` rejects with `URI::InvalidComponentError` raises.
Result<bool> tweet_url(std::string_view url) {
  auto parsed = richtext::parse_uri(url);
  if (!parsed) {
    if (parsed.error() == richtext::UriError::InvalidComponent)
      return fail(Errc::Internal, "URI::InvalidComponentError");
    return false;
  }
  return is_twitter_host(parsed->host) && parsed->path && !is_blank(*parsed->path) && *parsed->path != "/";
}

// `replace_twitter_domain_for_opengraph_support`
std::optional<std::string> replace_twitter_domain(std::string_view url) {
  auto parsed = richtext::parse_uri(url);
  if (!parsed) return std::nullopt;
  if (is_twitter_host(parsed->host)) parsed->host = std::string(kFxTwitterHost);
  return parsed->to_s();
}

// `fetch_document(untrusted_url)`: tweets are read through fxtwitter.com. A tweet whose fxtwitter page cannot be read
// raises (`nil.force_encoding`).
Result<std::optional<std::string>> metadata_document(const Context& context, std::string_view url) {
  auto tweet = tweet_url(url);
  if (!tweet) return std::unexpected(tweet.error());
  if (*tweet) {
    const auto fx = replace_twitter_domain(url);
    Location location(context, fx ? std::optional<std::string_view>(*fx) : std::nullopt);
    auto html = read_html(location);
    if (!html) return fail(Errc::Internal, "NoMethodError");
    return html;
  }
  Location location(context, url);
  return read_html(location);
}

// `valid_canonical_url(url, fallback)`
std::string valid_canonical_url(const Context& context, const std::optional<std::string>& url,
                                std::string_view fallback) {
  if (url) {
    Location location(context, *url);
    if (location.is_valid()) return *url;
  }
  return std::string(fallback);
}

// `valid_image_content_type(image)`: kept only when a HEAD says it is a JPEG, PNG, GIF or WebP.
std::optional<std::string> valid_image_content_type(const Context& context, const std::optional<std::string>& image) {
  if (!image || is_blank(*image)) return std::nullopt;
  if (!richtext::parse_uri(*image)) {
    log_warn("Failed to fetch image content tpye: {} (bad URI(is not URI?))", *image);
    return std::nullopt;
  }
  Location location(context, *image);
  auto type = read_content_type(location);
  if (!type) return std::nullopt;
  for (char& c : *type) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (std::find(kAllowedImageContentTypes.begin(), kAllowedImageContentTypes.end(), *type) ==
      kAllowedImageContentTypes.end()) {
    return std::nullopt;
  }
  return image;
}

// `strip_tags` (Rails::HTML5::FullSanitizer): the text of the HTML5 fragment, serialized.
Result<std::string> strip_tags(std::string_view html) {
  if (html.empty()) return std::string();
  auto dom = richtext::parse_fragment(html);
  if (!dom) return fail(Errc::Internal, "ArgumentError");
  std::string text;
  std::vector<const richtext::Node*> stack{dom->root()};
  while (!stack.empty()) {
    const richtext::Node* node = stack.back();
    stack.pop_back();
    if (node->is_text()) text += node->text;
    for (const richtext::Node* child = node->last_child; child != nullptr; child = child->prev) stack.push_back(child);
  }
  richtext::Dom out;
  if (!text.empty()) out.append_child(out.root(), out.create_text(text));
  return richtext::to_html(out.root());
}

// `sanitize` (Rails::HTML5::SafeListSanitizer with its default allowlist).
Result<std::string> sanitize(std::string_view html) {
  auto clean = richtext::sanitize(html, richtext::SafeList::defaults());
  if (!clean) return fail(Errc::Internal, "ArgumentError");
  return std::move(*clean);
}

Result<Metadata> from_url(const Context& context, std::string_view url) {
  auto body = metadata_document(context, url);
  if (!body) return std::unexpected(body.error());
  const auto found = opengraph_attributes(*body ? std::optional<std::string_view>(**body) : std::nullopt);
  const auto og = [&](std::string_view key) -> std::optional<std::string> {
    for (const auto& [name, value] : found) {
      if (name == key) return value;
    }
    return std::nullopt;
  };
  const std::string canonical = valid_canonical_url(context, og("url"), url);
  const std::optional<std::string> image = valid_image_content_type(context, og("image"));
  Metadata metadata;
  for (const auto& [name, value] : found) metadata.attributes.emplace_back(name, value);
  metadata.assign("url", canonical);
  metadata.assign("image", image);
  return metadata;
}

// `valid?`: `before_validation` sanitizes the title and the description, then the checks.
Result<bool> validate(const Context& context, Metadata& metadata) {
  for (const std::string_view key : {"title", "description"}) {
    const std::string* value = metadata.get(key);
    if (value == nullptr) continue;
    auto stripped = strip_tags(*value);
    if (!stripped) return std::unexpected(stripped.error());
    auto clean = sanitize(*stripped);
    if (!clean) return std::unexpected(clean.error());
    metadata.assign(key, std::move(*clean));
  }
  bool valid = metadata.present("title") && metadata.present("url") && metadata.present("description");
  const std::string* image = metadata.get("image");
  if (image != nullptr && !is_blank(*image)) {
    Location location(context, *image);
    valid = location.is_valid() && valid;
  }
  return valid;
}

// `render json: opengraph` after `valid?`: `instance_values`, which then include the validation context and the
// (empty) errors.
std::string to_json(const Metadata& metadata) {
  using compat::json::Value;
  std::string json = "{";
  for (const auto& [key, value] : metadata.attributes) {
    json += compat::json::encode(Value(key)) + ":" + compat::json::encode(value ? Value(*value) : Value()) + ",";
  }
  json += R"("context_for_validation":{"context":null},"errors":{}})";
  return json;
}

}  // namespace

Result<Unfurl> unfurl(const unfurl::Network& network, std::string_view url, unfurl::Clock::time_point deadline) {
  const Context context{network, deadline};
  const auto gave_up = [&]() -> Result<Unfurl> {
    log_warn("Gave up unfurling {} after {}s", url, kUnfurlDeadline.count());
    return Unfurl{};
  };
  if (Clock::now() >= deadline) return gave_up();
  auto metadata = from_url(context, url);
  if (!metadata) return std::unexpected(metadata.error());
  auto valid = validate(context, *metadata);
  if (!valid) return std::unexpected(valid.error());
  if (Clock::now() >= deadline) return gave_up();
  if (!*valid) return Unfurl{};
  return Unfurl{true, to_json(*metadata)};
}

}  // namespace campfire::app::opengraph
