// The effective config.active_storage content type lists: Rails 8.2 engine defaults with
// load_defaults 8.2, minus the types removed in config/initializers/vips.rb
// (Rust: crates/storage/src/content_types.rs).
#pragma once

#include <array>
#include <optional>
#include <string_view>

namespace campfire::storage::content_types {

// variable_content_types (bmp, ico and psd removed by config/initializers/vips.rb).
inline constexpr std::array<std::string_view, 8> kVariable = {"image/png",  "image/gif",  "image/jpeg", "image/tiff",
                                                              "image/webp", "image/avif", "image/heic", "image/heif"};
// web_image_content_types (webp added by load_defaults 7.2).
inline constexpr std::array<std::string_view, 4> kWebImage = {"image/png", "image/jpeg", "image/gif", "image/webp"};
inline constexpr std::array<std::string_view, 10> kAllowedInline = {"image/webp",
                                                                    "image/avif",
                                                                    "image/png",
                                                                    "image/gif",
                                                                    "image/jpeg",
                                                                    "image/tiff",
                                                                    "image/bmp",
                                                                    "image/vnd.adobe.photoshop",
                                                                    "image/vnd.microsoft.icon",
                                                                    "application/pdf"};
inline constexpr std::array<std::string_view, 9> kServeAsBinary = {
    "text/html",          "image/svg+xml",   "application/postscript", "application/x-shockwave-flash",
    "text/xml",           "application/xml", "application/xhtml+xml",  "application/mathml+xml",
    "text/cache-manifest"};

inline constexpr std::string_view kBinaryContentType = "application/octet-stream";

// ActiveStorage.video_preview_arguments from load_defaults 7.0, already split like a shell does.
inline constexpr std::array<std::string_view, 6> kVideoPreviewArguments = {
    "-vf",       R"(select=eq(n\,0)+eq(key\,1)+gt(scene\,0.015),loop=loop=-1:size=2,trim=start_frame=1)",
    "-frames:v", "1",
    "-f",        "image2"};

namespace detail {
template <class A>
constexpr bool has(const A& list, std::string_view v) {
  for (auto item : list) {
    if (item == v) return true;
  }
  return false;
}
}  // namespace detail

constexpr bool is_variable(std::string_view t) {
  return detail::has(kVariable, t);
}
constexpr bool is_web_image(std::string_view t) {
  return detail::has(kWebImage, t);
}
constexpr bool is_allowed_inline(std::string_view t) {
  return detail::has(kAllowedInline, t);
}
constexpr bool serve_as_binary(std::string_view t) {
  return detail::has(kServeAsBinary, t);
}

// content_type_for_serving.
constexpr std::string_view for_serving(std::string_view t) {
  return serve_as_binary(t) ? kBinaryContentType : t;
}

// forced_disposition_for_serving: "attachment" for binary or non-inline types.
constexpr std::optional<std::string_view> forced_disposition(std::string_view t) {
  if (serve_as_binary(t) || !is_allowed_inline(t)) return "attachment";
  return std::nullopt;
}

// ActiveStorage.paths is empty in Campfire, so the binaries come from PATH.
inline constexpr std::string_view kFfmpeg = "ffmpeg";
inline constexpr std::string_view kFfprobe = "ffprobe";

}  // namespace campfire::storage::content_types
