// ContentFilters::TextMessagePresentationFilters with attachments. Rails: app/helpers/content_filters/*.rb.
// Rust: crates/richtext/src/filters.rs
#pragma once

#include "richtext/content.hpp"

namespace campfire::richtext {

// RemoveSoloUnfurledLinkText, SanitizeTags, SanitizeAttributes, in that order.
[[nodiscard]] Result<Content> apply_message_filters(Content content, const RenderContext& ctx);

// RemoveSoloUnfurledLinkText: a message that is only a link to what it unfurls shows only the unfurl.
[[nodiscard]] Result<Content> remove_solo_unfurled_link_text(Content content, const RenderContext& ctx);

}  // namespace campfire::richtext
