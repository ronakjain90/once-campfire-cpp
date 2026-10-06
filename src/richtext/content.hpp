// ActionText::Content: load (canonicalize), render attachments, plain text.
// Rails: actiontext content.rb, fragment.rb, attachment_gallery.rb, trix_attachment.rb.
// Rust: crates/richtext/src/content.rs
//
// Each Ruby step that writes a node as markup and parses it again (`Fragment#replace` with a string,
// `inner_html=`, a filter that returns HTML) does the same here, in the same parse context. The
// output has the shape of those round trips.
#pragma once

#include <string>
#include <string_view>

#include "richtext/attachables.hpp"
#include "richtext/dom.hpp"
#include "richtext/error.hpp"

namespace campfire::richtext {

inline constexpr std::string_view kAttachmentTag = "action-text-attachment";

// How deep content attachments render inside one another. Each level parses and sanitizes all
// that is below it again. Rails does not limit the depth, so its work grows with the square of the
// size of the body. Deeper content attachments render empty (README "Known differences").
inline constexpr int kMaxContentAttachmentDepth = 8;

// A fragment of rich text. `root` is a node of `dom`.
class Content {
 public:
  // `ActionText::Content.new(html)`, which is also how a stored body loads: canonicalized.
  [[nodiscard]] static Result<Content> load(std::string_view html, const RenderContext& ctx);
  // `ActionText::Content.new(html, canonicalize: false)`.
  [[nodiscard]] static Result<Content> wrap(std::string_view html);

  Content(Content&&) noexcept = default;
  Content& operator=(Content&&) noexcept = default;
  Content(const Content&) = delete;
  Content& operator=(const Content&) = delete;
  ~Content() = default;

  [[nodiscard]] std::string to_html() const;
  // `ActionText::Content#to_plain_text`
  [[nodiscard]] Result<std::string> to_plain_text(const RenderContext& ctx) const;
  // `render_action_text_content(content)`: attachments and galleries rendered, then sanitized with
  // the list of Action Text.
  [[nodiscard]] Result<std::string> render(const RenderContext& ctx) const;
  // `Content#to_s`: the content partial inside layouts/action_text/contents/_content.html.erb,
  // which Campfire overrides with a `lexxy-content` wrapper.
  [[nodiscard]] Result<std::string> to_rendered_html_with_layout(const RenderContext& ctx) const;

  // `render`, for content `depth` content attachments down.
  [[nodiscard]] Result<std::string> render_nested(const RenderContext& ctx, int depth) const;

  Dom dom;
  Node* root = nullptr;

 private:
  Content() = default;
};

// Loads and canonicalizes `html` into the empty fragment node `fragment` of `dom`.
[[nodiscard]] Result<void> load_into(Dom& dom, Node* fragment, std::string_view html, const RenderContext& ctx);

// The `action-text-attachment` elements below `root`, in document order.
[[nodiscard]] std::vector<Node*> attachment_nodes(Node* root);

// An attachment, rendered as the editor and the page need it.
[[nodiscard]] Result<std::string> render_attachment_html(const Attachment& attachment, const RenderContext& ctx);

}  // namespace campfire::richtext
