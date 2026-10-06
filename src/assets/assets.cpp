// Asset URL helpers and tags (Rails: ActionView AssetUrlHelper, Propshaft::Helper; Rust: helpers.rs, tags.rs).
#include "assets/assets.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>

#include "assets/mime.hpp"
#include "compat/ruby.hpp"

namespace campfire::assets {

namespace {

std::string_view view(const BlobRef& ref) noexcept {
  return {generated_data().blob + ref.offset, ref.size};
}

Error missing_asset(std::string_view source) {
  return Error{Errc::NotFound, "The asset '" + std::string(source) + "' was not found in the load path."};
}

std::string lower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

// ActionView::Helpers::AssetUrlHelper::URI_REGEXP: %r{^[-a-z]+://|^(?:cid|data):|^//}i
bool is_uri(std::string_view source) {
  const std::string s = lower(source);
  if (s.starts_with("//") || s.starts_with("cid:") || s.starts_with("data:")) {
    return true;
  }
  const auto scheme = s.find("://");
  return scheme != std::string::npos && scheme > 0 &&
         std::all_of(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(scheme),
                     [](char c) { return c == '-' || (c >= 'a' && c <= 'z'); });
}

Result<std::string> compute(std::string_view source, std::string_view extension) {
  if (source.empty()) {
    return std::string();
  }
  if (is_uri(source)) {
    return std::string(source);
  }
  // tail = source[/([?#].+)$/]: a "?" or "#" that something follows.
  std::string_view tail;
  const auto cut = source.find_first_of("?#");
  if (cut != std::string_view::npos && cut + 1 < source.size()) {
    tail = source.substr(cut);
    source = source.substr(0, cut);
  }
  std::string path(source);
  if (!extension.empty() && file_extname(path) != extension) {
    path += extension;
  }
  if (!path.starts_with('/')) {
    const auto digested = digested_path(path);
    if (!digested) {
      return std::unexpected(missing_asset(path));
    }
    path = std::string(kPrefix) + "/" + std::string(*digested);
  }
  path += tail;
  return path;
}

constexpr std::size_t kMaxLinkHeaderSize = 1000;

}  // namespace

std::optional<StaticFile> find_file(std::string_view url_path) noexcept {
  const auto files = generated_data().files;
  const auto it = std::lower_bound(files.begin(), files.end(), url_path,
                                   [](const FileRecord& r, std::string_view key) { return r.url < key; });
  if (it == files.end() || it->url != url_path) {
    return std::nullopt;
  }
  const auto type = mime_type(file_extname(it->url));
  return StaticFile{it->url, type.value_or("text/plain"), view(it->identity), view(it->gzip), view(it->zstd)};
}

EncodedBody choose_body(const StaticFile& file, std::string_view accept_encoding) noexcept {
  const auto accepts = [&](std::string_view coding) {
    // `/\bcoding\b/i` over each element of the header, as Rack parses it.
    std::size_t pos = 0;
    while (pos <= accept_encoding.size()) {
      const std::size_t comma = std::min(accept_encoding.find(',', pos), accept_encoding.size());
      std::string_view part = accept_encoding.substr(pos, comma - pos);
      part = part.substr(0, part.find(';'));
      const std::string value = lower(part);
      for (auto at = value.find(coding); at != std::string::npos; at = value.find(coding, at + 1)) {
        const auto word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; };
        const bool before = at > 0 && word(value[at - 1]);
        const bool after = at + coding.size() < value.size() && word(value[at + coding.size()]);
        if (!before && !after) {
          return true;
        }
      }
      pos = comma + 1;
    }
    return false;
  };
  if (!file.zstd.empty() && accepts("zstd")) {
    return {file.zstd, "zstd"};
  }
  if (!file.gzip.empty() && accepts("gzip")) {
    return {file.gzip, "gzip"};
  }
  return {file.identity, {}};
}

std::optional<std::string_view> digested_path(std::string_view logical_path) noexcept {
  const auto records = generated_data().manifest;
  const auto it = std::lower_bound(records.begin(), records.end(), logical_path,
                                   [](const ManifestRecord& r, std::string_view key) { return r.logical < key; });
  if (it == records.end() || it->logical != logical_path) {
    return std::nullopt;
  }
  return it->digested;
}

Result<std::string> asset_path(std::string_view source) { return compute(source, {}); }
Result<std::string> image_path(std::string_view source) { return compute(source, {}); }
Result<std::string> audio_path(std::string_view source) { return compute(source, {}); }
Result<std::string> javascript_path(std::string_view source) { return compute(source, ".js"); }
Result<std::string> stylesheet_path(std::string_view source) { return compute(source, ".css"); }

Result<std::string> asset_url(std::string_view base_url, std::string_view source) {
  auto path = asset_path(source);
  if (!path || path->empty() || is_uri(*path)) {
    return path;
  }
  // File.join(host, path)
  while (base_url.ends_with('/')) {
    base_url.remove_suffix(1);
  }
  std::string_view rest = *path;
  while (rest.starts_with('/')) {
    rest.remove_prefix(1);
  }
  return std::string(base_url) + "/" + std::string(rest);
}

Result<std::string> image_url(std::string_view base_url, std::string_view source) { return asset_url(base_url, source); }

std::span<const ManifestRecord> manifest() noexcept { return generated_data().manifest; }
std::string_view manifest_json() noexcept { return view(generated_data().manifest_json); }
std::span<const std::string_view> all_stylesheet_paths() noexcept { return generated_data().stylesheets; }
std::string_view javascript_importmap_tags() noexcept { return view(generated_data().importmap_tags); }

Result<StylesheetTags> stylesheet_link_tag_all(std::span<const std::pair<std::string_view, std::string_view>> options) {
  return stylesheet_link_tag(all_stylesheet_paths(), options);
}

// Propshaft::Helper renders one Rails `stylesheet_link_tag` for each source and joins them with "\n".
Result<StylesheetTags> stylesheet_link_tag(std::span<const std::string_view> sources,
                                           std::span<const std::pair<std::string_view, std::string_view>> options) {
  StylesheetTags tags;
  for (const std::string_view source : sources) {
    auto href = stylesheet_path(source);
    if (!href) {
      return std::unexpected(href.error());
    }
    if (!href->empty() && !href->starts_with("data:")) {
      tags.preload_links.push_back("<" + *href + ">; rel=preload; as=style; nopush");
    }
    if (!tags.html.empty()) {
      tags.html += '\n';
    }
    tags.html += "<link rel=\"stylesheet\" href=\"" + compat::html_escape(*href) + "\"";
    for (const auto& [name, value] : options) {
      tags.html += " " + std::string(name) + "=\"" + compat::html_escape(value) + "\"";
    }
    tags.html += " />";
  }
  return tags;
}

std::string append_preload_links(std::string_view header, std::span<const std::string> links) {
  std::string out(header);
  for (const auto& link : links) {
    if (out.size() + link.size() > kMaxLinkHeaderSize) {
      continue;
    }
    if (!out.empty()) {
      out += ',';
    }
    out += link;
  }
  return out;
}

const std::string& built_at_http_date() {
  static const std::string value = [] {
    static constexpr std::array<const char*, 7> kDays{"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static constexpr std::array<const char*, 12> kMonths{"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const auto t = static_cast<std::time_t>(generated_data().built_at);
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::array<char, 40> buf{};
    std::snprintf(buf.data(), buf.size(), "%s, %02d %s %d %02d:%02d:%02d GMT", kDays.at(static_cast<std::size_t>(tm.tm_wday)),
                  tm.tm_mday, kMonths.at(static_cast<std::size_t>(tm.tm_mon)), tm.tm_year + 1900, tm.tm_hour, tm.tm_min,
                  tm.tm_sec);
    return std::string(buf.data());
  }();
  return value;
}

}  // namespace campfire::assets
