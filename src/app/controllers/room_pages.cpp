// RoomsController#show, MessagesController#index and Rooms::RefreshesController#show: the read side of the messages.
// Rails: app/controllers/rooms_controller.rb, messages_controller.rb, rooms/refreshes_controller.rb. Rust:
// crates/campfire/src/controllers/rooms.rs, messages.rs, rooms/refreshes.rs.
//
// The page cache key of these pages comes from the rows that the handler reads in the tracked scope (the room, the
// messages of the page and what the nav and the invitation print), and from the facets. The rows that a message
// fragment needs (the creator, the body, the boosts) are read with no tracking: the version of the message (its id and
// `updated_at`) stands for them, as the fragment cache key does in Rails.
#include <string>
#include <vector>

#include "app/broadcasts.hpp"
#include "app/concerns.hpp"
#include "app/controllers/rooms.hpp"
#include "app/dispatch.hpp"
#include "app/message_actions.hpp"
#include "app/message_presenter.hpp"
#include "app/page.hpp"
#include "app/platform.hpp"
#include "app/render_page.hpp"
#include "assets/assets.hpp"
#include "compat/global_id.hpp"
#include "compat/ruby.hpp"
#include "compat/turbo.hpp"
#include "core/time_format.hpp"
#include "models/account.hpp"
#include "models/message.hpp"
#include "models/room.hpp"
#include "models/user.hpp"
#include "routes/routes.hpp"
#include "views/helpers/users.hpp"
#include "views/messages/support.hpp"
#include "views/rooms/show.hpp"
#include "views/templates.gen.hpp"

namespace campfire::app::controllers {

namespace {

using views::messages::MessageItem;

// Reads without folding the statements into the page key. The scope comes back when the object goes.
class UntrackedReads {
 public:
  explicit UntrackedReads(Rq& rq) : conn_(&rq.db()), scope_(conn_->scope()) { conn_->set_scope(nullptr); }
  UntrackedReads(const UntrackedReads&) = delete;
  UntrackedReads& operator=(const UntrackedReads&) = delete;
  ~UntrackedReads() { conn_->set_scope(scope_); }

 private:
  db::Connection* conn_;
  db::DependencyScope* scope_;
};

views::RoomKind kind_of(std::string_view type) {
  if (type == models::kRoomDirect) return views::RoomKind::Direct;
  return type == models::kRoomClosed ? views::RoomKind::Closed : views::RoomKind::Open;
}

// `render @messages` with `cached: true`: a message whose fragment the cache holds needs no view.
Flow<std::vector<MessageItem>> message_items(Rq& rq, const std::vector<models::Message>& messages) {
  const UntrackedReads untracked(rq);
  MessagePresenter presenter(rq.db(), rq.arena(), rq.app, std::string(rq.info.host()));
  views::FragmentCache& cache = views::fragment_cache();
  std::vector<MessageItem> items;
  items.reserve(messages.size());
  for (const models::Message& message : messages) {
    MessageItem item;
    item.id = message.id;
    item.room_id = message.room_id;
    item.client_message_id = message.client_message_id;
    item.updated_at = message.updated_at;
    if (cache.enabled()) {
      // Shared with the cache: the page copies the fragment one time, into its body.
      if (auto html = cache.get(views::messages::message_fragment_key(message.id, message.updated_at))) {
        item.html = std::move(html);
        items.push_back(std::move(item));
        continue;
      }
    }
    auto view = presenter.message(message);
    if (!view) return messages::db_failure(view.error());
    item.view = std::move(*view);
    items.push_back(std::move(item));
  }
  return items;
}

// `room_display_name(room)` for the current user: the other members as a sentence, else the name of the user.
Flow<std::string> room_display_name(Rq& rq, const models::Room& room) {
  if (!room.is_direct()) return room.name.value_or("");
  auto members = models::users::of_room(rq.db(), rq.arena(), room.id);
  if (!members) return fail_internal(members.error().message);
  const models::User& current = *rq.current_user();
  std::vector<std::string> names;
  for (const models::User& member : *members) {
    if (member.id != current.id) names.push_back(member.name);
  }
  std::string sentence = to_sentence(names, " and ");
  if (sentence.find_first_not_of(" \t\n\v\f\r") == std::string::npos) return current.name;
  return sentence;
}

// The facets of what the room pages print about the browser: the notification help of the bell.
void add_platform_facets(db::DependencyScope& deps, const views::Platform& p) {
  std::string text = p.browser + "|" + p.operating_system + "|";
  for (const bool flag : {p.ios, p.android, p.mac, p.windows, p.chrome, p.firefox, p.safari, p.edge, p.mobile,
                          p.desktop, p.apple_messages}) {
    text.push_back(flag ? '1' : '0');
  }
  deps.facet("platform", text);
}

std::string room_gid_param(const models::Room& room) {
  return compat::global_id::GlobalId::make(room.type, std::to_string(room.id)).to_param();
}

}  // namespace

// `find_messages`: the page around `params[:message_id]` when the room has that message, else the last page.
Task<Flow<net::Response>> rooms_show(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  db::DependencyScope& deps = rq.track();
  auto room = set_room(rq, models::RoomScope::All);
  if (!room) co_return std::unexpected(std::move(room.error()));
  remember_last_room_visited(rq, *room);

  std::optional<models::Message> anchor;
  if (const auto raw = rq.param_str("message_id")) {
    if (const auto id = compat::integer_cast(*raw)) {
      auto found = models::messages::find_in_room(rq.db(), rq.arena(), room->id, *id);
      if (!found) co_return messages::db_failure(found.error());
      anchor = std::move(*found);
    }
  }
  std::vector<models::Message> page;
  if (anchor) {
    // `page_around`: the page before, the message, the page after.
    auto earlier = models::messages::page_before(rq.db(), rq.arena(), room->id, *anchor);
    if (!earlier) co_return messages::db_failure(earlier.error());
    auto later = models::messages::page_after(rq.db(), rq.arena(), room->id, *anchor);
    if (!later) co_return messages::db_failure(later.error());
    page = std::move(*earlier);
    page.push_back(*anchor);
    page.insert(page.end(), later->begin(), later->end());
  } else {
    auto last = models::messages::last_page(rq.db(), rq.arena(), room->id);
    if (!last) co_return messages::db_failure(last.error());
    page = std::move(*last);
  }
  // The implicit render finds only an HTML template.
  if (auto format = ensure_html(rq); !format) co_return std::unexpected(std::move(format.error()));
  auto layout = load_layout(rq);
  if (!layout) co_return std::unexpected(std::move(layout.error()));

  views::rooms::ShowView show;
  show.room.id = room->id;
  show.room.kind = kind_of(room->type);
  auto name = room_display_name(rq, *room);
  if (!name) co_return std::unexpected(std::move(name.error()));
  show.room.display_name = std::move(*name);
  if (const auto updated = parse_db(room->updated_at)) show.loaded_at = std::to_string(epoch_ms(*updated));
  show.user = current_user_view(rq);
  // `@room == Room.original && !@room.messages.paged?`
  auto original = models::rooms::original(rq.db(), rq.arena());
  if (!original) co_return fail_internal(original.error().message);
  if (*original && (*original)->id == room->id) {
    auto count = models::messages::count_in_room(rq.db(), rq.arena(), room->id);
    if (!count) co_return messages::db_failure(count.error());
    show.invitation = *count <= models::kMessagePageSize;
    if (show.invitation) {
      auto code = models::accounts::first_join_code(rq.db(), rq.arena());
      if (!code) co_return fail_internal(code.error().message);
      show.join_code = code->value_or("");
    }
  }
  // A pure function of the room's type and id: the worker keeps it (an HMAC for each request otherwise).
  std::string stream_key = "stream:";
  stream_key += room->type;
  stream_key += ':';
  stream_key += std::to_string(room->id);
  stream_key += ":messages";
  show.messages_stream_name = rq.worker.memo(stream_key, [&] {
    const std::string room_gid = room_gid_param(*room);
    const std::array<std::string_view, 2> stream{room_gid, "messages"};
    return compat::turbo::signed_stream_name(rq.app.secrets, stream);
  });

  const std::string title = show.room.display_name;
  PageSpec spec;
  spec.name = "rooms#show";
  spec.title = title;
  spec.body_class = "sidebar";
  spec.parts_etag = true;
  spec.head = [&](Out& out, const views::ViewContext& ctx) { views::rooms::show::head(out, ctx, show.room); };
  spec.nav = [&](Out& out, const views::ViewContext& ctx) { views::rooms::show::nav(out, ctx, show.room); };
  spec.sidebar = [](Out& out, const views::ViewContext&) {
    views::helpers::sidebar_turbo_frame_tag(out, campfire::routes::user_sidebar());
  };
  spec.content = [&](Out& out, const views::ViewContext& ctx) { views::rooms::show::content(out, ctx, show); };
  spec.footer = [&](Out& out, const views::ViewContext& ctx) { views::rooms::show::composer(out, ctx, show.room); };
  // The message fragments are read on a miss only: the page key has the rows of the page, and a message version stands
  // for what its fragment prints.
  spec.prepare = [&]() -> Flow<void> {
    auto items = message_items(rq, page);
    if (!items) return std::unexpected(std::move(items.error()));
    show.items = std::move(*items);
    return {};
  };
  spec.facets = [&](db::DependencyScope& scope, const LayoutData&) {
    add_platform_facets(scope, ApplicationPlatform(rq.user_agent()).to_view());
  };
  co_return render_page(rq, 200, spec, deps, *layout);
}

namespace {

// `@room.messages.find(params[:before])`: a missing message is `ActiveRecord::RecordNotFound`.
Flow<models::Message> find_paged_anchor(Rq& rq, std::int64_t room_id, const req::Param& param) {
  const auto text = param.as_str();
  const auto id = text ? compat::integer_cast(*text) : std::nullopt;
  if (!id) return fail_with(ErrorKind::NotFound, "Couldn't find Message");
  auto found = models::messages::find_in_room(rq.db(), rq.arena(), room_id, *id);
  if (!found) return messages::db_failure(found.error());
  if (!*found) return fail_with(ErrorKind::NotFound, "Couldn't find Message");
  return std::move(**found);
}

// `find_paged_messages`: the page before or after a message, else the last page.
Flow<std::vector<models::Message>> find_paged_messages(Rq& rq, std::int64_t room_id) {
  const req::Param* before = rq.params().get("before");
  const req::Param* after = rq.params().get("after");
  if (before != nullptr && !before->is_blank()) {
    auto anchor = find_paged_anchor(rq, room_id, *before);
    if (!anchor) return std::unexpected(std::move(anchor.error()));
    auto page = models::messages::page_before(rq.db(), rq.arena(), room_id, *anchor);
    if (!page) return messages::db_failure(page.error());
    return std::move(*page);
  }
  if (after != nullptr && !after->is_blank()) {
    auto anchor = find_paged_anchor(rq, room_id, *after);
    if (!anchor) return std::unexpected(std::move(anchor.error()));
    auto page = models::messages::page_after(rq.db(), rq.arena(), room_id, *anchor);
    if (!page) return messages::db_failure(page.error());
    return std::move(*page);
  }
  auto page = models::messages::last_page(rq.db(), rq.arena(), room_id);
  if (!page) return messages::db_failure(page.error());
  return std::move(*page);
}

// The context of the message partials outside a layout: they print asset paths and nothing else of the request.
views::ViewContext partial_context(Rq& rq) {
  views::ViewContext ctx;
  ctx.vapid_public_key = rq.app.config.vapid_public_key;
  ctx.asset_path = [](std::string_view source) {
    auto path = assets::asset_path(source);
    if (!path) throw std::runtime_error(path.error().message);  // Propshaft::MissingAssetError: a 500
    return std::move(*path);
  };
  return ctx;
}

// `Time.at(0, params[:since].to_i, :millisecond)` as the text of the database.
Flow<std::string> since_param(Rq& rq) {
  std::int64_t millis = 0;
  if (const req::Param* since = rq.params().get("since"); since != nullptr && !since->is_null()) {
    const auto text = since->as_str();
    // `to_i` is not defined for a hash or an array.
    if (!text) return fail_internal("undefined method 'to_i'");
    millis = compat::to_i(*text);
  }
  constexpr std::int64_t kMax = kMaxSeconds * 1000;
  constexpr std::int64_t kMin = kMinSeconds * 1000;
  millis = std::clamp(millis, kMin, kMax);
  const std::int64_t seconds = millis >= 0 ? millis / 1000 : -((-millis + 999) / 1000);
  const auto rest = static_cast<std::int32_t>((millis - seconds * 1000) * 1'000'000);
  return format_db(Timestamp{seconds, rest});
}

}  // namespace

// `layout false`, `fresh_when @messages`: the page of messages that the client loads while it scrolls.
Task<Flow<net::Response>> messages_index(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  db::DependencyScope& deps = rq.track();
  auto room = messages::set_room(rq);
  if (!room) co_return std::unexpected(std::move(room.error()));
  auto page = find_paged_messages(rq, room->id);
  if (!page) co_return std::unexpected(std::move(page.error()));
  if (page->empty()) co_return rq.head(204);

  // The ETag is the digest of the record keys with their versions, as `fresh_when` makes it.
  Freshness freshness;
  std::string etag;
  for (const models::Message& message : *page) {
    const auto updated = parse_db(message.updated_at);
    if (!etag.empty()) etag.push_back('/');
    etag += "messages/" + std::to_string(message.id) + "-" + (updated ? format_cache_version(*updated) : "");
    if (updated && (!freshness.last_modified || *freshness.last_modified < *updated)) freshness.last_modified = updated;
  }
  freshness.etag = std::move(etag);
  freshness.template_digest = "messages/index";
  if (auto not_modified = rq.fresh_when(freshness)) co_return std::move(*not_modified);

  const req::Format offered[] = {&req::mime::HTML};
  if (auto format = rq.respond_to(offered); !format) co_return std::unexpected(std::move(format.error()));
  deps.facet("page", "messages#index");
  const views::ViewContext ctx = partial_context(rq);
  co_return cached_page_checked(
      rq, 200, deps,
      [&](Out& out) -> Flow<void> {
        auto items = message_items(rq, *page);
        if (!items) return std::unexpected(std::move(items.error()));
        views::messages::index(out, ctx, *items);
        return {};
      },
      false);
}

// `RoomScoped`, `set_last_updated_at`: what changed in the room since the client loaded it.
Task<Flow<net::Response>> refreshes_show(Rq& rq) {
  auto before = co_await concerns::before_actions(rq, concerns::Before{});
  if (!before) co_return std::unexpected(std::move(before.error()));
  auto room = messages::set_room(rq);
  if (!room) co_return std::unexpected(std::move(room.error()));
  auto since = since_param(rq);
  if (!since) co_return std::unexpected(std::move(since.error()));
  const req::Format offered[] = {&req::mime::TURBO_STREAM};
  if (auto format = rq.respond_to(offered); !format) co_return std::unexpected(std::move(format.error()));

  auto created = models::messages::page_created_since(rq.db(), rq.arena(), room->id, *since);
  if (!created) co_return messages::db_failure(created.error());
  auto updated = models::messages::page_updated_since_without_new(rq.db(), rq.arena(), room->id, *since);
  if (!updated) co_return messages::db_failure(updated.error());
  views::rooms::RefreshView refresh;
  refresh.room_id = room->id;
  refresh.kind = kind_of(room->type);
  auto new_items = message_items(rq, *created);
  if (!new_items) co_return std::unexpected(std::move(new_items.error()));
  refresh.new_messages = std::move(*new_items);
  auto updated_items = message_items(rq, *updated);
  if (!updated_items) co_return std::unexpected(std::move(updated_items.error()));
  refresh.updated_messages = std::move(*updated_items);
  const views::ViewContext ctx = partial_context(rq);
  Out out(rq.ctx.resource());
  views::rooms::refreshes::show_turbo_stream(out, ctx, refresh);
  co_return rq.turbo_stream(std::move(out));
}

}  // namespace campfire::app::controllers

namespace campfire::routes::messages_controller {

Task<net::Response> index(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::messages_index);
}

}  // namespace campfire::routes::messages_controller

namespace campfire::routes::rooms_refreshes {

Task<net::Response> show(net::Ctx& c) {
  return app::dispatch(c, &app::controllers::refreshes_show);
}

}  // namespace campfire::routes::rooms_refreshes
