// `image_tag` and `asset_path` (Rust: crates/views/src/helpers/assets.rs).
#include "views/helpers/assets.hpp"

#include <algorithm>
#include <cctype>

namespace campfire::views::helpers {

std::string asset_path(const ViewContext& ctx, std::string_view source) {
  bool is_url = source.starts_with('/') || source.starts_with("data:") || source.starts_with("cid:");
  if (!is_url) {
    const std::size_t scheme_end = source.find("://");
    if (scheme_end != std::string_view::npos && scheme_end > 0) {
      is_url = std::ranges::all_of(source.substr(0, scheme_end),
                                   [](unsigned char c) { return (std::isalpha(c) != 0 && c < 128) || c == '-'; });
    }
  }
  return is_url ? std::string(source) : ctx.asset(source);
}

void image_tag(Out& out, const ViewContext& ctx, std::string_view source, Attrs options) {
  const std::optional<Value> size = options.remove("size");
  options.set("src", Value(asset_path(ctx, source)));
  if (size) {
    const std::string text = size->to_s();
    const std::size_t x = text.find('x');
    options.set("width", Value(x == std::string::npos ? text : text.substr(0, x)));
    options.set("height", Value(x == std::string::npos ? text : text.substr(x + 1)));
  }
  legacy_tag(out, "img", options);
}

}  // namespace campfire::views::helpers
