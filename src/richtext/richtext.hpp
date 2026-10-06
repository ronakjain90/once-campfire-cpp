// What a stored message body looks like on a page, in the editor and as plain text.
// Rails: app/helpers/messages_helper.rb, app/helpers/rich_text_helper.rb, lib/rails_ext/.
// Rust: crates/richtext/src/lib.rs, crates/campfire/src/rich_text.rs
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "richtext/attachables.hpp"
#include "richtext/error.hpp"

namespace campfire::richtext {

// How messages/_message.html.erb shows a text message body.
struct Presentation {
  enum class Kind : std::uint8_t {
    Html,
    Unrenderable,  // `message_tag` rescued an exception: render messages/_unrenderable.html.erb
  };
  Kind kind = Kind::Html;
  std::string html;
};

// The text branch of `MessagesHelper#message_presentation`:
// `auto_link h(TextMessagePresentationFilters.apply(message.body.body)), ...`.
[[nodiscard]] Result<std::string> message_presentation(std::string_view body, const RenderContext& ctx);

// `message_presentation` with its rescue: an exception renders "", unless the log line of the
// rescue raises too: then the message is unrenderable.
[[nodiscard]] Presentation present_message(std::string_view body, const RenderContext& ctx);

// `message.body.to_plain_text` (Action Text `RichText#to_plain_text`).
[[nodiscard]] Result<std::string> to_plain_text(std::string_view body, const RenderContext& ctx);

// The value that the Lexxy editor gets when it edits a message: `RichTextHelper#editable_body`, then
// `render_custom_attachments_in` of Lexxy. nullopt when the body is blank (no `value` attribute). The
// caller escapes it for the `<lexxy-editor value="...">` attribute.
[[nodiscard]] Result<std::optional<std::string>> editable_value(std::string_view body, const RenderContext& ctx);

// `Message::Mentionee#mentioned_users`: the users in attachments with a verified SGID, once each.
[[nodiscard]] Result<std::vector<MentionUser>> mentioned_users(std::string_view body, const RenderContext& ctx);

// `TextMessagePresentationFilters.apply(content).to_html`
[[nodiscard]] Result<std::string> filtered_html(std::string_view body, const RenderContext& ctx);

// `Webhook#without_recipient_mentions`: the plain body without the "@Name" of the bot, with Unicode
// white space off both ends.
[[nodiscard]] std::string without_recipient_mentions(std::string_view plain_text, std::string_view recipient_name);

}  // namespace campfire::richtext
