// `image_tag` and `asset_path` (ActionView AssetTagHelper, AssetUrlHelper).
// Rust: crates/views/src/helpers/assets.rs.
#pragma once

#include <string>
#include <string_view>

#include "views/context.hpp"
#include "views/helpers/tag.hpp"

namespace campfire::views::helpers {

// URLs and absolute paths pass through. Logical asset paths get their digest from the context.
[[nodiscard]] std::string asset_path(const ViewContext& ctx, std::string_view source);

// `image_tag(source, options)`: the options in order, then `src`, then `width` and `height` from
// `size:` ("20" or "20x30").
void image_tag(Out& out, const ViewContext& ctx, std::string_view source, Attrs options = {});

}  // namespace campfire::views::helpers
