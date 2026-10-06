// Rails: app/helpers/messages_helper.rb, app/helpers/rich_text_helper.rb, app/models/message/mentionee.rb,
// app/models/webhook.rb. Rust: crates/richtext/src/lib.rs
#include "richtext/richtext.hpp"

#include <algorithm>

#include "compat/json.hpp"
#include "richtext/autolink.hpp"
#include "richtext/content.hpp"
#include "richtext/filters.hpp"
#include "richtext/message_filters.hpp"
#include "richtext/sanitizer.hpp"
#include "richtext/text_util.hpp"
#include "richtext/tree.hpp"

namespace campfire::richtext {

Result<std::string> message_presentation(std::string_view body, const RenderContext& ctx) {
  auto content = Content::load(body, ctx);
  if (!content) return std::unexpected(content.error());
  auto filtered = apply_message_filters(std::move(*content), ctx);
  if (!filtered) return std::unexpected(filtered.error());
  auto rendered = filtered->to_rendered_html_with_layout(ctx);
  if (!rendered) return std::unexpected(rendered.error());
  auto linked = auto_link(*rendered, SafeList::auto_link());
  if (!linked) return fail(linked.error());
  return std::move(*linked);
}

Presentation present_message(std::string_view body, const RenderContext& ctx) {
  auto html = message_presentation(body, ctx);
  if (html) {
    return {Presentation::Kind::Html, std::move(*html)};
  }
  if (html.error().kind == Error::Kind::Unrenderable) {
    return {Presentation::Kind::Unrenderable, {}};
  }
  return {Presentation::Kind::Html, {}};
}

Result<std::string> to_plain_text(std::string_view body, const RenderContext& ctx) {
  auto content = Content::load(body, ctx);
  if (!content) return std::unexpected(content.error());
  return content->to_plain_text(ctx);
}

Result<std::string> filtered_html(std::string_view body, const RenderContext& ctx) {
  auto content = Content::load(body, ctx);
  if (!content) return std::unexpected(content.error());
  auto filtered = apply_message_filters(std::move(*content), ctx);
  if (!filtered) return std::unexpected(filtered.error());
  return filtered->to_html();
}

Result<std::optional<std::string>> editable_value(std::string_view body, const RenderContext& ctx) {
  // editable_body: each attachment is built again from its attachable, on the stored markup as it is.
  Dom first;
  if (auto parsed = parse_into(first, first.root(), ruby_strip(body)); !parsed) {
    return fail(parsed.error());
  }
  for (Node* node : attachment_nodes(first.root())) {
    auto attachment = attachment_from_node(node, ctx);
    if (!attachment) return std::unexpected(attachment.error());
    // A mention of a deleted user has nothing to edit, so it leaves the editor (Rails raises).
    if (attachment->attachable.kind == Attachable::Kind::Missing) {
      first.detach(node);
      continue;
    }
    auto content_type = attachable_content_type(attachment->attachable);
    if (!content_type) return std::unexpected(content_type.error());
    auto content = render_attachment_html(*attachment, ctx);
    if (!content) return std::unexpected(content.error());
    first.set_attr(node, "content-type", *content_type);
    first.set_attr(node, "content", *content);
  }
  const std::string editable = to_html(first.root());
  if (is_blank(editable)) {
    return std::optional<std::string>();
  }

  // Lexxy: an attachment with no url gets its rendered partial as a JSON string.
  Dom second;
  if (auto parsed = parse_into(second, second.root(), ruby_strip(editable)); !parsed) {
    return fail(parsed.error());
  }
  for (Node* node : attachment_nodes(second.root())) {
    auto url = attr_value(node, "url");
    if (!url || is_blank(*url)) {
      auto attachment = attachment_from_node(node, ctx);
      if (!attachment) return std::unexpected(attachment.error());
      auto content = render_attachment_html(*attachment, ctx);
      if (!content) return std::unexpected(content.error());
      second.set_attr(node, "content", compat::json::encode(compat::json::Value(*content)));
    }
  }
  return std::optional<std::string>(to_html(second.root()));
}

Result<std::vector<MentionUser>> mentioned_users(std::string_view body, const RenderContext& ctx) {
  auto content = Content::load(body, ctx);
  if (!content) return std::unexpected(content.error());
  std::vector<MentionUser> users;
  for (Node* node : attachment_nodes(content->root)) {
    Attachable attachable = action_text_attachable_from_node(node, ctx);
    if (attachable.kind == Attachable::Kind::User &&
        std::none_of(users.begin(), users.end(), [&](const MentionUser& u) { return u.id == attachable.user.id; })) {
      users.push_back(std::move(attachable.user));
    }
  }
  return users;
}

std::string without_recipient_mentions(std::string_view plain_text, std::string_view recipient_name) {
  const std::string needle = "@" + std::string(recipient_name);
  std::string replaced;
  if (needle.empty()) {
    replaced = std::string(plain_text);
  } else {
    std::size_t pos = 0;
    while (true) {
      const std::size_t at = plain_text.find(needle, pos);
      if (at == std::string_view::npos) {
        replaced.append(plain_text.substr(pos));
        break;
      }
      replaced.append(plain_text.substr(pos, at - pos));
      pos = at + needle.size();
    }
  }
  // Trim Unicode white space at both ends.
  std::size_t begin = 0;
  while (begin < replaced.size()) {
    std::size_t next = begin;
    if (!is_unicode_space(next_code_point(replaced, next))) break;
    begin = next;
  }
  std::size_t end = replaced.size();
  while (end > begin) {
    std::size_t start = end - 1;
    while (start > begin && (static_cast<unsigned char>(replaced[start]) & 0xC0u) == 0x80u) --start;
    std::size_t probe = start;
    if (!is_unicode_space(next_code_point(replaced, probe))) break;
    end = start;
  }
  return replaced.substr(begin, end - begin);
}

}  // namespace campfire::richtext
