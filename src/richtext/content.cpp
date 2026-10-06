// Rails: actiontext content.rb, fragment.rb, attachment_gallery.rb, trix_attachment.rb,
// helpers/action_text/content_helper.rb. Rust: crates/richtext/src/content.rs
#include "richtext/content.hpp"

#include <algorithm>
#include <array>
#include <utility>

#include "compat/json.hpp"
#include "richtext/filters.hpp"
#include "richtext/plain_text.hpp"
#include "richtext/sanitizer.hpp"
#include "richtext/text_util.hpp"
#include "richtext/tree.hpp"

namespace campfire::richtext {

namespace {

// ActionText::TrixAttachment::ATTRIBUTES, in order: the name in Trix and the name of the element.
constexpr std::array<std::pair<std::string_view, std::string_view>, 12> kTrixAttributes = {{
    {"sgid", "sgid"},
    {"contentType", "content-type"},
    {"url", "url"},
    {"href", "href"},
    {"filename", "filename"},
    {"filesize", "filesize"},
    {"width", "width"},
    {"height", "height"},
    {"previewable", "previewable"},
    {"content", "content"},
    {"caption", "caption"},
    {"presentation", "presentation"},
}};

// The attachment attributes of Action Text (and Lexxy), in the order of the sanitizer list.
constexpr std::array<std::string_view, 12> kAttachmentAttributes = {
    "sgid",  "content-type", "url",         "href",         "filename", "filesize",
    "width", "height",       "previewable", "presentation", "caption",  "content"};

Node* make_attachment(Dom& dom, const std::vector<std::pair<std::string_view, std::string>>& attrs) {
  Node* node = dom.create_element(kAttachmentTag);
  for (const auto& [name, value] : attrs) {
    dom.set_attr(node, name, value);
  }
  return node;
}

std::string_view trix_name_of(std::string_view dashed) {
  for (const auto& [trix, name] : kTrixAttributes) {
    if (name == dashed) {
      return trix;
    }
  }
  return {};
}

// `fragment_by_converting_trix_attachments`: `figure[data-trix-attachment]` (or any element that has
// the attribute) becomes an <action-text-attachment>, or disappears if it has none of the attributes.
Result<void> convert_trix_attachments(Dom& dom, Node* root, const RenderContext& ctx) {
  std::vector<Node*> nodes;
  for (Node* n : descendants(root)) {
    if (n->is_element() && n->find_attr("data-trix-attachment") != nullptr) {
      nodes.push_back(n);
    }
  }
  for (Node* node : nodes) {
    std::vector<std::pair<std::string, compat::json::Value>> attributes;
    for (std::string_view name : {"data-trix-attachment", "data-trix-attributes"}) {
      compat::json::Value parsed;  // null
      if (auto json = attr_value(node, name)) {
        // Unparseable JSON is logged and counts as no attributes.
        if (auto value = compat::json::parse(*json, {.allow_comments = true})) {
          parsed = std::move(*value);
        }
      }
      if (parsed.is_null() || (parsed.is_bool() && !parsed.as_bool())) {
        continue;
      }
      if (!parsed.is_object()) {
        return std::unexpected(Error::raised("NoMethodError: merge"));
      }
      for (const auto& [key, value] : parsed.as_object()) {
        const bool known = std::any_of(kTrixAttributes.begin(), kTrixAttributes.end(),
                                       [&](const auto& pair) { return pair.first == key; });
        if (!known) {
          continue;
        }
        auto existing =
            std::find_if(attributes.begin(), attributes.end(), [&](const auto& a) { return a.first == key; });
        if (existing != attributes.end()) {
          existing->second = value;
        } else {
          attributes.emplace_back(key, value);
        }
      }
    }
    std::vector<std::pair<std::string_view, std::string>> element_attrs;
    for (std::string_view name : kAttachmentAttributes) {
      const std::string_view trix = trix_name_of(name);
      auto found = std::find_if(attributes.begin(), attributes.end(), [&](const auto& a) { return a.first == trix; });
      if (found != attributes.end()) {
        element_attrs.emplace_back(name, json_value_to_s(found->second));
      }
    }
    std::string replacement;
    if (!element_attrs.empty()) {
      Node* element = make_attachment(dom, element_attrs);
      // Attachment.from_node finds the attachable, which can raise.
      if (auto attachment = attachment_from_node(element, ctx); !attachment) {
        return std::unexpected(attachment.error());
      }
      replacement = to_html(element);
    }
    if (auto replaced = replace_with_html(dom, node, replacement); !replaced) {
      return fail(replaced.error());
    }
  }
  return {};
}

// The first step of `render_attachments`: the `content` attribute is sanitized with the list of
// Action Text and dropped if nothing is left.
Result<void> sanitize_content_attribute(Dom& dom, Node* node) {
  if (auto content = remove_attr(dom, node, "content")) {
    auto sanitized = sanitize(*content, SafeList::action_text());
    if (!sanitized) {
      return fail(sanitized.error());
    }
    if (!is_blank(*sanitized)) {
      dom.set_attr(node, "content", *sanitized);
    }
  }
  return {};
}

// `Attachment#with_full_attributes`: a new node that has the attachment attributes of the node, the
// own attributes of the attachable (the SGID and the content type of a user), and the SGID of the node.
Result<Node*> node_with_full_attributes(Dom& dom, const Node* node, const Attachable& attachable) {
  std::vector<std::pair<std::string_view, std::string>> attrs;
  const bool user = attachable.kind == Attachable::Kind::User;
  for (std::string_view name : kAttachmentAttributes) {
    if (user && name == "sgid") {
      attrs.emplace_back(name, std::string(attr_value(node, "sgid").value_or(attachable.user.attachable_sgid)));
    } else if (user && name == "content-type") {
      attrs.emplace_back(name, std::string(kMentionContentType));
    } else if (auto value = attr_value(node, name)) {
      attrs.emplace_back(name, std::string(*value));
    }
  }
  if (attrs.empty()) {
    // from_attributes gives nil, and the render block calls #node on it.
    return std::unexpected(Error::raised("NoMethodError: node for nil"));
  }
  return make_attachment(dom, attrs);
}

Result<std::string> render_attachment_html_at(const Attachment& attachment, const RenderContext& ctx, int depth);

Result<std::string> render_content_at(std::string_view content, const RenderContext& ctx, int depth);

bool is_gallery_attachment(const Node* node) {
  return is_named(node, kAttachmentTag) && attr_value(node, "presentation") == std::string_view("gallery");
}

}  // namespace

std::vector<Node*> attachment_nodes(Node* root) {
  std::vector<Node*> out;
  for (Node* n : descendants(root)) {
    if (is_named(n, kAttachmentTag)) {
      out.push_back(n);
    }
  }
  return out;
}

namespace {

// `AttachmentGallery.find_attachment_gallery_nodes`: `div:has(A + A)` for gallery attachments A,
// where all children are gallery attachments or text of newlines and spaces.
std::vector<Node*> attachment_gallery_nodes(Node* root) {
  std::vector<Node*> out;
  for (Node* div : descendants(root)) {
    if (!is_named(div, "div")) {
      continue;
    }
    bool has_pair = false;
    for (Node* n : descendants(div)) {
      if (!is_gallery_attachment(n)) {
        continue;
      }
      // The element sibling just before `n`.
      Node* prev = n->prev;
      while (prev != nullptr && !prev->is_element()) {
        prev = prev->prev;
      }
      if (prev != nullptr && is_gallery_attachment(prev)) {
        has_pair = true;
        break;
      }
    }
    if (!has_pair) {
      continue;
    }
    bool only_gallery = true;
    for (Node* child = div->first_child; child != nullptr; child = child->next) {
      if (child->is_text()) {
        if (!std::all_of(child->text.begin(), child->text.end(), [](char c) { return c == '\n' || c == ' '; })) {
          only_gallery = false;
        }
      } else if (!is_gallery_attachment(child)) {
        only_gallery = false;
      }
    }
    if (only_gallery) {
      out.push_back(div);
    }
  }
  return out;
}

Result<void> render_attachments(Dom& dom, Node* root, const RenderContext& ctx, int depth) {
  for (Node* node : attachment_nodes(root)) {
    if (auto r = sanitize_content_attribute(dom, node); !r) return r;
    auto attachment = attachment_from_node(node, ctx);
    if (!attachment) return std::unexpected(attachment.error());
    auto full = node_with_full_attributes(dom, node, attachment->attachable);
    if (!full) return std::unexpected(full.error());
    Attachment rendered_for{std::move(attachment->attachable), std::nullopt};
    if (auto caption = attr_value(*full, "caption"); caption && !is_blank(*caption)) {
      rendered_for.caption = std::string(*caption);
    }
    auto html = render_attachment_html_at(rendered_for, ctx, depth);
    if (!html) return std::unexpected(html.error());
    if (auto r = set_inner_html(dom, *full, *html); !r) return fail(r.error());
    if (auto r = replace_with_html(dom, node, to_html(*full)); !r) return fail(r.error());
  }
  return {};
}

Result<void> render_attachment_galleries(Dom& dom, Node* root, const RenderContext& ctx, int depth) {
  for (Node* gallery : attachment_gallery_nodes(root)) {
    std::string rendered;
    std::size_t count = 0;
    for (Node* member : descendants(gallery)) {
      if (!is_gallery_attachment(member)) {
        continue;
      }
      ++count;
      auto attachment = attachment_from_node(member, ctx);
      if (!attachment) return std::unexpected(attachment.error());
      auto full = node_with_full_attributes(dom, member, attachment->attachable);
      if (!full) return std::unexpected(full.error());
      auto html = render_attachment_html_at(*attachment, ctx, depth);
      if (!html) return std::unexpected(html.error());
      if (auto r = set_inner_html(dom, *full, *html); !r) return fail(r.error());
      rendered += to_html(*full);
    }
    const std::string html = "<div class=\"attachment-gallery attachment-gallery--" + std::to_string(count) +
                             "\">\n  " + rendered + "\n</div>";
    if (auto r = replace_with_html(dom, gallery, html); !r) return fail(r.error());
  }
  return {};
}

Result<std::string> render_attachment_html_at(const Attachment& attachment, const RenderContext& ctx, int depth) {
  return render_attachment(attachment,
                           [&](std::string_view content) { return render_content_at(content, ctx, depth); });
}

}  // namespace

Result<std::string> render_attachment_html(const Attachment& attachment, const RenderContext& ctx) {
  return render_attachment_html_at(attachment, ctx, 0);
}

// --- Content -------------------------------------------------------------------------------------

Result<void> load_into(Dom& dom, Node* fragment, std::string_view html, const RenderContext& ctx) {
  if (auto parsed = parse_into(dom, fragment, ruby_strip(html)); !parsed) {
    return fail(parsed.error());
  }
  if (auto r = convert_trix_attachments(dom, fragment, ctx); !r) return r;
  for (Node* node : attachment_nodes(fragment)) {
    if (auto r = set_inner_html(dom, node, ""); !r) return fail(r.error());
  }
  for (Node* gallery : attachment_gallery_nodes(fragment)) {
    const std::string html_in_div = "<div>" + inner_html(gallery) + "</div>";
    if (auto r = replace_with_html(dom, gallery, html_in_div); !r) return fail(r.error());
  }
  return {};
}

Result<Content> Content::load(std::string_view html, const RenderContext& ctx) {
  Content content;
  content.root = content.dom.root();
  if (auto r = load_into(content.dom, content.root, html, ctx); !r) {
    return std::unexpected(r.error());
  }
  return content;
}

Result<Content> Content::wrap(std::string_view html) {
  Content content;
  content.root = content.dom.root();
  if (auto parsed = parse_into(content.dom, content.root, ruby_strip(html)); !parsed) {
    return fail(parsed.error());
  }
  return content;
}

std::string Content::to_html() const {
  return richtext::to_html(root);
}

Result<std::string> Content::to_plain_text(const RenderContext& ctx) const {
  Dom copy = clone_dom(dom);
  Node* copy_root = copy.root();
  for (Node* node : attachment_nodes(copy_root)) {
    if (auto r = sanitize_content_attribute(copy, node); !r) return std::unexpected(r.error());
    auto attachment = attachment_from_node(node, ctx);
    if (!attachment) return std::unexpected(attachment.error());
    PlainTextRepresentation plain = attachment_plain_text(*attachment);
    if (plain.is_content) {
      Node* fragment = new_fragment(copy);
      if (auto r = load_into(copy, fragment, plain.text, ctx); !r) return std::unexpected(r.error());
      const std::vector<Node*> kids = children(fragment);
      replace_with_nodes(copy, node, kids);
    } else if (auto r = replace_with_html(copy, node, plain.text); !r) {
      return fail(r.error());
    }
  }
  return node_to_plain_text(copy_root);
}

Result<std::string> Content::render(const RenderContext& ctx) const {
  return render_nested(ctx, 0);
}

Result<std::string> Content::render_nested(const RenderContext& ctx, int depth) const {
  Dom copy = clone_dom(dom);
  Node* copy_root = copy.root();
  if (auto r = render_attachments(copy, copy_root, ctx, depth); !r) return std::unexpected(r.error());
  if (auto r = render_attachment_galleries(copy, copy_root, ctx, depth); !r) return std::unexpected(r.error());
  auto sanitized = sanitize(richtext::to_html(copy_root), SafeList::action_text());
  if (!sanitized) {
    return fail(sanitized.error());
  }
  return std::move(*sanitized);
}

Result<std::string> Content::to_rendered_html_with_layout(const RenderContext& ctx) const {
  auto rendered = render(ctx);
  if (!rendered) {
    return rendered;
  }
  return "<div class=\"lexxy-content\">\n  " + *rendered + "\n</div>\n";
}

namespace {

// A content attachment renders through `ContentAttachment#to_html`: the content partial, with no layout.
Result<std::string> render_content_at(std::string_view content, const RenderContext& ctx, int depth) {
  if (depth >= kMaxContentAttachmentDepth) {
    return std::string();
  }
  auto loaded = Content::load(content, ctx);
  if (!loaded) return std::unexpected(loaded.error());
  auto rendered = loaded->render_nested(ctx, depth + 1);
  if (!rendered) return rendered;
  return *rendered + "\n";
}

}  // namespace

}  // namespace campfire::richtext
