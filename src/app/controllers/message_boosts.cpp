// Messages::BoostsController and Messages::Boosts::ByBotsController. Rails: app/controllers/messages/boosts_controller.rb,
// app/controllers/messages/boosts/by_bots_controller.rb. Rust: crates/campfire/src/controllers/messages/boosts.rs and
// messages/boosts/by_bots.rs.
#include "app/broadcasts.hpp"
#include "app/concerns.hpp"
#include "app/controllers/common.hpp"
#include "app/dispatch.hpp"
#include "app/message_actions.hpp"
#include "app/message_partial.hpp"
#include "compat/ruby.hpp"
#include "richtext/text_util.hpp"
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

// `Current.user.reachable_messages.find(params[:message_id])`
Flow<models::Message> set_reachable_message(Rq& rq) {
  const models::User* user = rq.current_user();
  const auto id = rq.param_str("message_id").and_then(compat::integer_cast);
  if (user == nullptr || !id) return fail_with(ErrorKind::NotFound, "Couldn't find Message");
  auto message = models::messages::find_reachable(rq.db(), rq.arena(), user->id, *id);
  if (!message) return db_failure(message.error());
  if (!*message) return fail_with(ErrorKind::NotFound, "Couldn't find Message");
  return std::move(**message);
}

// `@message.boosts.find_by!(id: params[:id], booster: Current.user)`
Flow<models::Boost> set_boost(Rq& rq, const models::Message& message) {
  const models::User* user = rq.current_user();
  const auto id = rq.param_str("id").and_then(compat::integer_cast);
  if (user == nullptr || !id) return fail_with(ErrorKind::NotFound, "Couldn't find Boost");
  auto boost = models::boosts::find_by_message_and_booster(rq.db(), rq.arena(), message.id, *id, user->id);
  if (!boost) return db_failure(boost.error());
  if (!*boost) return fail_with(ErrorKind::NotFound, "Couldn't find Boost");
  return std::move(**boost);
}

// `@message.boosts.create!(content:)`, boosted by `Current.user`.
Task<Flow<models::Boost>> create_boost(Rq& rq, const models::Message& message, std::optional<std::string> content) {
  // A nil content breaks the NOT NULL of the column: `ActiveRecord::NotNullViolation`, a 500.
  if (!content) co_return fail_internal("NOT NULL constraint failed: boosts.content");
  MessagePresenter presenter(rq.db(), rq.arena(), rq.app, std::string(rq.info.host()));
  auto plain_text = presenter.plain_text_body(message);
  if (!plain_text) co_return db_failure(plain_text.error());
  const std::int64_t booster_id = rq.current_user()->id;
  auto written = co_await rq.app.db->write(rq.ctx.scheduler(), [&](db::Tx& tx) {
    return models::boosts::create(tx, message.id, booster_id, *content, *plain_text);
  });
  if (!written) co_return db_failure(written.error());
  co_return std::move(*written);
}

Flow<models::RoomRef> room_of(Rq& rq, const models::Message& message) {
  auto room = models::room_refs::find(rq.db(), rq.arena(), message.room_id);
  if (!room) return db_failure(room.error());
  if (!*room) return fail_with(ErrorKind::NotFound, "Couldn't find Room");
  return std::move(**room);
}

// `@boost.destroy!`, then `broadcast_remove`.
Task<Flow<void>> destroy_boost(Rq& rq, const models::Message& message, const models::Boost& boost) {
  MessagePresenter presenter(rq.db(), rq.arena(), rq.app, std::string(rq.info.host()));
  auto plain_text = presenter.plain_text_body(message);
  if (!plain_text) co_return db_failure(plain_text.error());
  auto written = co_await rq.app.db->write(rq.ctx.scheduler(),
                                           [&](db::Tx& tx) { return models::boosts::destroy(tx, boost, *plain_text); });
  if (!written) co_return db_failure(written.error());
  auto room = room_of(rq, message);
  if (!room) co_return std::unexpected(std::move(room.error()));
  broadcasts::boost_remove(rq.app, *room, boost.id);
  co_return Flow<void>{};
}

// `broadcast_create`: `messages/boosts/_boost` appended to the boosts of the message.
Flow<void> broadcast_boost(Rq& rq, const models::Message& message, const models::Boost& boost) {
  auto ctx = messages::detached_context(rq);
  if (!ctx) return std::unexpected(std::move(ctx.error()));
  MessagePresenter presenter(rq.db(), rq.arena(), rq.app, std::string(rq.info.host()));
  auto view = presenter.boost(boost);
  if (!view) return db_failure(view.error());
  auto room = room_of(rq, message);
  if (!room) return std::unexpected(std::move(room.error()));
  Out out;
  views::messages::boosts::boost(out, *ctx, *view);
  broadcasts::boost_append(rq.app, *room, message.client_message_id, out.to_string());
  return {};
}

std::optional<std::string> boost_content(Rq& rq, std::optional<Failure>& failure) {
  auto required = rq.params().require("boost");
  if (!required) {
    failure.emplace(fail_with(ErrorKind::ParameterMissing, required.error().message).error());
    return std::nullopt;
  }
  const req::ParamMap* hash = (*required)->as_hash();
  if (hash == nullptr) return std::nullopt;
  const req::ParamMap permitted = hash->permit({"content"}, rq.ctx.resource());
  const req::Param* content = permitted.get("content");
  if (content == nullptr) return std::nullopt;
  if (const auto text = content->as_str()) return std::string(*text);
  return std::nullopt;
}

Task<Flow<net::Response>> boosts_index(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  auto message = set_reachable_message(rq);
  if (!message) co_return std::unexpected(std::move(message.error()));
  if (auto ok = accept(rq, {&req::mime::HTML}); !ok) co_return std::unexpected(std::move(ok.error()));
  MessagePresenter presenter(rq.db(), rq.arena(), rq.app, std::string(rq.info.host()));
  auto view = presenter.message(*message);
  if (!view) co_return db_failure(view.error());
  co_return messages::content_page(rq, 200, false, [&](Out& out, const views::ViewContext& ctx) {
    views::messages::boosts::boosts(out, ctx, *view);
  });
}

Task<Flow<net::Response>> boosts_new(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  auto message = set_reachable_message(rq);
  if (!message) co_return std::unexpected(std::move(message.error()));
  if (auto ok = accept(rq, {&req::mime::HTML}); !ok) co_return std::unexpected(std::move(ok.error()));
  MessagePresenter presenter(rq.db(), rq.arena(), rq.app, std::string(rq.info.host()));
  auto view = presenter.message(*message);
  if (!view) co_return db_failure(view.error());
  const views::messages::UserView user = user_view(rq.app, *rq.current_user());
  co_return messages::content_page(rq, 200, false, [&](Out& out, const views::ViewContext& ctx) {
    views::messages::boosts::new_(out, ctx, *view, user);
  });
}

Task<Flow<net::Response>> boosts_create(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  auto message = set_reachable_message(rq);
  if (!message) co_return std::unexpected(std::move(message.error()));
  std::optional<Failure> failure;
  auto content = boost_content(rq, failure);
  if (failure) co_return std::unexpected(std::move(*failure));
  auto boost = co_await create_boost(rq, *message, std::move(content));
  if (!boost) co_return std::unexpected(std::move(boost.error()));
  if (auto sent = broadcast_boost(rq, *message, *boost); !sent) co_return std::unexpected(std::move(sent.error()));
  co_return rq.redirect_to(rq.url_for(campfire::routes::message_boosts(message->id)));
}

Task<Flow<net::Response>> boosts_destroy(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  auto message = set_reachable_message(rq);
  if (!message) co_return std::unexpected(std::move(message.error()));
  auto boost = set_boost(rq, *message);
  if (!boost) co_return std::unexpected(std::move(boost.error()));
  if (auto gone = co_await destroy_boost(rq, *message, *boost); !gone) co_return std::unexpected(std::move(gone.error()));
  // No destroy template: `head :no_content`.
  co_return rq.head(204);
}

// ---- Messages::Boosts::ByBotsController -----------------------------------------------------------------------

concerns::Before bot_before() {
  return concerns::Before{}.allow_bot_access();
}

// `Current.user.rooms.find_by(id: params[:room_id])`, then `room.messages.find_by(id: params[:message_id])`.
// `head :not_found unless @message`
Flow<models::Message> set_bot_message(Rq& rq) {
  const auto room_id = rq.param_str("room_id").and_then(compat::integer_cast);
  const auto message_id = rq.param_str("message_id").and_then(compat::integer_cast);
  if (room_id && message_id && rq.current_user() != nullptr) {
    auto room = models::room_refs::find_for_user(rq.db(), rq.arena(), rq.current_user()->id, *room_id);
    if (!room) return db_failure(room.error());
    if (*room) {
      auto message = models::messages::find_in_room(rq.db(), rq.arena(), (*room)->id, *message_id);
      if (!message) return db_failure(message.error());
      if (*message) return std::move(**message);
    }
  }
  return halt(concerns::head_in_before_action(rq, 404));
}

Task<Flow<net::Response>> boost_bots_create(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, bot_before());
  if (!before) co_return std::unexpected(std::move(before.error()));
  auto message = set_bot_message(rq);
  if (!message) co_return std::unexpected(std::move(message.error()));
  // `head :unprocessable_content if raw_request_body.blank?`
  const std::string_view body = rq.raw_post();
  if (richtext::is_blank(body)) co_return halt(concerns::head_in_before_action(rq, 422));
  auto boost = co_await create_boost(rq, *message, std::string(body));
  if (!boost) co_return std::unexpected(std::move(boost.error()));
  if (auto sent = broadcast_boost(rq, *message, *boost); !sent) co_return std::unexpected(std::move(sent.error()));
  // `render :show, status: :created` (messages/boosts/by_bots/show.json.jbuilder)
  MessagePresenter presenter(rq.db(), rq.arena(), rq.app, std::string(rq.info.host()));
  const std::string base_url = rq.url_for("");
  auto json = presenter.boost_json(*boost, *message, base_url);
  if (!json) co_return db_failure(json.error());
  co_return rq.json(201, *json);
}

Task<Flow<net::Response>> boost_bots_destroy(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, bot_before());
  if (!before) co_return std::unexpected(std::move(before.error()));
  auto message = set_bot_message(rq);
  if (!message) co_return std::unexpected(std::move(message.error()));
  // `set_boost`: `rescue ActiveRecord::RecordNotFound` is `head :not_found`.
  auto boost = set_boost(rq, *message);
  if (!boost) {
    if (const auto* error = std::get_if<HttpError>(&boost.error());
        error != nullptr && error->kind == ErrorKind::NotFound) {
      co_return halt(concerns::head_in_before_action(rq, 404));
    }
    co_return std::unexpected(std::move(boost.error()));
  }
  if (auto gone = co_await destroy_boost(rq, *message, *boost); !gone) co_return std::unexpected(std::move(gone.error()));
  co_return rq.head(204);
}

}  // namespace

}  // namespace campfire::app::controllers

namespace campfire::routes::messages_boosts {

Task<net::Response> index(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::boosts_index);
}
Task<net::Response> new_(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::boosts_new);
}
Task<net::Response> create(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::boosts_create);
}
Task<net::Response> destroy(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::boosts_destroy);
}

}  // namespace campfire::routes::messages_boosts

namespace campfire::routes::messages_boosts_by_bots {

Task<net::Response> create(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::boost_bots_create);
}
Task<net::Response> destroy(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::boost_bots_destroy);
}

}  // namespace campfire::routes::messages_boosts_by_bots
