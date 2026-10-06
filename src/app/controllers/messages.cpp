// MessagesController (create, show, edit, update, destroy). Rails: app/controllers/messages_controller.rb. Rust:
// crates/campfire/src/controllers/messages.rs. `index` belongs to the rooms area (A2).
#include "app/broadcasts.hpp"
#include "app/concerns.hpp"
#include "app/controllers/common.hpp"
#include "app/dispatch.hpp"
#include "app/message_actions.hpp"
#include "app/message_partial.hpp"
#include "routes/routes.hpp"
#include "views/templates.gen.hpp"

namespace campfire::app::controllers {

namespace {

using messages::db_failure;

Flow<void> accept(Rq& rq, std::initializer_list<req::Format> offered) {
  const std::vector<req::Format> list(offered);
  auto format = rq.respond_to(list);
  if (!format) return std::unexpected(std::move(format.error()));
  return {};
}

// `render action: :room_not_found` (inside the layout).
Flow<net::Response> render_room_not_found(Rq& rq) {
  if (auto ok = accept(rq, {&req::mime::HTML}); !ok) return std::unexpected(std::move(ok.error()));
  return messages::content_page(rq, 200, true,
                                [](Out& out, const views::ViewContext&) { views::messages::room_not_found(out); });
}

bool is_not_found(const Failure& failure) {
  const auto* error = std::get_if<HttpError>(&failure);
  return error != nullptr && error->kind == ErrorKind::NotFound;
}

// The turbo stream of `create`: `turbo_stream.append dom_id(@message.room, :messages), @message`.
std::string append_stream(const models::RoomRef& room, std::string_view message_html) {
  Out out;
  out.append_raw("<turbo-stream action=\"append\" target=\"");
  html_escape(out, broadcasts::room_dom_id(room, "messages"));
  out.append_raw("\"><template>");
  out.append_raw(message_html);
  out.append_raw("</template></turbo-stream>");
  return out.to_string();
}

Task<Flow<net::Response>> messages_create(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  // `set_room` runs inside the action: a room that is gone renders `room_not_found`.
  auto room = messages::set_room(rq);
  if (!room) {
    if (is_not_found(room.error())) co_return render_room_not_found(rq);
    co_return std::unexpected(std::move(room.error()));
  }
  auto params = messages::message_params(rq);
  if (!params) co_return std::unexpected(std::move(params.error()));
  auto message = co_await messages::create_message(rq, *room, std::move(*params));
  if (!message) co_return std::unexpected(std::move(message.error()));
  auto html = messages::broadcast_create(rq, *room, *message);
  if (!html) co_return std::unexpected(std::move(html.error()));
  if (auto hooks = messages::deliver_webhooks_to_bots(rq, *room, *message); !hooks) {
    co_return std::unexpected(std::move(hooks.error()));
  }
  if (auto ok = accept(rq, {&req::mime::TURBO_STREAM}); !ok) co_return std::unexpected(std::move(ok.error()));
  Out out(rq.ctx.resource());
  out.append_raw(append_stream(*room, *html));
  co_return rq.turbo_stream(std::move(out));
}

Task<Flow<net::Response>> messages_show(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  auto room = messages::set_room(rq);
  if (!room) co_return std::unexpected(std::move(room.error()));
  auto message = messages::set_message(rq, *room);
  if (!message) co_return std::unexpected(std::move(message.error()));
  if (auto ok = accept(rq, {&req::mime::HTML}); !ok) co_return std::unexpected(std::move(ok.error()));
  MessagePresenter presenter(rq.db(), rq.arena(), rq.app, std::string(rq.info.host()));
  auto view = presenter.message(*message);
  if (!view) co_return db_failure(view.error());
  co_return messages::content_page(
      rq, 200, true, [&](Out& out, const views::ViewContext& ctx) { views::messages::message(out, ctx, *view); });
}

Task<Flow<net::Response>> messages_edit(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  auto room = messages::set_room(rq);
  if (!room) co_return std::unexpected(std::move(room.error()));
  auto message = messages::set_message(rq, *room);
  if (!message) co_return std::unexpected(std::move(message.error()));
  if (auto can = messages::ensure_can_administer(rq, *message); !can) co_return std::unexpected(std::move(can.error()));
  if (auto ok = accept(rq, {&req::mime::HTML}); !ok) co_return std::unexpected(std::move(ok.error()));
  MessagePresenter presenter(rq.db(), rq.arena(), rq.app, std::string(rq.info.host()));
  auto editable = presenter.editable_body(*message);
  if (!editable) co_return db_failure(editable.error());
  auto view = presenter.message(*message);
  if (!view) co_return db_failure(view.error());
  co_return messages::content_page(rq, 200, true, [&](Out& out, const views::ViewContext& ctx) {
    std::string attachment_html;
    if (const views::messages::AttachmentView* attachment = view->attachment()) {
      Out inner;
      views::messages::attachment_presentation(inner, ctx, *attachment);
      attachment_html = inner.to_string();
    }
    std::optional<SafeHtml> attachment;
    if (view->attachment() != nullptr) attachment = SafeHtml::trusted(attachment_html);
    views::messages::edit(out, ctx, *view, *editable, attachment);
  });
}

Task<Flow<net::Response>> messages_update(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  auto room = messages::set_room(rq);
  if (!room) co_return std::unexpected(std::move(room.error()));
  auto message = messages::set_message(rq, *room);
  if (!message) co_return std::unexpected(std::move(message.error()));
  if (auto can = messages::ensure_can_administer(rq, *message); !can) co_return std::unexpected(std::move(can.error()));
  auto params = messages::message_params(rq);
  if (!params) co_return std::unexpected(std::move(params.error()));
  auto updated = co_await messages::update_message(rq, std::move(*message), std::move(*params));
  if (!updated) co_return std::unexpected(std::move(updated.error()));
  if (auto replaced = messages::broadcast_replace(rq, *room, *updated); !replaced) {
    co_return std::unexpected(std::move(replaced.error()));
  }
  const req::Format offered[] = {&req::mime::HTML, &req::mime::JSON};
  auto format = rq.respond_to(offered);
  if (!format) co_return std::unexpected(std::move(format.error()));
  // `format.json { render :show }`: this controller has no `messages/show.json` template.
  if (*format == &req::mime::JSON) co_return fail_internal("Missing template messages/show");
  co_return rq.redirect_to(rq.url_for(campfire::routes::room_message(room->id, updated->id)));
}

Task<Flow<net::Response>> messages_destroy(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  auto room = messages::set_room(rq);
  if (!room) co_return std::unexpected(std::move(room.error()));
  auto message = messages::set_message(rq, *room);
  if (!message) co_return std::unexpected(std::move(message.error()));
  if (auto can = messages::ensure_can_administer(rq, *message); !can) co_return std::unexpected(std::move(can.error()));
  if (auto gone = co_await messages::destroy_message(rq, *room, *message); !gone) {
    co_return std::unexpected(std::move(gone.error()));
  }
  if (auto ok = accept(rq, {&req::mime::TURBO_STREAM}); !ok) co_return std::unexpected(std::move(ok.error()));
  // `turbo_stream.remove @message`
  Out out(rq.ctx.resource());
  out.append_raw("<turbo-stream action=\"remove\" target=\"message_");
  html_escape(out, message->client_message_id);
  out.append_raw("\"></turbo-stream>");
  co_return rq.turbo_stream(std::move(out));
}

}  // namespace

}  // namespace campfire::app::controllers

namespace campfire::routes::messages_controller {

Task<net::Response> create(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::messages_create);
}
Task<net::Response> show(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::messages_show);
}
Task<net::Response> edit(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::messages_edit);
}
Task<net::Response> update(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::messages_update);
}
Task<net::Response> destroy(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::messages_destroy);
}

}  // namespace campfire::routes::messages_controller
