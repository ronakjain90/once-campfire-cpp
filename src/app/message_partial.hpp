// The message display partials that the rooms area (A2) owns, as the message area (A3) calls them.
// Rails: app/views/messages/_message.html.erb, _presentation.html.erb, boosts/_boosts.html.erb and
// Messages::AttachmentPresentation (app/models/messages/attachment_presentation.rb). Rust:
// crates/views/src/messages.rs.
//
// A3 writes the controllers and the broadcasts. The partials are templates in src/views/messages/ (the names that
// tools/ctc.py gives them). This header declares them for the code of the app.
#pragma once

#include "core/out.hpp"
#include "views/context.hpp"
#include "views/messages/types.hpp"

namespace campfire::views::messages {

// `render message`: `messages/_message`, whose body is `cache [ message, "presentation-v3" ]`.
void message(Out& out, const ViewContext& ctx, const MessageView& message);
// `render "messages/presentation", message: message`: what `MessagesController#update` broadcasts.
void presentation(Out& out, const ViewContext& ctx, const MessageView& message);
// `message_attachment_presentation(message)`: the attachment of a message, as `messages/edit` shows it.
void attachment_presentation(Out& out, const ViewContext& ctx, const AttachmentView& attachment);

}  // namespace campfire::views::messages

// The partials in `messages/boosts/` have the names that tools/ctc.py gives them.
namespace campfire::views::messages::boosts {

// `render "messages/boosts/boosts", message: message`: the boosts frame of a message.
void boosts(Out& out, const ViewContext& ctx, const MessageView& view);
// `render "messages/boosts/boost", boost: boost`: `messages/boosts/_boost`, whose body is `cache boost`.
void boost(Out& out, const ViewContext& ctx, const BoostView& boost);

}  // namespace campfire::views::messages::boosts
