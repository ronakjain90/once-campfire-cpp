// The asset pipeline of the reference app, prepared at build time (Rails: Propshaft and
// importmap-rails; Rust: crates/assets/src/lib.rs, helpers.rs, tags.rs).
//
// - `digested_path`, `asset_path` and the others map a logical path to its digested path.
// - `find_file` maps a URL path to a file with identity, gzip and zstd bodies.
// - `stylesheet_link_tag_all` and `javascript_importmap_tags` give the tags of the layout.
// - `serve` (static_files.hpp) answers a request like ActionDispatch::Static.
#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "assets/table.hpp"
#include "core/error.hpp"

namespace campfire::assets {

// The URL prefix of the digested assets (`config.assets.prefix`).
inline constexpr std::string_view kPrefix = "/assets";

// A file that the server can send. All views are valid for the life of the process.
struct StaticFile {
  std::string_view url;
  std::string_view content_type;  // as Rack::Mime gives it, or "text/plain" for an unknown extension
  std::string_view identity;
  std::string_view gzip;  // empty: no gzip body
  std::string_view zstd;  // empty: no zstd body
};

// The file for a URL path with no query, as it is in the table (for example "/assets/base-637a0ec8.css").
[[nodiscard]] std::optional<StaticFile> find_file(std::string_view url_path) noexcept;

// The body that fits an `Accept-Encoding` header: zstd, then gzip, then identity.
struct EncodedBody {
  std::string_view body;
  std::string_view content_encoding;  // empty for identity
};
[[nodiscard]] EncodedBody choose_body(const StaticFile& file, std::string_view accept_encoding) noexcept;

// The digested path, relative to "/assets/", for a logical path.
[[nodiscard]] std::optional<std::string_view> digested_path(std::string_view logical_path) noexcept;

// ActionView asset_path over the Propshaft static resolver: "/assets/<digested>" for an asset. A
// URL and an absolute path pass through. A "?query" or "#fragment" tail stays. A missing asset is
// an `Errc::NotFound` error with the message of Propshaft::MissingAssetError.
[[nodiscard]] Result<std::string> asset_path(std::string_view source);
[[nodiscard]] Result<std::string> image_path(std::string_view source);
[[nodiscard]] Result<std::string> audio_path(std::string_view source);
[[nodiscard]] Result<std::string> javascript_path(std::string_view source);
[[nodiscard]] Result<std::string> stylesheet_path(std::string_view source);
// asset_url: the path joined to the base URL of the request (for example "https://host:3000").
[[nodiscard]] Result<std::string> asset_url(std::string_view base_url, std::string_view source);
[[nodiscard]] Result<std::string> image_url(std::string_view base_url, std::string_view source);

// Propshaft's manifest, sorted by logical path, and its JSON as `/assets/.manifest.json` serves it.
[[nodiscard]] std::span<const ManifestRecord> manifest() noexcept;
[[nodiscard]] std::string_view manifest_json() noexcept;

struct StylesheetTags {
  std::string html;
  std::vector<std::string> preload_links;
};

// Propshaft::Helper#all_stylesheets_paths: the logical path of each CSS asset, sorted.
[[nodiscard]] std::span<const std::string_view> all_stylesheet_paths() noexcept;
// `stylesheet_link_tag :all, **options` and the preload links that Rails adds to the `link` header.
[[nodiscard]] Result<StylesheetTags> stylesheet_link_tag_all(
    std::span<const std::pair<std::string_view, std::string_view>> options);
[[nodiscard]] Result<StylesheetTags> stylesheet_link_tag(
    std::span<const std::string_view> sources, std::span<const std::pair<std::string_view, std::string_view>> options);
// send_preload_links_header: a link that makes the header longer than 1000 bytes is left out.
[[nodiscard]] std::string append_preload_links(std::string_view header, std::span<const std::string> links);

// `javascript_importmap_tags` for "application". It is fixed at build time.
[[nodiscard]] std::string_view javascript_importmap_tags() noexcept;

// The time of the build as Time#httpdate gives it: "Sat, 26 Sep 2026 12:23:14 GMT".
[[nodiscard]] const std::string& built_at_http_date();

}  // namespace campfire::assets
