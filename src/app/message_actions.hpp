// The write flows of messages that the controllers share. Rails: app/controllers/messages_controller.rb (create,
// update, destroy, deliver_webhooks_to_bots), app/models/message/{attachment,broadcasts}.rb. Rust:
// crates/campfire/src/controllers/messages.rs.
#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "app/flow.hpp"
#include "app/message_presenter.hpp"
#include "app/rq.hpp"
#include "core/task.hpp"
#include "models/boost.hpp"
#include "models/message.hpp"
#include "models/room_ref.hpp"
#include "req/param.hpp"
#include "views/context.hpp"

namespace campfire::app::messages {

// What `attachment=` does with a permitted param: nothing, remove, replace with an upload, or raise.
struct AttachmentParam {
  enum class Kind : std::uint8_t { Unchanged, Delete, Upload, Invalid };
  Kind kind = Kind::Unchanged;
  std::shared_ptr<req::UploadedFile> file;
};

// What `create_with_attachment!` and `update!` receive.
struct MessageParams {
  std::optional<std::string> body;
  AttachmentParam attachment;
  std::optional<std::string> client_message_id;
};

// A failed read or write: a missing record is a 404, anything else a 500.
[[nodiscard]] std::unexpected<Failure> db_failure(const Error& error);

// `params.require(:message).permit(:body, :attachment, :client_message_id)`
[[nodiscard]] Flow<MessageParams> message_params(Rq& rq);
// `params.permit(:attachment)` applied to the key `attachment` of `permitted`.
[[nodiscard]] AttachmentParam attachment_param(const req::ParamMap& permitted);

// `RoomScoped#set_room`: `Current.user.memberships.find_by!(room_id: params[:room_id])`, then its room. A missing
// membership is `ActiveRecord::RecordNotFound`: a 404.
[[nodiscard]] Flow<models::RoomRef> set_room(Rq& rq);
// `@room.messages.find(params[:id])`
[[nodiscard]] Flow<models::Message> set_message(Rq& rq, const models::RoomRef& room);
// `head :forbidden unless Current.user.can_administer?(@message)`
[[nodiscard]] Flow<void> ensure_can_administer(Rq& rq, const models::Message& message);

// `@room.messages.create_with_attachment!(attributes)`, with `process_attachment`.
[[nodiscard]] Task<Flow<models::Message>> create_message(Rq& rq, const models::RoomRef& room, MessageParams params);
// `@message.update!(attributes)`
[[nodiscard]] Task<Flow<models::Message>> update_message(Rq& rq, models::Message message, MessageParams params);
// `@message.destroy`, then `@message.broadcast_remove`.
[[nodiscard]] Task<Flow<void>> destroy_message(Rq& rq, const models::RoomRef& room, const models::Message& message);

// `@message.broadcast_create`: the message partial appended to the room, then the unread pings. Gives the HTML of the
// partial: the response of `create` shows the same fragment.
[[nodiscard]] Flow<std::string> broadcast_create(Rq& rq, const models::RoomRef& room, const models::Message& message);
// `broadcast_replace_to @room, :messages, target: [ @message, :presentation ], ...`
[[nodiscard]] Flow<void> broadcast_replace(Rq& rq, const models::RoomRef& room, const models::Message& message);
// `deliver_webhooks_to_bots`: every active bot of a direct room, else every mentioned active bot, but not the creator.
[[nodiscard]] Flow<void> deliver_webhooks_to_bots(Rq& rq, const models::RoomRef& room, const models::Message& message);

// A content-only template in a layout. Rails wraps it in the application layout, or in turbo-rails' frame layout when
// the request has a `Turbo-Frame` header. `always_application` is for a controller that declares its own `layout`
// (`MessagesController`: `layout false, only: :index`), which replaces that choice.
[[nodiscard]] Flow<net::Response> content_page(Rq& rq, int status, bool always_application,
                                               const std::function<void(Out&, const views::ViewContext&)>& content);

// The view context of `ApplicationController.render` outside a request: no user, no flash, no CSRF tokens. The base URL
// has the protocol and the host of the request, but no port.
[[nodiscard]] Flow<views::ViewContext> detached_context(Rq& rq);
}  // namespace campfire::app::messages
