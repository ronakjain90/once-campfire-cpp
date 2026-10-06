// Messages::ByBotsController: the bot API under /rooms/:room_id/:bot_key/messages. Rails:
// app/controllers/messages/by_bots_controller.rb. Rust: crates/campfire/src/controllers/messages/by_bots.rs.
#include "app/broadcasts.hpp"
#include "app/concerns.hpp"
#include "app/controllers/common.hpp"
#include "app/dispatch.hpp"
#include "app/message_actions.hpp"
#include "compat/ruby.hpp"
#include "richtext/text_util.hpp"
#include "routes/routes.hpp"

namespace campfire::app::controllers {

namespace {

using messages::db_failure;

concerns::Before bot_before() {
  return concerns::Before{}.allow_bot_access();
}

// `set_room`: `Current.user.rooms.find_by(id: params[:room_id])`, else `head :not_found`.
Flow<models::RoomRef> set_bot_room(Rq& rq) {
  const auto room_id = rq.param_str("room_id").and_then(compat::integer_cast);
  if (room_id && rq.current_user() != nullptr) {
    auto room = models::room_refs::find_for_user(rq.db(), rq.arena(), rq.current_user()->id, *room_id);
    if (!room) return db_failure(room.error());
    if (*room) return std::move(**room);
  }
  return halt(concerns::head_in_before_action(rq, 404));
}

// `head :unprocessable_content if params[:attachment].blank? && raw_request_body.blank?`
Flow<void> ensure_body_or_attachment_present(Rq& rq) {
  const req::Param* attachment = rq.params().get("attachment");
  const bool attachment_blank = attachment == nullptr || attachment->is_blank();
  if (attachment_blank && richtext::is_blank(rq.raw_post())) return halt(concerns::head_in_before_action(rq, 422));
  return {};
}

// `params[:attachment] ? params.permit(:attachment) : { body: raw_request_body }`
messages::MessageParams bot_message_params(Rq& rq) {
  messages::MessageParams out;
  const req::Param* attachment = rq.params().get("attachment");
  if (attachment != nullptr && !attachment->is_null()) {
    const req::ParamMap permitted = rq.params().permit({"attachment"}, rq.ctx.resource());
    out.attachment = messages::attachment_param(permitted);
  } else {
    out.body = std::string(rq.raw_post());
  }
  return out;
}

// `@room.messages.find(params[:before])` and friends (`find_paged_messages`).
Flow<std::vector<models::Message>> find_paged_messages(Rq& rq, const models::RoomRef& room) {
  const auto find = [&](const req::Param& param) -> Flow<models::Message> {
    const auto text = param.as_str();
    const auto id = text ? compat::integer_cast(*text) : std::nullopt;
    if (!id) return fail_with(ErrorKind::NotFound, "Couldn't find Message");
    auto message = models::messages::find_in_room(rq.db(), rq.arena(), room.id, *id);
    if (!message) return db_failure(message.error());
    if (!*message) return fail_with(ErrorKind::NotFound, "Couldn't find Message");
    return std::move(**message);
  };
  const req::Param* before = rq.params().get("before");
  const req::Param* after = rq.params().get("after");
  Result<std::vector<models::Message>> page;
  if (before != nullptr && before->is_present()) {
    auto message = find(*before);
    if (!message) return std::unexpected(std::move(message.error()));
    page = models::messages::page_before(rq.db(), rq.arena(), room.id, *message);
  } else if (after != nullptr && after->is_present()) {
    auto message = find(*after);
    if (!message) return std::unexpected(std::move(message.error()));
    page = models::messages::page_after(rq.db(), rq.arena(), room.id, *message);
  } else {
    page = models::messages::last_page(rq.db(), rq.arena(), room.id);
  }
  if (!page) return db_failure(page.error());
  return std::move(*page);
}

// `X-Total-Count`, and a `Link` to the next page when there is one.
Flow<void> set_pagination_headers(Rq& rq, const models::RoomRef& room, const std::vector<models::Message>& messages) {
  auto count = models::messages::count_in_room(rq.db(), rq.arena(), room.id);
  if (!count) return db_failure(count.error());
  rq.set_header("x-total-count", std::to_string(*count));
  if (messages.empty()) return {};
  const req::Param* after = rq.params().get("after");
  const bool after_present = after != nullptr && after->is_present();
  std::string_view key;
  std::int64_t id = 0;
  if (after_present) {
    auto more = models::messages::exists_after(rq.db(), rq.arena(), room.id, messages.back());
    if (!more) return db_failure(more.error());
    if (*more) {
      key = "after";
      id = messages.back().id;
    }
  } else {
    auto more = models::messages::exists_before(rq.db(), rq.arena(), room.id, messages.front());
    if (!more) return db_failure(more.error());
    if (*more) {
      key = "before";
      id = messages.front().id;
    }
  }
  if (!key.empty()) {
    const std::string bot_key(rq.param_str("bot_key").value_or(""));
    const std::string url = rq.url_for(campfire::routes::room_bot_messages(room.id, bot_key)) + "?" + std::string(key) +
                            "=" + std::to_string(id);
    rq.set_header("link", "<" + url + ">; rel=\"next\"");
  }
  return {};
}

// `render :show` (messages/by_bots/show.json.jbuilder)
Flow<net::Response> render_show(Rq& rq, const models::Message& message) {
  MessagePresenter presenter(rq.db(), rq.arena(), rq.app, std::string(rq.info.host()));
  auto json = presenter.message_json(message, rq.url_for(""));
  if (!json) return db_failure(json.error());
  return rq.json(200, *json);
}

Task<Flow<net::Response>> bots_index(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, bot_before());
  if (!before) co_return std::unexpected(std::move(before.error()));
  auto room = set_bot_room(rq);
  if (!room) co_return std::unexpected(std::move(room.error()));
  auto page = find_paged_messages(rq, *room);
  if (!page) co_return std::unexpected(std::move(page.error()));
  if (auto headers = set_pagination_headers(rq, *room, *page); !headers) {
    co_return std::unexpected(std::move(headers.error()));
  }
  const req::Format offered[] = {&req::mime::JSON};
  if (auto format = rq.respond_to(offered); !format) co_return std::unexpected(std::move(format.error()));
  MessagePresenter presenter(rq.db(), rq.arena(), rq.app, std::string(rq.info.host()));
  const std::string base_url = rq.url_for("");
  compat::json::Value::Array array;
  for (const models::Message& message : *page) {
    auto json = presenter.message_json(message, base_url);
    if (!json) co_return db_failure(json.error());
    array.push_back(std::move(*json));
  }
  co_return rq.json(200, compat::json::Value(std::move(array)));
}

Task<Flow<net::Response>> bots_create(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, bot_before());
  if (!before) co_return std::unexpected(std::move(before.error()));
  auto room = set_bot_room(rq);
  if (!room) co_return std::unexpected(std::move(room.error()));
  if (auto present = ensure_body_or_attachment_present(rq); !present)
    co_return std::unexpected(std::move(present.error()));
  // MessagesController#create
  auto message = co_await messages::create_message(rq, *room, bot_message_params(rq));
  if (!message) co_return std::unexpected(std::move(message.error()));
  auto html = messages::broadcast_create(rq, *room, *message);
  if (!html) co_return std::unexpected(std::move(html.error()));
  if (auto hooks = messages::deliver_webhooks_to_bots(rq, *room, *message); !hooks) {
    co_return std::unexpected(std::move(hooks.error()));
  }
  net::Response response = rq.head(201);
  response.add_copy("location", rq.url_for(campfire::routes::message(message->id)));
  co_return response;
}

Task<Flow<net::Response>> bots_update(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, bot_before());
  if (!before) co_return std::unexpected(std::move(before.error()));
  auto room = set_bot_room(rq);
  if (!room) co_return std::unexpected(std::move(room.error()));
  auto message = messages::set_message(rq, *room);
  if (!message) co_return std::unexpected(std::move(message.error()));
  if (auto can = messages::ensure_can_administer(rq, *message); !can) co_return std::unexpected(std::move(can.error()));
  // MessagesController#update
  auto updated = co_await messages::update_message(rq, std::move(*message), bot_message_params(rq));
  if (!updated) co_return std::unexpected(std::move(updated.error()));
  if (auto replaced = messages::broadcast_replace(rq, *room, *updated); !replaced) {
    co_return std::unexpected(std::move(replaced.error()));
  }
  const req::Format offered[] = {&req::mime::HTML, &req::mime::JSON};
  auto format = rq.respond_to(offered);
  if (!format) co_return std::unexpected(std::move(format.error()));
  if (*format == &req::mime::JSON) co_return render_show(rq, *updated);
  co_return rq.redirect_to(rq.url_for(campfire::routes::room_message(room->id, updated->id)));
}

Task<Flow<net::Response>> bots_destroy(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, bot_before());
  if (!before) co_return std::unexpected(std::move(before.error()));
  auto room = set_bot_room(rq);
  if (!room) co_return std::unexpected(std::move(room.error()));
  auto message = messages::set_message(rq, *room);
  if (!message) co_return std::unexpected(std::move(message.error()));
  if (auto can = messages::ensure_can_administer(rq, *message); !can) co_return std::unexpected(std::move(can.error()));
  if (auto gone = co_await messages::destroy_message(rq, *room, *message); !gone) {
    co_return std::unexpected(std::move(gone.error()));
  }
  co_return rq.head(204);
}

}  // namespace

}  // namespace campfire::app::controllers

namespace campfire::routes::messages_by_bots {

Task<net::Response> index(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::bots_index);
}
Task<net::Response> create(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::bots_create);
}
Task<net::Response> update(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::bots_update);
}
Task<net::Response> destroy(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::bots_destroy);
}

}  // namespace campfire::routes::messages_by_bots
